// Rack Turnup Manager - explicit wall-clock time, durations and freshness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/time.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr std::int64_t kMillisecondsPerMinute = 60 * kMillisecondsPerSecond;
constexpr std::int64_t kMillisecondsPerHour = 60 * kMillisecondsPerMinute;
constexpr std::int64_t kMillisecondsPerDay = 24 * kMillisecondsPerHour;

// Howard Hinnant's civil calendar algorithms. They are exact for the whole
// range of std::int64_t days and depend on nothing outside the standard
// library, so a timestamp never depends on the host locale or time zone.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month,
                     unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::uint64_t day_of_era = static_cast<std::uint64_t>(days - era * 146097);
  const std::uint64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(year_of_era) + era * 400;
  const std::uint64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::uint64_t mp = (5 * day_of_year + 2) / 153;
  const std::uint64_t d = day_of_year - (153 * mp + 2) / 5 + 1;
  const std::uint64_t m = (mp < 10) ? (mp + 3) : (mp - 9);
  year = y + (m <= 2 ? 1 : 0);
  month = static_cast<unsigned>(m);
  day = static_cast<unsigned>(d);
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= (month <= 2) ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned month_prime = (month > 2) ? (month - 3) : (month + 9);
  const unsigned day_of_year = (153 * month_prime + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

[[nodiscard]] bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  constexpr unsigned kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kLengths[month - 1];
}

void append_padded(std::string& out, std::uint64_t value, std::size_t width) {
  char digits[24];
  std::size_t length = 0;
  do {
    digits[length] = static_cast<char>('0' + (value % 10u));
    value /= 10u;
    ++length;
  } while (value != 0);
  while (length < width) {
    digits[length] = '0';
    ++length;
  }
  while (length > 0) {
    --length;
    out.push_back(digits[length]);
  }
}

[[nodiscard]] bool digits_at(std::string_view text, std::size_t offset, std::size_t count,
                             std::uint64_t& out) noexcept {
  if (offset + count > text.size()) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (!ascii_digit(c)) {
      return false;
    }
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

[[nodiscard]] Result<std::int64_t> parse_duration_value(std::string_view text,
                                                       std::int64_t multiplier) {
  std::int64_t magnitude = 0;
  if (!parse_i64(text, magnitude)) {
    return make_error(ErrorCode::InvalidDuration, "duration is not a decimal integer",
                      ErrorDetail{.operation = "parse_duration", .subject = std::string(text)});
  }
  if (magnitude < 0) {
    return make_error(ErrorCode::InvalidDuration, "duration must not be negative",
                      ErrorDetail{.operation = "parse_duration", .subject = std::string(text)});
  }
  const std::int64_t limit = kMaxDurationMilliseconds / multiplier;
  if (magnitude > limit) {
    return make_error(ErrorCode::InvalidDuration, "duration exceeds the supported bound",
                      ErrorDetail{.operation = "parse_duration",
                                  .expected = static_cast<std::uint64_t>(kMaxDurationMilliseconds),
                                  .actual = static_cast<std::uint64_t>(magnitude * multiplier)});
  }
  return magnitude * multiplier;
}

}  // namespace

WallClock WallClock::now() noexcept {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  return WallClock{static_cast<std::int64_t>(milliseconds)};
}

std::string WallClock::to_text() const {
  std::int64_t days = ms_ / kMillisecondsPerDay;
  std::int64_t remainder = ms_ % kMillisecondsPerDay;
  if (remainder < 0) {
    remainder += kMillisecondsPerDay;
    --days;
  }
  std::int64_t year = 0;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);

  const std::uint64_t hour = static_cast<std::uint64_t>(remainder / kMillisecondsPerHour);
  const std::uint64_t minute =
      static_cast<std::uint64_t>((remainder % kMillisecondsPerHour) / kMillisecondsPerMinute);
  const std::uint64_t second =
      static_cast<std::uint64_t>((remainder % kMillisecondsPerMinute) / kMillisecondsPerSecond);
  const std::uint64_t millisecond = static_cast<std::uint64_t>(remainder % kMillisecondsPerSecond);

  std::string out;
  out.reserve(24);
  append_padded(out, static_cast<std::uint64_t>(year), 4);
  out.push_back('-');
  append_padded(out, month, 2);
  out.push_back('-');
  append_padded(out, day, 2);
  out.push_back('T');
  append_padded(out, hour, 2);
  out.push_back(':');
  append_padded(out, minute, 2);
  out.push_back(':');
  append_padded(out, second, 2);
  out.push_back('.');
  append_padded(out, millisecond, 3);
  out.push_back('Z');
  return out;
}

