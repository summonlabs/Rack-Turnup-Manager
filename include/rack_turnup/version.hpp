// Rack Turnup Manager - version and format constants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "rack_turnup/export.hpp"

namespace rackturnup {

// Library version. The three components are the release contract for
// downstream consumers; CMake's package version file uses the same numbers.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// Version of the durable state file format written by this build. A reader
// rejects any other value with UnsupportedFormatVersion rather than guessing.
inline constexpr std::uint32_t kStateFormatVersion = 1;

// Version of the composition digest preimage layout. The composition digest is
// what fences turnup authority to an exact rack composition, so a change to the
// preimage layout is a change to the fencing contract and advances this number.
inline constexpr std::uint32_t kCompositionDigestModel = 1;

// Version of the verdict digest preimage layout used by turnup authorizations
// and activation records to prove they were issued against one exact decision.
inline constexpr std::uint32_t kVerdictDigestModel = 1;

// Version of the evidence-set digest preimage layout.
inline constexpr std::uint32_t kEvidenceSetDigestModel = 1;

[[nodiscard]] RACK_TURNUP_API std::string_view version_string() noexcept;

[[nodiscard]] RACK_TURNUP_API std::string_view build_configuration() noexcept;

// Compiler description captured at build time, for support and provenance.
[[nodiscard]] RACK_TURNUP_API std::string_view build_compiler() noexcept;

}  // namespace rackturnup
