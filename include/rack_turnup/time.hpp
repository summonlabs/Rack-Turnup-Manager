// Rack Turnup Manager - explicit time, freshness and injection.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "rack_turnup/export.hpp"
#include "rack_turnup/limits.hpp"
#include "rack_turnup/result.hpp"

namespace rackturnup {

// Time in this library is never implicit.
//
// * `WallClock` is Unix epoch milliseconds. It is what evidence timestamps are
//   expressed in, because evidence is imported from other systems and must be
//   comparable across processes and restarts.
// * Freshness is a property of evidence, not of a cache: evidence carries the
//   interval over which it is considered to describe the world, and a
//   readiness verdict is computed against an authority time supplied by the
//   caller. Nothing consults the host clock behind the caller's back, so a
//   readiness answer is reproducible from its inputs.
// * There is no default "now". A caller that wants the current time asks for
//   it explicitly through `WallClock::now()`, and the CLI makes that visible.

class WallClock final {
 public:
  static constexpr std::int64_t kMillisecondsPerSecond = 1000;
  static constexpr std::int64_t kMillisecondsPerMinute = 60 * kMillisecondsPerSecond;
  static constexpr std::int64_t kMillisecondsPerHour = 60 * kMillisecondsPerMinute;
  static constexpr std::int64_t kMillisecondsPerDay = 24 * kMillisecondsPerHour;

  constexpr WallClock() noexcept = default;

  // Milliseconds since the Unix epoch. Negative values (before 1970) are
  // representable and representable only; they are not a sentinel.
  constexpr explicit WallClock(std::int64_t unix_milliseconds) noexcept
      : ms_(unix_milliseconds) {}

  [[nodiscard]] static WallClock now() noexcept;
  [[nodiscard]] static Result<WallClock> from_unix_milliseconds(std::int64_t value) {
    return WallClock{value};
  }

  [[nodiscard]] constexpr std::int64_t unix_milliseconds() const noexcept { return ms_; }

  // RFC 3339 in UTC with millisecond precision, for example
  // "2026-02-11T08:15:00.000Z". Canonical text form used by the CLI and by the
  // durable format.
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] static Result<WallClock> parse(std::string_view text);

  [[nodiscard]] constexpr bool operator==(const WallClock& other) const noexcept {
    return ms_ == other.ms_;
  }
  [[nodiscard]] constexpr bool operator!=(const WallClock& other) const noexcept {
    return ms_ != other.ms_;
  }
  [[nodiscard]] constexpr bool operator<(const WallClock& other) const noexcept {
    return ms_ < other.ms_;
  }
  [[nodiscard]] constexpr bool operator<=(const WallClock& other) const noexcept {
    return ms_ <= other.ms_;
  }
  [[nodiscard]] constexpr bool operator>(const WallClock& other) const noexcept {
    return ms_ > other.ms_;
  }
  [[nodiscard]] constexpr bool operator>=(const WallClock& other) const noexcept {
    return ms_ >= other.ms_;
  }

 private:
  std::int64_t ms_ = 0;
};

// A non-negative duration in milliseconds with an explicit upper bound. The
// bound exists because durations are multiplied, added and compared against
// epoch milliseconds, and an unbounded value would make that arithmetic
// overflow-detecting rather than total.
class Millis final {
 public:
  constexpr Millis() noexcept = default;
  constexpr explicit Millis(std::int64_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<Millis> create(std::int64_t value) {
    if (value < 0) {
      return make_error(ErrorCode::InvalidDuration, "duration must not be negative",
                        ErrorDetail{.operation = "Millis", .expected = 0,
                                    .actual = static_cast<std::uint64_t>(value)});
    }
    if (value > kMaxDurationMilliseconds) {
      return make_error(ErrorCode::InvalidDuration, "duration exceeds the supported bound",
                        ErrorDetail{.operation = "Millis",
                                    .expected = static_cast<std::uint64_t>(kMaxDurationMilliseconds),
                                    .actual = static_cast<std::uint64_t>(value)});
    }
    return Millis{value};
  }

  [[nodiscard]] static Millis seconds(std::int64_t value) noexcept {
    return Millis{value * WallClock::kMillisecondsPerSecond};
  }
  [[nodiscard]] static Millis minutes(std::int64_t value) noexcept {
    return Millis{value * WallClock::kMillisecondsPerMinute};
  }
  [[nodiscard]] static Millis hours(std::int64_t value) noexcept {
    return Millis{value * WallClock::kMillisecondsPerHour};
  }
  [[nodiscard]] static Millis days(std::int64_t value) noexcept {
    return Millis{value * WallClock::kMillisecondsPerDay};
  }

  [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept { return value_; }
  [[nodiscard]] constexpr std::int64_t seconds_rounded_up() const noexcept {
    return (value_ + 999) / 1000;
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] constexpr bool operator==(const Millis& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const Millis& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const Millis& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const Millis& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const Millis& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const Millis& other) const noexcept {
    return value_ >= other.value_;
  }

 private:
  std::int64_t value_ = 0;
};

// Parses a duration with an explicit unit suffix. Accepted suffixes are
// "ms", "s", "m", "h" and "d"; text without a suffix is milliseconds.
// Negative values, whitespace, unknown units, an empty numeric part and
// values above kMaxDurationMilliseconds are rejected, so a duration that
// parses is always safe to add to a timestamp.
[[nodiscard]] RACK_TURNUP_API Result<Millis> parse_duration(std::string_view text);

// The result of comparing an evidence record's validity window against an
// authority time. "Unknown" is not representable here: an evidence record that
// exists always has a validity window, and the window is either satisfied or
// not. Missing evidence is a different condition and is reported by
// ReadinessState::Unknown.
enum class FreshnessVerdict : std::uint8_t {
  Fresh = 0,
  Expired = 1,
  // The observation time lies in the future relative to the authority time, so
  // the evidence cannot yet describe the world the verdict is about. This is
  // rejected rather than clamped.
  NotYetObserved = 2,
};

[[nodiscard]] RACK_TURNUP_API std::string_view freshness_verdict_name(FreshnessVerdict verdict) noexcept;

// Evaluates `observed_at + validity` against `authority_time`. `validity` of
// zero means the evidence is valid only at the exact instant it was observed.
[[nodiscard]] RACK_TURNUP_API FreshnessVerdict evaluate_freshness(WallClock observed_at,
                                                                 Millis validity,
                                                                 WallClock authority_time) noexcept;

// Age of an observation at an authority time. Returns the signed difference in
// milliseconds; callers that require a non-negative age must check the verdict
// first. Saturating: the difference of two representable epoch millisecond
// values is always representable in std::int64_t.
[[nodiscard]] RACK_TURNUP_API std::int64_t age_milliseconds(WallClock observed_at,
                                                            WallClock authority_time) noexcept;

}  // namespace rackturnup
