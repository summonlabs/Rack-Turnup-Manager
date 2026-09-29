// Rack Turnup Manager - text validation and canonical ordering.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/text.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace rackturnup {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// Character code constants. The literal spellings of these characters are
// avoided so that the source stays readable in every editor and diff tool.
constexpr char kTab = 9;
constexpr char kDoubleQuote = 34;
constexpr char kSingleQuote = 39;
constexpr char kBackslash = 92;

// Decodes one UTF-8 sequence starting at `offset`. On success `offset` is
// advanced past the sequence and the code point is stored in `code_point`.
// Overlong encodings, surrogate halves, values above U+10FFFF, truncated
// sequences and the bytes 0xC0, 0xC1 and 0xF5..0xFF are rejected.
[[nodiscard]] bool decode_utf8(std::string_view text, std::size_t& offset,
                               std::uint32_t& code_point) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  const unsigned char lead = bytes[offset];

  if (lead < 0x80u) {
    code_point = lead;
    ++offset;
    return true;
  }

  std::size_t continuation_count = 0;
  std::uint32_t value = 0;
  if (lead >= 0xC2u && lead <= 0xDFu) {
    continuation_count = 1;
    value = lead & 0x1Fu;
  } else if (lead >= 0xE0u && lead <= 0xEFu) {
    continuation_count = 2;
    value = lead & 0x0Fu;
  } else if (lead >= 0xF0u && lead <= 0xF4u) {
    continuation_count = 3;
    value = lead & 0x07u;
  } else {
    return false;
  }

  if (offset + continuation_count >= size) {
    return false;
  }
  for (std::size_t i = 1; i <= continuation_count; ++i) {
    const unsigned char continuation = bytes[offset + i];
    if ((continuation & 0xC0u) != 0x80u) {
      return false;
    }
    value = (value << 6) | static_cast<std::uint32_t>(continuation & 0x3Fu);
  }

  if (continuation_count == 2 && (value < 0x800u || (value >= 0xD800u && value <= 0xDFFFu))) {
    return false;
  }
  if (continuation_count == 3 && (value < 0x10000u || value > 0x10FFFFu)) {
    return false;
  }

  code_point = value;
  offset += continuation_count + 1;
  return true;
}

[[nodiscard]] bool is_space(char c) noexcept { return c == ' ' || c == kTab; }

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t offset = 0;
  std::uint32_t code_point = 0;
  while (offset < text.size()) {
    if (!decode_utf8(text, offset, code_point)) {
      return false;
    }
  }
  return true;
}

bool has_no_control_characters(std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t code_point = 0;
    if (!decode_utf8(text, offset, code_point)) {
      // Invalid UTF-8 cannot be certified free of control characters.
      return false;
    }
    if (code_point < 0x20u || code_point == 0x7Fu || (code_point >= 0x80u && code_point <= 0x9Fu)) {
      return false;
    }
  }
  return true;
}

bool byte_less(std::string_view left, std::string_view right) noexcept {
  const std::size_t common = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto a = static_cast<unsigned char>(left[i]);
    const auto b = static_cast<unsigned char>(right[i]);
    if (a != b) {
      return a < b;
    }
  }
  return left.size() < right.size();
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve(size * 2u);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kHexDigits[(data[i] >> 4) & 0x0Fu]);
    out.push_back(kHexDigits[data[i] & 0x0Fu]);
  }
  return out;
}

std::string to_hex(const std::vector<std::uint8_t>& data) {
  return to_hex(data.data(), data.size());
}

std::vector<std::string> split(std::string_view text, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(delimiter, start);
    if (position == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, position - start));
    start = position + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, char delimiter) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.push_back(delimiter);
    }
    out.append(parts[i]);
  }
  return out;
}

bool canonicalize(std::vector<std::string>& values) {
  bool already_canonical = true;
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (!byte_less(values[i - 1], values[i])) {
      already_canonical = false;
      break;
    }
  }
  std::sort(values.begin(), values.end(), byte_less);
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return already_canonical;
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (!ascii_digit(c)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

bool parse_i64(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  }
  std::uint64_t magnitude = 0;
  for (const char c : text) {
    if (!ascii_digit(c)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    magnitude = magnitude * 10u + digit;
  }
  constexpr std::uint64_t kMinMagnitude =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
  if (negative) {
    if (magnitude > kMinMagnitude) {
      return false;
    }
    out = (magnitude == kMinMagnitude) ? std::numeric_limits<std::int64_t>::min()
                                       : -static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  out = static_cast<std::int64_t>(magnitude);
  return true;
}

bool split_arguments(std::string_view text, std::vector<std::string>& out) {
  out.clear();
  std::string current;
  bool in_token = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (is_space(c)) {
      if (in_token) {
        out.push_back(current);
        current.clear();
        in_token = false;
      }
      ++i;
      continue;
    }
    in_token = true;
    if (c == kDoubleQuote) {
      ++i;
      bool closed = false;
      while (i < text.size()) {
        const char inner = text[i];
        const bool escaped = inner == kBackslash && i + 1 < text.size() &&
                             (text[i + 1] == kDoubleQuote || text[i + 1] == kBackslash);
        if (escaped) {
          current.push_back(text[i + 1]);
          i += 2;
          continue;
        }
        if (inner == kDoubleQuote) {
          closed = true;
          ++i;
          break;
        }
        current.push_back(inner);
        ++i;
      }
      if (!closed) {
        out.clear();
        return false;
      }
      continue;
    }
    if (c == kSingleQuote) {
      ++i;
      bool closed = false;
      while (i < text.size()) {
        const char inner = text[i];
        if (inner == kSingleQuote) {
          closed = true;
          ++i;
          break;
        }
        current.push_back(inner);
        ++i;
      }
      if (!closed) {
        out.clear();
        return false;
      }
      continue;
    }
    const bool escaped = c == kBackslash && i + 1 < text.size() &&
                         (text[i + 1] == kDoubleQuote || text[i + 1] == kSingleQuote);
    if (escaped) {
      current.push_back(text[i + 1]);
      i += 2;
      continue;
    }
    current.push_back(c);
    ++i;
  }
  if (in_token) {
    out.push_back(current);
  }
  return true;
}

}  // namespace rackturnup
