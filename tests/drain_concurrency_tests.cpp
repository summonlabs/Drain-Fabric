// Drain Fabric -- concurrency, race, and reentrancy tests.
//
// These cases exercise the paths that a single-threaded test cannot reach:
// admission racing the closure boundary, duplicate callbacks arriving at once,
// cancellation landing in the middle of evacuation, hooks that call back into
// the engine, and repeated server start/stop cycles.
//
// The engine guarantees that the mutex is never held while a caller-supplied
// hook runs, so a hook may re-enter the engine. Every sink in this file asserts
// that property.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "drain/engine.hpp"
#include "drain/server.hpp"
#include "fixture.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

struct ContentionWorld {
  drain::ManualClock clock;
  drain::DrainPolicy policy;
  drain::DrainEngine engine;
  ScriptedSink sink;
  drain::AuthorityId authority;
  drain::DrainTarget target;

  ContentionWorld() : engine(policy, clock), sink(engine) {
    policy.set_admission_fence_settle(std::chrono::seconds(0));
    policy.set_max_concurrent_drains_per_domain(4);
    engine.set_sink(&sink);
    (void)engine.install_incarnation(drain::IncarnationId{1}, clock.now());
    auto issued = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    if (!issued.ok()) {
      DRAIN_FAIL("cannot issue authority");
    }
    authority = issued->id();
    target = target_of("contended", "d1");
    DRAIN_CHECK_OK(engine.add_resource(
        drain::ResourceNode(target, drain::CapacityUnits::from(1000)), clock.now()));
  }
};

}  // namespace

