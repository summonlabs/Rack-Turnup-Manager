// Rack Turnup Manager - content digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/export.hpp"
#include "rack_turnup/result.hpp"

namespace rackturnup {

// A 256-bit content digest. Digests are the fencing currency of this library:
// a turnup authorization is bound to the composition digest of the exact rack
// composition it was planned against, so any membership change fences it.
//
// The digest is SHA-256, computed over a canonical preimage defined by the
// digest model version in version.hpp. A digest is never compared as text
// without first being parsed into this type, and `is_zero()` means "absent",
// never "matches everything".
class Digest final {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() = default;

  // Parses 64 lowercase hexadecimal characters. Uppercase is rejected: the
  // canonical text form is lowercase, and accepting both would allow two text
  // forms of the same digest to compare unequal elsewhere.
  [[nodiscard]] static Result<Digest> parse(std::string_view text);

  [[nodiscard]] static Digest of(std::vector<std::uint8_t> bytes);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }

  [[nodiscard]] std::string to_hex() const;

  // A zero digest means the value was never computed. It is never treated as a
  // match.
  [[nodiscard]] bool is_zero() const noexcept;

  [[nodiscard]] bool operator==(const Digest& other) const noexcept { return bytes_ == other.bytes_; }
  [[nodiscard]] bool operator!=(const Digest& other) const noexcept { return !(*this == other); }
  [[nodiscard]] bool operator<(const Digest& other) const noexcept { return bytes_ < other.bytes_; }

 private:
  std::vector<std::uint8_t> bytes_ = std::vector<std::uint8_t>(kBytes, 0);
};

// Incremental SHA-256 so that large preimages never need to be materialized as
// one contiguous buffer. The digest produced by a sequence of updates is
// identical to hashing the concatenation.
class Sha256 final {
 public:
  Sha256() noexcept;

  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  // Big-endian fixed-width integer helpers. Little-endian host order is never
  // used in a preimage: a digest must not depend on the host.
  void update_u8(std::uint8_t value) noexcept;
  void update_u16(std::uint16_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_u64(std::uint64_t value) noexcept;

  // Length-prefixed text, so that concatenating "ab"+"c" and "a"+"bc" cannot
  // produce the same preimage.
  void update_len_text(std::string_view text) noexcept;

  [[nodiscard]] Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_count_ = 0;
  std::uint8_t buffer_[64];
  std::size_t buffered_ = 0;
  bool finished_ = false;
};

// Hashes a byte range in one call.
[[nodiscard]] RACK_TURNUP_API Digest sha256(const std::uint8_t* data, std::size_t size);

// Constant-time-ish comparison of two digests. Both are fixed length, so this
// is a plain comparison; it exists so that call sites do not reach for
// std::memcmp on raw buffers with an attacker-controlled length.
[[nodiscard]] RACK_TURNUP_API bool digests_equal(const Digest& left, const Digest& right) noexcept;

}  // namespace rackturnup
