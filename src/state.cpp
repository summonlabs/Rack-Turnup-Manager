// Rack Turnup Manager - durable state structure, digests and invariants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rack_turnup/limits.hpp"
#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

constexpr std::uint32_t kRackRecordModel = 1;
constexpr std::uint32_t kRackStateModel = 1;
constexpr std::uint32_t kServiceStateModel = 1;

[[nodiscard]] Status fail(ErrorCode code, std::string message, ErrorDetail detail = {}) {
  return make_error(code, std::move(message), std::move(detail));
}

void update_digest_bytes(Sha256& hasher, const Digest& digest) noexcept {
  const std::vector<std::uint8_t>& bytes = digest.bytes();
  hasher.update(bytes.data(), bytes.size());
}

void update_time(Sha256& hasher, WallClock time) noexcept {
  hasher.update_u64(static_cast<std::uint64_t>(time.unix_milliseconds()));
}

[[nodiscard]] bool strictly_sorted_by_lifetime(const std::vector<RackTurnupPlan>& plans) {
  for (std::size_t i = 1; i < plans.size(); ++i) {
    if (!(plans[i - 1].lifetime < plans[i].lifetime)) {
      return false;
    }
  }
  return true;
}

}  // namespace

void RackRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kRackRecordModel);
  hasher.update_len_text(id.text());
  hasher.update_len_text(label.text());
  hasher.update_len_text(site.text());
  hasher.update_u64(generation.value());
  update_digest_bytes(hasher, composition.digest());
  hasher.update_byte(static_cast<std::uint8_t>(lifecycle));
  update_time(hasher, registered_at);
  hasher.update_len_text(registered_by.text());
  hasher.update_len_text(source.text());
  hasher.update_len_text(note.text());
}

const RackTurnupPlan* RackState::active_plan() const noexcept {
  for (const RackTurnupPlan& plan : plans) {
    if (plan.state == PlanState::Active) {
      return &plan;
    }
  }
  return nullptr;
}

const RackTurnupPlan* RackState::current_plan() const noexcept {
  const RackTurnupPlan* active = active_plan();
  if (active != nullptr) {
    return active;
  }
  return plans.empty() ? nullptr : &plans.back();
}

RackTurnupPlan* RackState::current_plan() noexcept {
  RackTurnupPlan* active = active_plan();
  if (active != nullptr) {
    return active;
  }
  return plans.empty() ? nullptr : &plans.back();
}

RackTurnupPlan* RackState::active_plan() noexcept {
  for (RackTurnupPlan& plan : plans) {
    if (plan.state == PlanState::Active) {
      return &plan;
    }
  }
  return nullptr;
}

const RackTurnupPlan* RackState::find_plan(const PlanId& id) const noexcept {
  for (const RackTurnupPlan& plan : plans) {
    if (plan.id == id) {
      return &plan;
    }
  }
  return nullptr;
}

RackTurnupPlan* RackState::find_plan(const PlanId& id) noexcept {
  for (RackTurnupPlan& plan : plans) {
    if (plan.id == id) {
      return &plan;
    }
  }
  return nullptr;
}

const TurnupAuthorization* RackState::find_authorization(const AttemptId& attempt) const noexcept {
  for (const TurnupAuthorization& authorization : authorizations) {
    if (authorization.attempt == attempt) {
      return &authorization;
    }
  }
  return nullptr;
}

TurnupAuthorization* RackState::find_authorization(const AttemptId& attempt) noexcept {
  for (TurnupAuthorization& authorization : authorizations) {
    if (authorization.attempt == attempt) {
      return &authorization;
    }
  }
  return nullptr;
}

