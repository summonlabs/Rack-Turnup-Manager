// Rack Turnup Manager - authorization, activation and audit records.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/authorization.hpp"

#include <string>
#include <vector>

namespace rackturnup {
namespace {

constexpr std::uint32_t kAuthorizationModel = 1;
constexpr std::uint32_t kFenceModel = 1;
constexpr std::uint32_t kActivationModel = 1;
constexpr std::uint32_t kCommissionModel = 1;
constexpr std::uint32_t kIdempotencyModel = 1;
constexpr std::uint32_t kProvenanceModel = 1;
constexpr std::uint32_t kRejectionModel = 1;

void update_digest_bytes(Sha256& hasher, const Digest& digest) noexcept {
  const std::vector<std::uint8_t>& bytes = digest.bytes();
  hasher.update(bytes.data(), bytes.size());
}

void update_time(Sha256& hasher, WallClock time) noexcept {
  hasher.update_u64(static_cast<std::uint64_t>(time.unix_milliseconds()));
}

void update_duration(Sha256& hasher, Millis duration) noexcept {
  hasher.update_u64(static_cast<std::uint64_t>(duration.milliseconds()));
}

}  // namespace

void TurnupAuthorization::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kAuthorizationModel);
  hasher.update_len_text(attempt.text());
  hasher.update_len_text(rack.text());
  hasher.update_len_text(plan.text());
  hasher.update_u64(lifetime.value());
  hasher.update_u64(revision.value());
  update_digest_bytes(hasher, composition);
  stamp.update_digest(hasher);
  hasher.update_u64(epoch.value());
  hasher.update_u64(policy.value());
  update_digest_bytes(hasher, verdict);
  update_digest_bytes(hasher, evidence_set);
  hasher.update_u64(evidence_high_water.value());
  update_time(hasher, issued_at);
  update_duration(hasher, validity);
  hasher.update_len_text(actor.text());
  hasher.update_len_text(source.text());
}

Digest TurnupAuthorization::compute_digest() const {
  Sha256 hasher;
  update_digest(hasher);
  return hasher.finish();
}

void AuthorizationFence::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kFenceModel);
  hasher.update_len_text(attempt.text());
  hasher.update_u32(static_cast<std::uint32_t>(reason));
  hasher.update_len_text(explanation);
  hasher.update_u64(sequence.value());
  update_time(hasher, fenced_at);
  hasher.update_len_text(actor.text());
  update_digest_bytes(hasher, request);
}

void ActivationRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kActivationModel);
  hasher.update_len_text(attempt.text());
  hasher.update_len_text(rack.text());
  hasher.update_len_text(plan.text());
  hasher.update_u64(revision.value());
  update_digest_bytes(hasher, authorization);
  update_digest_bytes(hasher, observed_composition);
  update_digest_bytes(hasher, observed_state);
  hasher.update_u64(sequence.value());
  update_time(hasher, observed_at);
  update_duration(hasher, validity);
  hasher.update_byte(static_cast<std::uint8_t>(outcome));
  hasher.update_u32(active_members);
  hasher.update_len_text(source.text());
  hasher.update_len_text(actor.text());
  hasher.update_len_text(note.text());
}

Digest ActivationRecord::compute_digest() const {
  Sha256 hasher;
  update_digest(hasher);
  return hasher.finish();
}

void CommissionRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kCommissionModel);
  hasher.update_len_text(attempt.text());
  hasher.update_len_text(plan.text());
  hasher.update_u64(revision.value());
  update_digest_bytes(hasher, activation);
  update_time(hasher, commissioned_at);
  hasher.update_len_text(actor.text());
  hasher.update_len_text(source.text());
  hasher.update_len_text(note.text());
}

Digest CommissionRecord::compute_digest() const {
  Sha256 hasher;
  update_digest(hasher);
  return hasher.finish();
}

void IdempotencyRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kIdempotencyModel);
  hasher.update_len_text(request.text());
  hasher.update_byte(static_cast<std::uint8_t>(operation));
  update_digest_bytes(hasher, request_digest);
  hasher.update_len_text(rack.text());
  hasher.update_len_text(plan.text());
  hasher.update_u64(revision_before.value());
  hasher.update_u64(revision_after.value());
  hasher.update_u64(sequence_after.value());
  hasher.update_len_text(evidence.text());
  hasher.update_len_text(attempt.text());
  update_time(hasher, recorded_at);
  hasher.update_len_text(actor.text());
}

Digest IdempotencyRecord::compute_digest() const {
  Sha256 hasher;
  update_digest(hasher);
  return hasher.finish();
}

void ProvenanceRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kProvenanceModel);
  hasher.update_byte(static_cast<std::uint8_t>(operation));
  hasher.update_len_text(rack.text());
  hasher.update_len_text(plan.text());
  hasher.update_len_text(actor.text());
  hasher.update_len_text(source.text());
  update_time(hasher, at);
  hasher.update_u64(revision.value());
  hasher.update_u64(sequence.value());
  update_digest_bytes(hasher, request);
  hasher.update_len_text(note.text());
}

void RejectionRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kRejectionModel);
  hasher.update_u32(static_cast<std::uint32_t>(code));
  hasher.update_byte(static_cast<std::uint8_t>(operation));
  hasher.update_len_text(rack.text());
  hasher.update_len_text(plan.text());
  hasher.update_len_text(message);
  update_time(hasher, at);
  hasher.update_len_text(actor.text());
  update_digest_bytes(hasher, request);
}

}  // namespace rackturnup
