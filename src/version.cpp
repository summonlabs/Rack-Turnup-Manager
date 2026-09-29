// Rack Turnup Manager - version and build provenance.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/version.hpp"

// The build system supplies these three strings so that the compiled library
// reports exactly what was built, not what a source file claims. The fallbacks
// exist so that the library still compiles when it is embedded in a build that
// does not set them, and they are deliberately explicit about being unknown.
#ifndef RACK_TURNUP_VERSION_STRING
#define RACK_TURNUP_VERSION_STRING "0.0.0-unknown"
#endif

#ifndef RACK_TURNUP_BUILD_CONFIGURATION
#define RACK_TURNUP_BUILD_CONFIGURATION "unknown"
#endif

#ifndef RACK_TURNUP_BUILD_COMPILER
#define RACK_TURNUP_BUILD_COMPILER "unknown"
#endif

namespace rackturnup {
namespace {

constexpr std::string_view kVersion = RACK_TURNUP_VERSION_STRING;
constexpr std::string_view kConfiguration = RACK_TURNUP_BUILD_CONFIGURATION;
constexpr std::string_view kCompiler = RACK_TURNUP_BUILD_COMPILER;

}  // namespace

std::string_view version_string() noexcept { return kVersion; }

std::string_view build_configuration() noexcept { return kConfiguration; }

std::string_view build_compiler() noexcept { return kCompiler; }

}  // namespace rackturnup