const EvidenceRecord* RackState::find_evidence(const EvidenceId& id) const noexcept {
  for (const EvidenceRecord& record : evidence) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

bool RackState::has_evidence(const EvidenceId& id) const noexcept { return find_evidence(id) != nullptr; }

const IdempotencyRecord* RackState::find_receipt(const RequestId& id) const noexcept {
  for (const IdempotencyRecord& record : idempotency) {
    if (record.request == id) {
      return &record;
    }
  }
  return nullptr;
}

const RackState* ServiceState::find_rack(const RackId& id) const noexcept {
  for (const RackState& rack : racks) {
    if (rack.rack.id == id) {
      return &rack;
    }
  }
  return nullptr;
}

RackState* ServiceState::find_rack(const RackId& id) noexcept {
  for (RackState& rack : racks) {
    if (rack.rack.id == id) {
      return &rack;
    }
  }
  return nullptr;
}

Digest compute_state_digest(const ServiceState& state) {
  Sha256 hasher;
  hasher.update_u32(kServiceStateModel);
  hasher.update_u64(state.store_epoch.value());
  hasher.update_u64(state.store_sequence.value());
  hasher.update_u64(state.incarnation.value());
  hasher.update_u64(state.control_epoch.value());
  hasher.update_u64(state.observation_sequence.value());
  update_time(hasher, state.created_at);
  update_time(hasher, state.updated_at);
  hasher.update_len_text(state.last_actor.text());
  hasher.update_u32(static_cast<std::uint32_t>(state.racks.size()));
  for (const RackState& rack : state.racks) {
    hasher.update_u32(kRackStateModel);
    rack.rack.update_digest(hasher);
    hasher.update_u32(static_cast<std::uint32_t>(rack.plans.size()));
    for (const RackTurnupPlan& plan : rack.plans) {
      plan.update_digest(hasher);
      update_digest_bytes(hasher, plan.requirements_digest);
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.evidence.size()));
    for (const EvidenceRecord& record : rack.evidence) {
      record.update_digest(hasher);
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.authorizations.size()));
    for (const TurnupAuthorization& authorization : rack.authorizations) {
      authorization.update_digest(hasher);
      hasher.update_byte(static_cast<std::uint8_t>(authorization.state));
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.fences.size()));
    for (const AuthorizationFence& fence : rack.fences) {
      fence.update_digest(hasher);
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.activations.size()));
    for (const ActivationRecord& activation : rack.activations) {
      activation.update_digest(hasher);
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.commissions.size()));
    for (const CommissionRecord& commission : rack.commissions) {
      commission.update_digest(hasher);
    }
    hasher.update_u32(static_cast<std::uint32_t>(rack.idempotency.size()));
    for (const IdempotencyRecord& record : rack.idempotency) {
      record.update_digest(hasher);
    }
    hasher.update_u64(rack.idempotency_evictions);
    hasher.update_u32(static_cast<std::uint32_t>(rack.provenance.size()));
    for (const ProvenanceRecord& record : rack.provenance) {
      record.update_digest(hasher);
    }
  }
  hasher.update_u32(static_cast<std::uint32_t>(state.rejections.size()));
  for (const RejectionRecord& record : state.rejections) {
    record.update_digest(hasher);
  }
  hasher.update_u64(state.rejection_evictions);
  return hasher.finish();
}