Result<WallClock> WallClock::parse(std::string_view text) {
  const auto reject = [&](std::string message) {
    return make_error(ErrorCode::InvalidTimestamp, std::move(message),
                      ErrorDetail{.operation = "WallClock::parse", .subject = std::string(text)});
  };

  if (text.size() < 20) {
    return reject("timestamp must be at least 20 characters in RFC 3339 UTC form");
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return reject("timestamp separators must be exactly YYYY-MM-DDTHH:MM:SS");
  }

  std::uint64_t year = 0;
  std::uint64_t month = 0;
  std::uint64_t day = 0;
  std::uint64_t hour = 0;
  std::uint64_t minute = 0;
  std::uint64_t second = 0;
  if (!digits_at(text, 0, 4, year) || !digits_at(text, 5, 2, month) || !digits_at(text, 8, 2, day) ||
      !digits_at(text, 11, 2, hour) || !digits_at(text, 14, 2, minute) ||
      !digits_at(text, 17, 2, second)) {
    return reject("timestamp fields must be digits in YYYY-MM-DDTHH:MM:SS order");
  }

  std::uint64_t millisecond = 0;
  std::size_t offset = 19;
  if (text[offset] == '.') {
    ++offset;
    std::size_t fraction_digits = 0;
    std::uint64_t fraction = 0;
    while (offset < text.size() && fraction_digits < 3) {
      const char c = text[offset];
      if (!ascii_digit(c)) {
        break;
      }
      fraction = fraction * 10u + static_cast<std::uint64_t>(c - '0');
      ++fraction_digits;
      ++offset;
    }
    if (fraction_digits == 0) {
      return reject("fractional seconds must contain at least one digit");
    }
    if (fraction_digits == 1) {
      fraction *= 100u;
    } else if (fraction_digits == 2) {
      fraction *= 10u;
    }
    millisecond = fraction;
    if (offset < text.size() && ascii_digit(text[offset])) {
      return reject("fractional seconds support at most three digits");
    }
  }
  if (offset + 1 != text.size() || text[offset] != 'Z') {
    return reject("timestamp must end with an uppercase Z and no other offset");
  }
  if (year == 0) {
    return reject("timestamp year must be between 0001 and 9999");
  }
  if (month < 1 || month > 12) {
    return reject("timestamp month must be between 01 and 12");
  }
  if (day < 1 || day > days_in_month(static_cast<std::int64_t>(year), static_cast<unsigned>(month))) {
    return reject("timestamp day is outside the month");
  }
  if (hour > 23) {
    return reject("timestamp hour must be between 00 and 23");
  }
  if (minute > 59) {
    return reject("timestamp minute must be between 00 and 59");
  }
  if (second > 59) {
    // A leap second has no distinct representation here, so it is rejected
    // rather than folded into the following second.
    return reject("timestamp second must be between 00 and 59");
  }

  const std::int64_t days =
      days_from_civil(static_cast<std::int64_t>(year), static_cast<unsigned>(month),
                      static_cast<unsigned>(day));
  const std::int64_t total =
      days * kMillisecondsPerDay + static_cast<std::int64_t>(hour) * kMillisecondsPerHour +
      static_cast<std::int64_t>(minute) * kMillisecondsPerMinute +
      static_cast<std::int64_t>(second) * kMillisecondsPerSecond +
      static_cast<std::int64_t>(millisecond);
  return WallClock{total};
}

std::string Millis::to_text() const { return std::to_string(value_) + "ms"; }

Result<Millis> parse_duration(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::InvalidDuration, "duration must not be empty",
                      ErrorDetail{.operation = "parse_duration"});
  }
  std::string_view digits = text;
  std::int64_t multiplier = 1;
  if (text.size() > 2 && text.substr(text.size() - 2) == "ms") {
    digits = text.substr(0, text.size() - 2);
    multiplier = 1;
  } else if (text.size() > 1) {
    const char unit = text.back();
    if (unit == 's') {
      multiplier = kMillisecondsPerSecond;
    } else if (unit == 'm') {
      multiplier = kMillisecondsPerMinute;
    } else if (unit == 'h') {
      multiplier = kMillisecondsPerHour;
    } else if (unit == 'd') {
      multiplier = kMillisecondsPerDay;
    } else {
      return make_error(ErrorCode::InvalidDuration,
                        "duration unit must be one of ms, s, m, h or d",
                        ErrorDetail{.operation = "parse_duration", .subject = std::string(text)});
    }
    digits = text.substr(0, text.size() - 1);
  }
  if (digits.empty()) {
    return make_error(ErrorCode::InvalidDuration, "duration has no numeric part",
                      ErrorDetail{.operation = "parse_duration", .subject = std::string(text)});
  }
  const Result<std::int64_t> value = parse_duration_value(digits, multiplier);
  if (!value.has_value()) {
    return value.error();
  }
  return Millis::create(value.value());
}

std::string_view freshness_verdict_name(FreshnessVerdict verdict) noexcept {
  switch (verdict) {
    case FreshnessVerdict::Fresh:
      return "fresh";
    case FreshnessVerdict::Expired:
      return "expired";
    case FreshnessVerdict::NotYetObserved:
      return "not_yet_observed";
  }
  return "unknown";
}

FreshnessVerdict evaluate_freshness(WallClock observed_at, Millis validity,
                                    WallClock authority_time) noexcept {
  const std::int64_t observed = observed_at.unix_milliseconds();
  const std::int64_t authority = authority_time.unix_milliseconds();
  if (observed > authority) {
    return FreshnessVerdict::NotYetObserved;
  }
  const std::int64_t window = validity.milliseconds();
  if (observed > std::numeric_limits<std::int64_t>::max() - window) {
    // The validity window cannot overflow, so the evidence never expires
    // within representable time.
    return FreshnessVerdict::Fresh;
  }
  return (observed + window < authority) ? FreshnessVerdict::Expired : FreshnessVerdict::Fresh;
}

std::int64_t age_milliseconds(WallClock observed_at, WallClock authority_time) noexcept {
  const std::int64_t observed = observed_at.unix_milliseconds();
  const std::int64_t authority = authority_time.unix_milliseconds();
  if (authority >= observed) {
    const std::uint64_t difference = static_cast<std::uint64_t>(authority) - static_cast<std::uint64_t>(observed);
    if (difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(difference);
  }
  return -age_milliseconds(authority_time, observed_at);
}

}  // namespace rackturnup
