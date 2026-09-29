// Rack Turnup Manager - SHA-256 and content digest primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/digest.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>

#include "rack_turnup/errors.hpp"
#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

// SHA-256 operates on 512-bit message blocks.
constexpr std::size_t kBlockBytes = 64;

// FIPS 180-4 initial hash value.
constexpr std::uint32_t kInitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

// FIPS 180-4 round constants: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                   unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

// Value of one hexadecimal digit, or -1 when the character is not a digit.
[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

}  // namespace

Digest Digest::of(std::vector<std::uint8_t> bytes) {
  // A digest is exactly 32 bytes by construction everywhere inside this
  // library. A different size can only come from a programming error, and it
  // fails closed: the result is the zero digest, which is "absent" and never
  // compares equal to any real digest.
  if (bytes.size() != kBytes) {
    assert(false && "Digest::of requires exactly 32 bytes");
    return Digest{};
  }
  Digest digest;
  digest.bytes_ = std::move(bytes);
  return digest;
}

Result<Digest> Digest::parse(std::string_view text) {
  if (text.size() != kBytes * 2u) {
    return make_error(ErrorCode::InvalidDigest,
                      "digest text must be exactly 64 lowercase hexadecimal characters",
                      ErrorDetail{.operation = "Digest::parse",
                                  .expected = kBytes * 2u,
                                  .actual = text.size()});
  }
  std::vector<std::uint8_t> bytes(kBytes, 0);
  for (std::size_t i = 0; i < kBytes; ++i) {
    const int high = hex_value(text[i * 2u]);
    const int low = hex_value(text[i * 2u + 1u]);
    if (high < 0 || low < 0) {
      // Uppercase is rejected on purpose: the canonical text form is lowercase,
      // and accepting both would let two spellings of one digest compare
      // unequal in text.
      return make_error(ErrorCode::InvalidDigest,
                        "digest text must be lowercase hexadecimal",
                        ErrorDetail{.operation = "Digest::parse",
                                    .subject = std::string(text)});
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Digest::of(std::move(bytes));
}

std::string Digest::to_hex() const { return rackturnup::to_hex(bytes_); }

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

Sha256::Sha256() noexcept
    : state_{kInitialState[0], kInitialState[1], kInitialState[2], kInitialState[3],
             kInitialState[4], kInitialState[5], kInitialState[6], kInitialState[7]},
      bit_count_(0),
      buffer_{},
      buffered_(0),
      finished_(false) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t i = 0; i < 16; ++i) {
    const std::size_t base = i * 4u;
    schedule[i] = (static_cast<std::uint32_t>(block[base]) << 24) |
                  (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                  (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                  static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^
                             (schedule[i - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^
                             (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + sum1 + choose + kRoundConstants[i] + schedule[i];
    const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
  if (finished_) {
    // Finishing a hash is a one-way operation; feeding more data afterwards
    // would silently produce a digest of something the caller did not ask for.
    assert(false && "Sha256::update called after finish");
    return;
  }
  if (size == 0) {
    return;
  }
  bit_count_ += static_cast<std::uint64_t>(size) * 8u;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t take = std::min(size - offset, kBlockBytes - buffered_);
    std::memcpy(buffer_ + buffered_, data + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == kBlockBytes) {
      compress(buffer_);
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Sha256::update_byte(std::uint8_t value) noexcept { update(&value, 1); }

void Sha256::update_u8(std::uint8_t value) noexcept { update_byte(value); }

void Sha256::update_u16(std::uint16_t value) noexcept {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 2);
}

void Sha256::update_u32(std::uint32_t value) noexcept {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>((value >> 24) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 16) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 4);
}

void Sha256::update_u64(std::uint64_t value) noexcept {
  const std::uint8_t bytes[8] = {static_cast<std::uint8_t>((value >> 56) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 48) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 40) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 32) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 24) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 16) & 0xffu),
                                 static_cast<std::uint8_t>((value >> 8) & 0xffu),
                                 static_cast<std::uint8_t>(value & 0xffu)};
  update(bytes, 8);
}

void Sha256::update_len_text(std::string_view text) noexcept {
  // Length prefixing is what stops "ab" + "c" from colliding with "a" + "bc".
  update_u64(static_cast<std::uint64_t>(text.size()));
  update(text);
}

Digest Sha256::finish() noexcept {
  if (!finished_) {
    const std::uint64_t total_bits = bit_count_;
    std::uint8_t padding[128];
    std::memset(padding, 0, sizeof(padding));
    padding[0] = 0x80u;
    const std::size_t padding_length =
        (buffered_ < 56u) ? (56u - buffered_) : (120u - buffered_);
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<std::uint8_t>((total_bits >> (56u - 8u * i)) & 0xffu);
    }
    update(padding, padding_length);
    update(length_bytes, 8);
    // The length field describes the message, not the padding, so the running
    // count is restored before the object is sealed.
    bit_count_ = total_bits;
    buffered_ = 0;
    finished_ = true;
  }

  std::vector<std::uint8_t> bytes(Digest::kBytes, 0);
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i * 4u] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
    bytes[i * 4u + 1u] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
    bytes[i * 4u + 2u] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
    bytes[i * 4u + 3u] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
  return Digest::of(std::move(bytes));
}

Digest sha256(const std::uint8_t* data, std::size_t size) {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

bool digests_equal(const Digest& left, const Digest& right) noexcept {
  const std::vector<std::uint8_t>& a = left.bytes();
  const std::vector<std::uint8_t>& b = right.bytes();
  if (a.size() != b.size()) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
  }
  return difference == 0;
}

}  // namespace rackturnup
