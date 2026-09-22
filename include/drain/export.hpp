#pragma once

// Drain Fabric -- Summon Software Labs
// Copyright 2026 Summon Software Labs.
//
// Symbol visibility / linkage decoration for the public Drain Fabric API.
// The library is built static by default; shared builds define DRAINFABRIC_SHARED.

#if defined(_WIN32) && defined(DRAINFABRIC_SHARED)
#define DRAINFABRIC_API __declspec(dllexport)
#define DRAINFABRIC_API_IMPORT __declspec(dllimport)
#elif defined(_WIN32)
#define DRAINFABRIC_API
#define DRAINFABRIC_API_IMPORT
#elif defined(__GNUC__) || defined(__clang__)
#define DRAINFABRIC_API __attribute__((visibility("default")))
#define DRAINFABRIC_API_IMPORT __attribute__((visibility("default")))
#else
#define DRAINFABRIC_API
#define DRAINFABRIC_API_IMPORT
#endif

#define DRAIN_API DRAINFABRIC_API