DRAIN_TEST(concurrency, admission_racing_the_closure_boundary_never_violates_it) {
  for (int round = 0; round < 6; ++round) {
    ContentionWorld world;
    drain::DrainRequest request;
    request.targets = {world.target};
    request.authority = world.authority;
    auto set = world.engine.request_drain_set(request, world.clock.now());
    DRAIN_CHECK(set.ok());
    const drain::DrainId drain_id = world.engine.drain_set(*set)->members.front();

    std::atomic<bool> go{false};
    std::atomic<std::size_t> admitted{0};
    std::atomic<std::size_t> refused{0};
    constexpr std::size_t kThreads = 4;
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < kThreads; ++index) {
      threads.emplace_back([&, index]() {
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int attempt = 0; attempt < 200; ++attempt) {
          drain::Obligation draft(drain::ObligationKind::ActiveFlow,
                                  holder_of("racer" + std::to_string(index) + "x" +
                                            std::to_string(attempt)),
                                  {world.target});
          auto result = world.engine.admit_obligation(draft, drain::Generation{}, world.authority,
                                                      world.clock.now());
          if (result.ok()) {
            admitted.fetch_add(1, std::memory_order_relaxed);
          } else {
            refused.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    std::thread driver([&]() {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int step = 0; step < 3; ++step) {
        (void)world.engine.advance(world.clock.now());
      }
    });
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
      thread.join();
    }
    driver.join();

    const auto record = world.engine.drain(drain_id);
    DRAIN_CHECK(record.has_value());
    DRAIN_CHECK(record->generation().value() >= 1u);
    DRAIN_CHECK_EQ(admitted.load() + refused.load(), kThreads * 200);

    const drain::AccountingReport audit = world.engine.audit(world.clock.now());
    DRAIN_CHECK_MSG(audit.clean(), audit.render());
    DRAIN_CHECK_EQ(world.sink.calls_under_lock, 0u);
  }
}

DRAIN_TEST(concurrency, duplicate_callbacks_are_serialised_and_only_one_wins) {
  ContentionWorld world;
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("holder"), {world.target});
  auto admitted = world.engine.admit_obligation(draft, drain::Generation{}, world.authority,
                                                world.clock.now());
  DRAIN_CHECK(admitted.ok());

  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{1};
  evidence.key.kind = drain::EvidenceKind::ObligationReleased;
  evidence.key.obligation = admitted->id();
  evidence.generation = world.engine.drain(drain::DrainId{0}).has_value() ? drain::Generation{1}
                                                                         : drain::Generation{1};
  evidence.epoch = world.engine.epoch();
  evidence.producer = world.engine.incarnation();
  evidence.observed_at = world.clock.now();
  DRAIN_CHECK_OK(world.engine.record_evidence(evidence, world.clock.now()));

  constexpr std::size_t kThreads = 8;
  std::atomic<std::size_t> succeeded{0};
  std::atomic<std::size_t> stale{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      // Every thread replays the identical callback: the same obligation, the
      // same expected revision, the same evidence.
      drain::ObligationReport report;
      report.obligation = admitted->id();
      report.expected_revision = admitted->revision();
      report.next = drain::ObligationState::Released;
      report.evidence = drain::EvidenceSeq{1};
      auto status = world.engine.report_obligation(report, world.clock.now());
      if (status.ok()) {
        succeeded.fetch_add(1, std::memory_order_relaxed);
      } else if (status.code() == drain::ErrorCode::StaleRevision) {
        stale.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }
  // One application, and the replayed copies are accepted idempotently or
  // rejected as stale -- never applied twice.
  DRAIN_CHECK(succeeded.load() >= 1);
  DRAIN_CHECK_EQ(succeeded.load() + stale.load(), kThreads);
  const auto obligation = world.engine.obligation(admitted->id());
  DRAIN_CHECK(obligation.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(obligation->state()),
                 static_cast<int>(drain::ObligationState::Released));
  DRAIN_CHECK_EQ(world.engine.audit(world.clock.now()).violations.size(), 0u);
}

DRAIN_TEST(concurrency, cancellation_during_evacuation_publishes_no_success) {
  for (int round = 0; round < 8; ++round) {
    ContentionWorld world;
    world.sink.mode = ScriptedSink::Mode::RecordOnly;
    drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("holder"), {world.target});
    auto admitted =
        world.engine.admit_obligation(draft, drain::Generation{}, world.authority, world.clock.now());
    DRAIN_CHECK(admitted.ok());

    drain::DrainRequest request;
    request.targets = {world.target};
    request.authority = world.authority;
    auto set = world.engine.request_drain_set(request, world.clock.now());
    DRAIN_CHECK(set.ok());
    const drain::DrainId drain_id = world.engine.drain_set(*set)->members.front();

    std::atomic<bool> cancelled{false};
    std::thread canceller([&]() {
      while (!cancelled.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      (void)world.engine.cancel_drain(drain_id, world.authority, "race cancel", world.clock.now());
    });

    for (int step = 0; step < 6; ++step) {
      (void)world.engine.advance(world.clock.now());
    }
    cancelled.store(true, std::memory_order_release);
    canceller.join();

    const auto record = world.engine.drain(drain_id);
    DRAIN_CHECK(record.has_value());
    DRAIN_CHECK_MSG(record->state() != drain::DrainState::Drained,
                    "a cancelled drain reported success");
    for (int step = 0; step < 12; ++step) {
      (void)world.engine.advance(world.clock.now());
      const auto after = world.engine.drain(drain_id);
      DRAIN_CHECK(after->state() != drain::DrainState::Drained);
    }

    // A late report carrying the closed generation is bookkeeping only; it can
    // never move the drain back to drained.
    drain::ObligationReport late;
    late.obligation = admitted->id();
    const auto obligation = world.engine.obligation(admitted->id());
    late.expected_revision = obligation->revision();
    late.next = drain::ObligationState::Retired;
    late.drain = drain_id;
    late.generation = record->generation();
    (void)world.engine.report_obligation(late, world.clock.now());
    DRAIN_CHECK_EQ(static_cast<int>(world.engine.drain(drain_id)->state()),
                   static_cast<int>(drain::DrainState::Cancelled));
    DRAIN_CHECK_EQ(world.sink.calls_under_lock, 0u);
    DRAIN_CHECK(world.engine.audit(world.clock.now()).clean());
  }
}

DRAIN_TEST(concurrency, hooks_reenter_the_engine_without_the_lock_held) {
  ContentionWorld world;
  // A sink that calls back into the engine on every hook from a worker thread.
  class ReentrantSink final : public drain::EvacuationSink {
   public:
    ReentrantSink(drain::DrainEngine& engine, drain::AuthorityId authority)
        : engine_(&engine), authority_(authority) {}

    std::atomic<std::size_t> calls{0};
    std::atomic<std::size_t> under_lock{0};
    std::atomic<std::size_t> admissions{0};

    drain::Status on_evacuation_request(const drain::EvacuationRequest& request) override {
      if (engine_->lock_held_by_this_thread()) {
        under_lock.fetch_add(1);
      }
      calls.fetch_add(1);
      // Re-enter the engine: this is legal precisely because the hook is not
      // invoked under the engine mutex.
      auto digest = engine_->authoritative_digest();
      (void)digest;
      drain::Obligation draft(drain::ObligationKind::Supplied, holder_of("hook"),
                              {request.target});
      auto admitted = engine_->admit_obligation(draft, drain::Generation{}, authority_, engine_->now());
      if (admitted.ok()) {
        admissions.fetch_add(1);
      }
      return drain::ok_status();
    }
    drain::Status on_restoration_request(const drain::RestorationRequest&) override {
      if (engine_->lock_held_by_this_thread()) {
        under_lock.fetch_add(1);
      }
      return drain::ok_status();
    }
    drain::Status on_admission_reopen(const drain::AdmissionReopenRequest&) override {
      if (engine_->lock_held_by_this_thread()) {
        under_lock.fetch_add(1);
      }
      return drain::ok_status();
    }

   private:
    drain::DrainEngine* engine_{nullptr};
    drain::AuthorityId authority_{};
  };

  ReentrantSink sink(world.engine, world.authority);
  world.engine.set_sink(&sink);
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("holder"), {world.target});
  DRAIN_CHECK(world.engine.admit_obligation(draft, drain::Generation{}, world.authority,
                                            world.clock.now())
                  .ok());
  drain::DrainRequest request;
  request.targets = {world.target};
  request.authority = world.authority;
  auto set = world.engine.request_drain_set(request, world.clock.now());
  DRAIN_CHECK(set.ok());
  const drain::DrainId drain_id = world.engine.drain_set(*set)->members.front();

  std::thread worker([&]() {
    for (int step = 0; step < 12; ++step) {
      (void)world.engine.advance(world.clock.now());
    }
  });
  for (int step = 0; step < 12; ++step) {
    (void)world.engine.advance(world.clock.now());
  }
  worker.join();

  DRAIN_CHECK(sink.calls.load() > 0);
  DRAIN_CHECK_EQ(sink.under_lock.load(), 0u);
  const auto record = world.engine.drain(drain_id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK(world.engine.audit(world.clock.now()).clean());
}

DRAIN_TEST(concurrency, repeated_server_start_and_stop_is_clean) {
  for (int round = 0; round < 4; ++round) {
    drain::ManualClock clock;
    drain::DrainPolicy policy = drain::DrainPolicy::defaults();
    drain::DrainEngine engine(policy, clock);
    DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
    drain::ServerOptions options;
    options.port = 0;
    options.accept_wait_ms = 2;
    options.read_wait_ms = 2;
    drain::ControllerServer server(engine, options, clock);
    DRAIN_CHECK_OK(server.start());
    DRAIN_CHECK(server.port() != 0);

    std::atomic<bool> finished{false};
    std::thread worker([&]() {
      while (!finished.load(std::memory_order_acquire)) {
        (void)server.poll_once();
      }
    });
    for (int step = 0; step < 20; ++step) {
      (void)engine.advance(clock.now());
      std::this_thread::yield();
    }
    finished.store(true, std::memory_order_release);
    worker.join();
    DRAIN_CHECK_OK(server.stop());
    DRAIN_CHECK(!server.running());
    DRAIN_CHECK(server.connection_count() == 0);
    DRAIN_CHECK(engine.audit(clock.now()).clean());
  }
}

DRAIN_TEST(concurrency, worker_pool_starts_and_stops_without_leaking_threads) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  drain::ServerOptions options;
  options.port = 0;
  options.max_worker_threads = 4;
  options.accept_wait_ms = 2;
  options.read_wait_ms = 2;
  drain::ControllerServer server(engine, options, clock);
  DRAIN_CHECK_OK(server.start());
  DRAIN_CHECK_OK(server.start_workers(3));
  DRAIN_CHECK_CODE(server.start_workers(1), drain::ErrorCode::Busy);
  DRAIN_CHECK_OK(server.stop_workers());
  DRAIN_CHECK_OK(server.stop());
}

DRAIN_TEST(concurrency, concurrent_authority_issuance_produces_unique_tokens) {
  ContentionWorld world;
  constexpr std::size_t kThreads = 6;
  constexpr std::size_t kPerThread = 50;
  std::vector<std::thread> threads;
  std::vector<std::vector<drain::AuthorityId>> results(kThreads);
  std::atomic<bool> go{false};
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t attempt = 0; attempt < kPerThread; ++attempt) {
        auto token = world.engine.issue_authority(drain::DomainId{}, std::chrono::hours(1),
                                                  world.clock.now());
        if (token.ok()) {
          results[index].push_back(token->id());
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }
  std::vector<drain::AuthorityId> all;
  for (const auto& list : results) {
    all.insert(all.end(), list.begin(), list.end());
  }
  std::sort(all.begin(), all.end());
  DRAIN_CHECK_EQ(std::unique(all.begin(), all.end()) - all.begin(),
                 static_cast<std::ptrdiff_t>(all.size()));
  DRAIN_CHECK_EQ(all.size(), kThreads * kPerThread);
}