Status validate_state(const ServiceState& state) {
  if (state.store_epoch.value() == 0) {
    return fail(ErrorCode::StoreEpochRegression, "a durable state must carry a store epoch");
  }
  if (state.incarnation.value() == 0) {
    return fail(ErrorCode::SnapshotInvalid, "a durable state must carry an incarnation");
  }
  if (state.control_epoch.value() == 0) {
    return fail(ErrorCode::SnapshotInvalid, "a durable state must carry a control epoch");
  }
  if (state.racks.size() > kMaxRacks) {
    return fail(ErrorCode::LimitExceeded, "the state holds more racks than the documented bound",
                ErrorDetail{.operation = "validate_state", .expected = kMaxRacks,
                            .actual = state.racks.size()});
  }
  if (state.rejections.size() > kMaxRejectionJournalEntries) {
    return fail(ErrorCode::LimitExceeded,
                "the rejection journal is larger than the documented bound",
                ErrorDetail{.operation = "validate_state",
                            .expected = kMaxRejectionJournalEntries,
                            .actual = state.rejections.size()});
  }

  for (std::size_t i = 0; i < state.racks.size(); ++i) {
    const RackState& rack = state.racks[i];
    if (rack.rack.id.empty()) {
      return fail(ErrorCode::EmptyValue, "a rack record must carry its identity");
    }
    if (i > 0 && !byte_less(state.racks[i - 1].rack.id.text(), rack.rack.id.text())) {
      return fail(ErrorCode::DuplicateRackId, "rack records are not in canonical order",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    if (!(rack.rack.composition.rack() == rack.rack.id)) {
      return fail(ErrorCode::SnapshotIdentityMismatch,
                  "a rack composition names a different rack than the record",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    if (!digests_equal(rack.rack.composition.digest(), rack.rack.composition.compute_digest())) {
      return fail(ErrorCode::IntegrityCheckFailed,
                  "a rack composition digest does not match its contents",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    if (rack.plans.size() > kMaxPlansPerRack) {
      return fail(ErrorCode::LimitExceeded, "a rack holds more plans than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    for (const RackTurnupPlan& plan : rack.plans) {
      if (plan.lifetime.value() == 0 || plan.revision.value() == 0) {
        return fail(ErrorCode::SnapshotInvalid,
                    "a plan must carry a positive lifetime and revision",
                    ErrorDetail{.operation = "validate_state", .subject = plan.id.text()});
      }
    }
    if (!strictly_sorted_by_lifetime(rack.plans)) {
      return fail(ErrorCode::DuplicatePlanId, "plan lifetimes are not strictly increasing",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    std::size_t active_plans = 0;
    for (const RackTurnupPlan& plan : rack.plans) {
      if (plan.state == PlanState::Active) {
        ++active_plans;
      }
      if (!(plan.binding.rack == rack.rack.id)) {
        return fail(ErrorCode::SnapshotIdentityMismatch, "a plan binds a different rack",
                    ErrorDetail{.operation = "validate_state", .subject = plan.id.text()});
      }
      if (!digests_equal(plan.requirements_digest, plan.compute_requirements_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "a plan requirements digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = plan.id.text()});
      }
    }
    if (active_plans > 1) {
      return fail(ErrorCode::PlanNotActive, "a rack holds more than one active plan",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }

    if (rack.evidence.size() > kMaxEvidenceRecordsPerRack) {
      return fail(ErrorCode::LimitExceeded, "a rack holds more evidence than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    for (std::size_t index = 0; index < rack.evidence.size(); ++index) {
      const EvidenceRecord& record = rack.evidence[index];
      if (!(record.rack == rack.rack.id)) {
        return fail(ErrorCode::SnapshotIdentityMismatch, "evidence names a different rack",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
      if (rack.find_plan(record.plan) == nullptr) {
        return fail(ErrorCode::UnknownPlanId, "evidence names a plan the rack does not hold",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
      if (!record.has_consistent_payload()) {
        return fail(ErrorCode::CorruptState,
                    "an evidence record payload does not match its subsystem",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
      if (!digests_equal(record.record_digest, record.compute_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "an evidence record digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
      if (index > 0 && !(rack.evidence[index - 1].sequence < record.sequence)) {
        return fail(ErrorCode::SequenceRegression,
                    "evidence records are not ordered by observation sequence",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
      if (state.observation_sequence < record.sequence) {
        return fail(ErrorCode::SequenceRegression,
                    "an evidence sequence is ahead of the service sequence",
                    ErrorDetail{.operation = "validate_state", .subject = record.id.text()});
      }
    }

    if (rack.authorizations.size() > kMaxFencesPerRack) {
      return fail(ErrorCode::LimitExceeded,
                  "a rack holds more authorizations than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    for (const TurnupAuthorization& authorization : rack.authorizations) {
      if (!(authorization.rack == rack.rack.id)) {
        return fail(ErrorCode::SnapshotIdentityMismatch, "an authorization names a different rack",
                    ErrorDetail{.operation = "validate_state", .subject = authorization.attempt.text()});
      }
      if (rack.find_plan(authorization.plan) == nullptr) {
        return fail(ErrorCode::UnknownPlanId,
                    "an authorization names a plan the rack does not hold",
                    ErrorDetail{.operation = "validate_state", .subject = authorization.attempt.text()});
      }
      if (!digests_equal(authorization.authorization_digest, authorization.compute_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "an authorization digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = authorization.attempt.text()});
      }
      const bool fenced = authorization.state == AuthorizationState::Fenced;
      bool has_fence = false;
      for (const AuthorizationFence& fence : rack.fences) {
        if (fence.attempt == authorization.attempt) {
          has_fence = true;
          break;
        }
      }
      if (fenced != has_fence) {
        return fail(ErrorCode::TurnupAuthorityFenced,
                    "a fenced authorization and its fence record disagree",
                    ErrorDetail{.operation = "validate_state", .subject = authorization.attempt.text()});
      }
    }

    for (const AuthorizationFence& fence : rack.fences) {
      if (rack.find_authorization(fence.attempt) == nullptr) {
        return fail(ErrorCode::UnknownEvidenceId, "a fence names an unknown authorization",
                    ErrorDetail{.operation = "validate_state", .subject = fence.attempt.text()});
      }
      if (fence.reason == ErrorCode::Ok) {
        return fail(ErrorCode::InvalidArgument, "a fence must record why it was created",
                    ErrorDetail{.operation = "validate_state", .subject = fence.attempt.text()});
      }
    }

    if (rack.activations.size() > kMaxActivationRecordsPerRack) {
      return fail(ErrorCode::LimitExceeded,
                  "a rack holds more activation records than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    for (const ActivationRecord& activation : rack.activations) {
      const TurnupAuthorization* authorization = rack.find_authorization(activation.attempt);
      if (authorization == nullptr) {
        return fail(ErrorCode::TurnupNotAuthorized,
                    "an activation observation names an authorization that is not present",
                    ErrorDetail{.operation = "validate_state", .subject = activation.attempt.text()});
      }
      if (!digests_equal(activation.activation_digest, activation.compute_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "an activation digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = activation.attempt.text()});
      }
      if (!digests_equal(activation.authorization, authorization->authorization_digest)) {
        return fail(ErrorCode::ActivationAttemptConflict,
                    "an activation observation names a different authorization",
                    ErrorDetail{.operation = "validate_state", .subject = activation.attempt.text()});
      }
      if (state.observation_sequence < activation.sequence) {
        return fail(ErrorCode::SequenceRegression,
                    "an activation sequence is ahead of the service sequence",
                    ErrorDetail{.operation = "validate_state", .subject = activation.attempt.text()});
      }
    }

    for (const CommissionRecord& commission : rack.commissions) {
      if (!digests_equal(commission.commission_digest, commission.compute_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "a commission digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = commission.attempt.text()});
      }
      const ActivationRecord* activation = nullptr;
      for (const ActivationRecord& candidate : rack.activations) {
        if (candidate.attempt == commission.attempt) {
          activation = &candidate;
          break;
        }
      }
      if (activation == nullptr) {
        return fail(ErrorCode::ActivationNotObserved,
                    "a commission record names an activation that is not present",
                    ErrorDetail{.operation = "validate_state", .subject = commission.attempt.text()});
      }
      if (!digests_equal(commission.activation, activation->activation_digest)) {
        return fail(ErrorCode::SnapshotIdentityMismatch,
                    "a commission record names a different activation",
                    ErrorDetail{.operation = "validate_state", .subject = commission.attempt.text()});
      }
    }

    if (rack.idempotency.size() > kMaxIdempotencyRecordsPerRack) {
      return fail(ErrorCode::LimitExceeded,
                  "a rack holds more idempotency receipts than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    for (std::size_t index = 0; index < rack.idempotency.size(); ++index) {
      const IdempotencyRecord& receipt = rack.idempotency[index];
      if (receipt.request.empty()) {
        return fail(ErrorCode::EmptyValue, "an idempotency receipt must carry its request identity",
                    ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
      }
      if (!digests_equal(receipt.response_digest, receipt.compute_digest())) {
        return fail(ErrorCode::IntegrityCheckFailed,
                    "an idempotency receipt digest does not match its contents",
                    ErrorDetail{.operation = "validate_state", .subject = receipt.request.text()});
      }
      for (std::size_t other = 0; other < index; ++other) {
        if (rack.idempotency[other].request == receipt.request) {
          return fail(ErrorCode::RequestIdConflict,
                      "a rack holds two idempotency receipts for one request",
                      ErrorDetail{.operation = "validate_state", .subject = receipt.request.text()});
        }
      }
    }

    if (rack.provenance.size() > kMaxProvenanceRecordsPerRack) {
      return fail(ErrorCode::LimitExceeded,
                  "a rack holds more provenance records than the documented bound",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }

    // Lifecycle positions that claim durable facts must be backed by them.
    if (rack.rack.lifecycle == RackLifecycleState::Authorized) {
      bool active = false;
      for (const TurnupAuthorization& authorization : rack.authorizations) {
        if (authorization.state == AuthorizationState::Active) {
          active = true;
          break;
        }
      }
      if (!active) {
        return fail(ErrorCode::TurnupNotAuthorized,
                    "a rack is marked authorized without an active authorization",
                    ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
      }
    }
    const bool claims_active = rack.rack.lifecycle == RackLifecycleState::Active ||
                               rack.rack.lifecycle == RackLifecycleState::Commissioned ||
                               rack.rack.lifecycle == RackLifecycleState::Draining ||
                               rack.rack.lifecycle == RackLifecycleState::Drained;
    if (claims_active && rack.activations.empty()) {
      return fail(ErrorCode::ActivationNotObserved,
                  "a rack claims an active lifecycle without an activation observation",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
    if (rack.rack.lifecycle == RackLifecycleState::Commissioned && rack.commissions.empty()) {
      return fail(ErrorCode::NotCommissioned,
                  "a rack is marked commissioned without a commission record",
                  ErrorDetail{.operation = "validate_state", .subject = rack.rack.id.text()});
    }
  }

  return Status{};
}

}  // namespace rackturnup
