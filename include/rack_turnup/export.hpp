// Rack Turnup Manager - public export macro.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

// Rack Turnup Manager builds as a static library by default. When it is built as
// a shared library the definition RACK_TURNUP_SHARED is set for consumers of the
// exported CMake target, and RACK_TURNUP_BUILDING is set while building the
// library itself.

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(RACK_TURNUP_SHARED)
#if defined(RACK_TURNUP_BUILDING)
#define RACK_TURNUP_API __declspec(dllexport)
#else
#define RACK_TURNUP_API __declspec(dllimport)
#endif
#else
#define RACK_TURNUP_API
#endif
#else
#if defined(RACK_TURNUP_SHARED) && defined(RACK_TURNUP_BUILDING)
#define RACK_TURNUP_API __attribute__((visibility("default")))
#else
#define RACK_TURNUP_API
#endif
#endif
