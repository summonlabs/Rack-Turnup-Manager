// Rack Turnup Manager - text validation and canonical ordering helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/export.hpp"

namespace rackturnup {

// Strict UTF-8 validation: rejects overlong encodings, surrogate halves, values
// above U+10FFFF, truncated sequences and the raw bytes 0xC0, 0xC1 and
// 0xF5..0xFF. Text accepted by this function round-trips byte for byte.
[[nodiscard]] RACK_TURNUP_API bool is_valid_utf8(std::string_view text) noexcept;

// True when the text contains no Unicode control characters (C0 and C1 ranges,
// plus DEL). Requires valid UTF-8; invalid UTF-8 returns false.
[[nodiscard]] RACK_TURNUP_API bool has_no_control_characters(std::string_view text) noexcept;

// Byte-wise ordering. All public and serialized ordering in this library is
// byte-wise over validated text, never locale dependent.
[[nodiscard]] RACK_TURNUP_API bool byte_less(std::string_view left,
                                             std::string_view right) noexcept;

[[nodiscard]] constexpr bool ascii_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr bool ascii_lower_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr bool ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool ascii_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

[[nodiscard]] constexpr char ascii_to_lower(char c) noexcept {
  return ascii_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when every byte satisfies `predicate`.
template <typename Predicate>
[[nodiscard]] bool all_bytes(std::string_view text, Predicate predicate) noexcept {
  for (const char c : text) {
    if (!predicate(c)) {
      return false;
    }
  }
  return true;
}

// Lowercase hexadecimal encoding of a byte string. Used for digests and for the
// canonical text form of lock records.
[[nodiscard]] RACK_TURNUP_API std::string to_hex(const std::uint8_t* data, std::size_t size);
[[nodiscard]] RACK_TURNUP_API std::string to_hex(const std::vector<std::uint8_t>& data);

// Splits on a single ASCII delimiter. Empty fields are preserved so that a
// malformed record is rejected rather than silently shortened.
[[nodiscard]] RACK_TURNUP_API std::vector<std::string> split(std::string_view text, char delimiter);

// Joins with a delimiter, inserting nothing before the first element.
[[nodiscard]] RACK_TURNUP_API std::string join(const std::vector<std::string>& parts, char delimiter);

// Sorts and removes duplicates using byte-wise ordering, returning true when
// the input already satisfied "sorted and unique".
[[nodiscard]] RACK_TURNUP_API bool canonicalize(std::vector<std::string>& values);

// Parses a non-negative decimal integer. Rejects empty text, signs, whitespace,
// non-digits, and any value that does not fit in std::uint64_t. There is no
// partial parse: trailing characters are an error.
[[nodiscard]] RACK_TURNUP_API bool parse_u64(std::string_view text, std::uint64_t& out) noexcept;

// Parses a signed decimal integer with an optional single leading '-'. Rejects
// empty text, "+1", whitespace, non-digits and overflow.
[[nodiscard]] RACK_TURNUP_API bool parse_i64(std::string_view text, std::int64_t& out) noexcept;

// Splits a command line style argument list. Supports double quotes, single
// quotes, and backslash escaping of the quote character. An unterminated quote
// is an error, not a silently truncated token.
[[nodiscard]] RACK_TURNUP_API bool split_arguments(std::string_view text,
                                                   std::vector<std::string>& out);

}  // namespace rackturnup
