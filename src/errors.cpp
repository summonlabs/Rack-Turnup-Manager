// Rack Turnup Manager - stable machine-readable error taxonomy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/errors.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace rackturnup {
namespace {

struct ErrorEntry {
  ErrorCode code;
  std::string_view name;
  std::string_view explanation;
};

// The table is ordered by numeric code. Names and explanations are part of the
// public contract: a name is never reused for a different meaning.
constexpr ErrorEntry kEntries[] = {
    {ErrorCode::Ok, "ok", "The operation completed and its effect is recorded."},
    {ErrorCode::InvalidArgument, "invalid_argument", "An argument is outside the domain accepted by this operation."},
    {ErrorCode::MalformedIdentity, "malformed_identity", "An identity does not have the prefix or shape its type requires."},
    {ErrorCode::IdentityTooLong, "identity_too_long", "An identity exceeds its documented byte bound."},
    {ErrorCode::InvalidCharacter, "invalid_character", "A value contains a character outside its accepted set."},
    {ErrorCode::InvalidEnumValue, "invalid_enum_value", "An encoded enumeration value is not part of the vocabulary."},
    {ErrorCode::InvalidRange, "invalid_range", "A numeric value is outside its accepted range."},
    {ErrorCode::EmptyValue, "empty_value", "A value that must be present is empty."},
    {ErrorCode::InvalidUtf8, "invalid_utf8", "Text is not valid UTF-8."},
    {ErrorCode::InvalidTimestamp, "invalid_timestamp", "A timestamp is not a canonical RFC 3339 UTC instant."},
    {ErrorCode::InvalidDuration, "invalid_duration", "A duration is negative, malformed or beyond its documented bound."},
    {ErrorCode::InvalidDigest, "invalid_digest", "A digest is not exactly 64 lowercase hexadecimal characters."},
    {ErrorCode::InvalidFlags, "invalid_flags", "A flag word sets reserved bits or an impossible combination."},
    {ErrorCode::InvalidCoordinate, "invalid_coordinate", "A rack unit or slot coordinate is out of range or used twice."},
    {ErrorCode::DuplicateRackId, "duplicate_rack_id", "The rack identity is already registered in this service."},
    {ErrorCode::UnknownRackId, "unknown_rack_id", "No rack with that identity is registered."},
    {ErrorCode::DuplicatePlanId, "duplicate_plan_id", "The plan identity is already used by another plan."},
    {ErrorCode::UnknownPlanId, "unknown_plan_id", "No plan with that identity exists for the rack."},
    {ErrorCode::DuplicateEvidenceId, "duplicate_evidence_id", "The evidence identity was already imported."},
    {ErrorCode::UnknownEvidenceId, "unknown_evidence_id", "No evidence record with that identity exists."},
    {ErrorCode::DuplicateDeviceId, "duplicate_device_id", "The same device identity appears more than once in one collection."},
    {ErrorCode::DuplicateAttemptId, "duplicate_attempt_id", "The activation attempt identity was already issued."},
    {ErrorCode::RequestIdConflict, "request_id_conflict", "The request identity was already used for a different request."},
    {ErrorCode::DuplicateCompatibilityRequirement, "duplicate_compatibility_requirement", "Two compatibility requirements in one plan are identical."},
    {ErrorCode::DuplicateInventoryEntry, "duplicate_inventory_entry", "The inventory reports the same device more than once."},
    {ErrorCode::StaleRackGeneration, "stale_rack_generation", "The rack record generation does not match the generation the request was bound to."},
    {ErrorCode::StaleCompositionGeneration, "stale_composition_generation", "The composition generation does not match the one the request was bound to."},
    {ErrorCode::StalePlanRevision, "stale_plan_revision", "The plan revision does not match the revision the request was bound to."},
    {ErrorCode::StaleAuthorityEpoch, "stale_authority_epoch", "The control epoch of the request predates the current control epoch."},
    {ErrorCode::StaleTopologyGeneration, "stale_topology_generation", "The topology generation does not match the plan binding."},
    {ErrorCode::StalePowerGeneration, "stale_power_generation", "The power generation does not match the plan binding."},
    {ErrorCode::StaleCoolingGeneration, "stale_cooling_generation", "The cooling generation does not match the plan binding."},
    {ErrorCode::StaleNetworkGeneration, "stale_network_generation", "The network generation does not match the plan binding."},
    {ErrorCode::StalePolicyGeneration, "stale_policy_generation", "The policy generation does not match the plan binding."},
    {ErrorCode::StaleDependencyGeneration, "stale_dependency_generation", "The dependency generation does not match the plan binding."},
    {ErrorCode::StaleCapacityGeneration, "stale_capacity_generation", "The capacity generation does not match the plan binding."},
    {ErrorCode::StaleMaintenanceGeneration, "stale_maintenance_generation", "The maintenance generation does not match the plan binding."},
    {ErrorCode::StaleHealthGeneration, "stale_health_generation", "The health generation does not match the plan binding."},
    {ErrorCode::StaleFirmwareGeneration, "stale_firmware_generation", "The firmware generation does not match the plan binding."},
    {ErrorCode::StaleObservedGeneration, "stale_observed_generation", "An observation describes a generation older than the one it is evaluated against."},
    {ErrorCode::CompositionDigestMismatch, "composition_digest_mismatch", "The composition digest does not match the composition the plan is bound to."},
    {ErrorCode::EvidenceBindingMismatch, "evidence_binding_mismatch", "Evidence was observed in a different generation set than the plan binding."},
    {ErrorCode::TurnupAuthorityFenced, "turnup_authority_fenced", "The turnup authorization was fenced and can never be used again."},
    {ErrorCode::SequenceRegression, "sequence_regression", "A sequence number is lower than one already observed."},
    {ErrorCode::SnapshotIdentityMismatch, "snapshot_identity_mismatch", "A snapshot does not describe the identity it claims to describe."},
    {ErrorCode::StaleWriterFenced, "stale_writer_fenced", "A writer holding an older store epoch attempted to publish."},
    {ErrorCode::StoreEpochRegression, "store_epoch_regression", "The store epoch does not advance beyond the epoch already published."},
    {ErrorCode::UnknownCompositionMember, "unknown_composition_member", "A device is not a member of the composition it was reported against."},
    {ErrorCode::AuthoritySuperseded, "authority_superseded", "Newer authority took over, so the older authority no longer applies."},
    {ErrorCode::StageOutOfOrder, "stage_out_of_order", "A stage cannot be satisfied before the stages that precede it."},
    {ErrorCode::LifecycleTransitionNotAllowed, "lifecycle_transition_not_allowed", "The requested lifecycle transition is not allowed from the current state."},
    {ErrorCode::TurnupAlreadyAuthorized, "turnup_already_authorized", "An active turnup authorization already exists for this rack."},
    {ErrorCode::TurnupNotAuthorized, "turnup_not_authorized", "No active turnup authorization exists for this rack."},
    {ErrorCode::TurnupNotReady, "turnup_not_ready", "The rack is not proven ready, so turnup cannot be authorized."},
    {ErrorCode::ActivationNotObserved, "activation_not_observed", "No post-action activation observation exists for this attempt."},
    {ErrorCode::AlreadyCommissioned, "already_commissioned", "The rack is already commissioned under an older plan."},
    {ErrorCode::NotCommissioned, "not_commissioned", "The rack is not commissioned, so the operation does not apply."},
    {ErrorCode::PlanNotActive, "plan_not_active", "The plan is superseded or cancelled and cannot be mutated further."},
    {ErrorCode::RollbackNotAllowed, "rollback_not_allowed", "The current lifecycle position does not permit a rollback."},
    {ErrorCode::ActivationAttemptConflict, "activation_attempt_conflict", "A different activation observation was already recorded for this attempt."},
    {ErrorCode::ActivationNotConfirmed, "activation_not_confirmed", "The post-action observation does not confirm the rack as active."},
    {ErrorCode::CompositionEmpty, "composition_empty", "A rack composition must contain at least one member."},
    {ErrorCode::CompositionGenerationRegression, "composition_generation_regression", "A new composition generation must be strictly greater than the current one."},
    {ErrorCode::InventoryGap, "inventory_gap", "The inventory does not account for every member of the composition."},
    {ErrorCode::DuplicateInventorySlot, "duplicate_inventory_slot", "Two inventory entries occupy the same rack unit and slot."},
    {ErrorCode::ReadinessUnknown, "readiness_unknown", "Readiness is not proven because evidence is missing or unmeasured."},
    {ErrorCode::ReadinessBlocked, "readiness_blocked", "Readiness is proven to be blocked by the reported condition."},
    {ErrorCode::EvidenceMissing, "evidence_missing", "No evidence covers this subsystem for the current binding."},
    {ErrorCode::EvidenceStale, "evidence_stale", "Evidence exists but its validity window has closed at the authority time."},
    {ErrorCode::EvidenceSuperseded, "evidence_superseded", "Newer evidence for the same subsystem and scope replaced this record."},
    {ErrorCode::HeadroomInsufficient, "headroom_insufficient", "The declared available power or capacity is below what the plan requires."},
    {ErrorCode::HealthFailed, "health_failed", "Device health evidence reports a failure."},
    {ErrorCode::UnhealthyDevice, "unhealthy_device", "A device is not healthy enough for the plan to accept it."},
    {ErrorCode::InventoryAmbiguous, "inventory_ambiguous", "The inventory contradicts the composition or itself."},
    {ErrorCode::HealthDegraded, "health_degraded", "Device health evidence reports a warning rather than a failure."},
    {ErrorCode::InventoryUnreadable, "inventory_unreadable", "An inventory position could not be read, so closure is not proven."},
    {ErrorCode::CompatibilityUnsatisfied, "compatibility_unsatisfied", "A mandatory compatibility requirement is not satisfied."},
    {ErrorCode::UnknownCompatibilityProfile, "unknown_compatibility_profile", "The compatibility profile is not one this build implements."},
    {ErrorCode::MalformedCompatibilityProfile, "malformed_compatibility_profile", "The compatibility profile is internally inconsistent."},
    {ErrorCode::FirmwareBaselineMismatch, "firmware_baseline_mismatch", "An observed firmware baseline is not the required baseline."},
    {ErrorCode::TraitUnknown, "trait_unknown", "A required trait is not declared by the member it is required of."},
    {ErrorCode::CompatibilityAdvisory, "compatibility_advisory", "An advisory compatibility requirement is not satisfied."},
    {ErrorCode::IoFailure, "io_failure", "An operating-system input or output operation failed."},
    {ErrorCode::IntegrityCheckFailed, "integrity_check_failed", "A stored record does not match its recorded digest or checksum."},
    {ErrorCode::UnsupportedFormatVersion, "unsupported_format_version", "The durable format version is not one this build can read."},
    {ErrorCode::TruncatedState, "truncated_state", "The durable record ends before its declared length."},
    {ErrorCode::CorruptState, "corrupt_state", "The durable record is structurally corrupt."},
    {ErrorCode::StateTooLarge, "state_too_large", "The durable record exceeds the documented size bound."},
    {ErrorCode::NoAuthoritativeState, "no_authoritative_state", "No durable state was found at the configured path."},
    {ErrorCode::InvalidStateEncoding, "invalid_state_encoding", "The durable record is not valid for its declared encoding."},
    {ErrorCode::ReservedBitsSet, "reserved_bits_set", "A reserved field is not zero."},
    {ErrorCode::TrailingBytes, "trailing_bytes", "Bytes remain after the declared end of the record."},
    {ErrorCode::MalformedFieldOrder, "malformed_field_order", "Fields were read in an order the format does not allow."},
    {ErrorCode::StaleDurableState, "stale_durable_state", "The durable state file predates the recorded publication watermark."},
    {ErrorCode::LimitExceeded, "limit_exceeded", "An operation would exceed a documented resource bound."},
    {ErrorCode::SnapshotInvalid, "snapshot_invalid", "A snapshot is structurally invalid."},
    {ErrorCode::CapacityExhausted, "capacity_exhausted", "A bounded collection has no room left for the requested entry."},
    {ErrorCode::WriterLockHeld, "writer_lock_held", "Another process holds the exclusive writer lock."},
    {ErrorCode::WriterLockInvalid, "writer_lock_invalid", "The writer lock record does not describe a valid owner."},
    {ErrorCode::ReadOnlyStore, "read_only_store", "The store was opened read-only and cannot be mutated."},
    {ErrorCode::LockIoFailure, "lock_io_failure", "The writer lock file could not be created, read or locked."},
    {ErrorCode::WriterIdentityMismatch, "writer_identity_mismatch", "The writer record does not identify the process that holds the lock."},
    {ErrorCode::PublicationFailed, "publication_failed", "The atomic publication step failed, so the previous state remains authoritative."},
};

[[nodiscard]] const ErrorEntry* find_entry(ErrorCode code) noexcept {
  for (const ErrorEntry& entry : kEntries) {
    if (entry.code == code) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

std::uint16_t code_value(ErrorCode code) noexcept {
  return static_cast<std::uint16_t>(code);
}

ErrorCategory category_of(ErrorCode code) noexcept {
  const std::uint16_t value = code_value(code);
  if (value == 0) {
    return ErrorCategory::None;
  }
  if (value >= 100 && value < 200) {
    return ErrorCategory::Input;
  }
  if (value >= 200 && value < 300) {
    return ErrorCategory::Identity;
  }
  if (value >= 300 && value < 400) {
    return ErrorCategory::Authority;
  }
  if (value >= 400 && value < 500) {
    return ErrorCategory::Lifecycle;
  }
  if (value >= 500 && value < 600) {
    return ErrorCategory::Readiness;
  }
  if (value >= 600 && value < 700) {
    return ErrorCategory::Compatibility;
  }
  if (value >= 700 && value < 800) {
    return ErrorCategory::Persistence;
  }
  if (value >= 800 && value < 900) {
    return ErrorCategory::Limits;
  }
  if (value >= 900 && value < 1000) {
    return ErrorCategory::Writer;
  }
  return ErrorCategory::None;
}

std::string_view code_name(ErrorCode code) noexcept {
  const ErrorEntry* entry = find_entry(code);
  return (entry != nullptr) ? entry->name : std::string_view{"unrecognized_error_code"};
}

std::string_view explain(ErrorCode code) noexcept {
  const ErrorEntry* entry = find_entry(code);
  return (entry != nullptr) ? entry->explanation
                            : std::string_view{"This code has no recorded explanation."};
}

bool is_staleness_code(ErrorCode code) noexcept {
  return category_of(code) == ErrorCategory::Authority;
}

bool is_readiness_code(ErrorCode code) noexcept {
  return category_of(code) == ErrorCategory::Readiness;
}

TurnupError make_error(ErrorCode code, std::string message, ErrorDetail detail) {
  TurnupError error;
  error.code = code;
  error.message = std::move(message);
  error.detail = std::move(detail);
  return error;
}

std::string describe(const TurnupError& error) {
  std::string out;
  out.append(code_name(error.code));
  out.push_back('(');
  out.append(std::to_string(code_value(error.code)));
  out.append("): ");
  out.append(error.message);

  const ErrorDetail& detail = error.detail;
  const bool has_detail = !detail.operation.empty() || !detail.subject.empty() ||
                          !detail.related.empty() || detail.expected != 0 || detail.actual != 0 ||
                          !detail.items.empty();
  if (!has_detail) {
    return out;
  }
  out.append(" [");
  bool first = true;
  const auto append_field = [&out, &first](std::string_view key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    if (!first) {
      out.push_back(' ');
    }
    first = false;
    out.append(key);
    out.push_back('=');
    out.append(value);
  };
  append_field("operation", detail.operation);
  append_field("subject", detail.subject);
  append_field("related", detail.related);
  if (detail.expected != 0 || detail.actual != 0) {
    if (!first) {
      out.push_back(' ');
    }
    first = false;
    out.append("expected=");
    out.append(std::to_string(detail.expected));
    out.push_back(' ');
    out.append("actual=");
    out.append(std::to_string(detail.actual));
  }
  if (!detail.items.empty()) {
    if (!first) {
      out.push_back(' ');
    }
    first = false;
    out.append("items=");
    for (std::size_t i = 0; i < detail.items.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      out.append(detail.items[i]);
    }
  }
  out.push_back(']');
  return out;
}

}  // namespace rackturnup
