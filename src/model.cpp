// Rack Turnup Manager - vocabulary helpers and generation stamps.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/model.hpp"

#include <cstddef>
#include <string>

#include "rack_turnup/errors.hpp"
#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

// Version of the generation stamp preimage layout. It is separate from the
// enclosing preimage version so that a stamp can be hashed on its own.
constexpr std::uint32_t kGenerationStampModel = 1;

[[nodiscard]] Result<SubsystemKind> invalid_subsystem(std::string_view text) {
  return make_error(ErrorCode::InvalidEnumValue, "unknown subsystem name",
                    ErrorDetail{.operation = "subsystem_kind_from_name",
                                .subject = std::string(text)});
}

[[nodiscard]] Result<StageKind> invalid_stage(std::string_view text) {
  return make_error(ErrorCode::InvalidEnumValue, "unknown stage name",
                    ErrorDetail{.operation = "stage_kind_from_name",
                                .subject = std::string(text)});
}

}  // namespace

std::string_view subsystem_kind_name(SubsystemKind kind) noexcept {
  switch (kind) {
    case SubsystemKind::IdentityComposition:
      return "identity_composition";
    case SubsystemKind::Power:
      return "power";
    case SubsystemKind::Cooling:
      return "cooling";
    case SubsystemKind::Network:
      return "network";
    case SubsystemKind::Inventory:
      return "inventory";
    case SubsystemKind::Health:
      return "health";
    case SubsystemKind::Compatibility:
      return "compatibility";
  }
  return "unknown";
}

Result<SubsystemKind> subsystem_kind_from_name(std::string_view name) {
  for (const SubsystemKind kind : all_subsystem_kinds()) {
    if (subsystem_kind_name(kind) == name) {
      return kind;
    }
  }
  return invalid_subsystem(name);
}

bool is_known_subsystem_kind(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(SubsystemKind::IdentityComposition) &&
         value <= static_cast<std::uint8_t>(SubsystemKind::Compatibility);
}

std::vector<SubsystemKind> all_subsystem_kinds() {
  return {SubsystemKind::IdentityComposition, SubsystemKind::Power, SubsystemKind::Cooling,
          SubsystemKind::Network, SubsystemKind::Inventory, SubsystemKind::Health,
          SubsystemKind::Compatibility};
}

std::string_view stage_kind_name(StageKind kind) noexcept {
  switch (kind) {
    case StageKind::IdentityCompositionValidation:
      return "identity_composition_validation";
    case StageKind::PowerReadiness:
      return "power_readiness";
    case StageKind::CoolingReadiness:
      return "cooling_readiness";
    case StageKind::NetworkReadiness:
      return "network_readiness";
    case StageKind::HardwareInventoryClosure:
      return "hardware_inventory_closure";
    case StageKind::HealthValidation:
      return "health_validation";
    case StageKind::CompatibilityClosure:
      return "compatibility_closure";
    case StageKind::TurnupAuthorization:
      return "turnup_authorization";
    case StageKind::ActivationObservation:
      return "activation_observation";
    case StageKind::CommissionedState:
      return "commissioned_state";
  }
  return "unknown";
}

Result<StageKind> stage_kind_from_name(std::string_view name) {
  for (const StageKind kind : plan_stage_ladder()) {
    if (stage_kind_name(kind) == name) {
      return kind;
    }
  }
  return invalid_stage(name);
}

bool is_known_stage_kind(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(StageKind::IdentityCompositionValidation) &&
         value <= static_cast<std::uint8_t>(StageKind::CommissionedState);
}

std::vector<StageKind> plan_stage_ladder() {
  return {StageKind::IdentityCompositionValidation, StageKind::PowerReadiness,
          StageKind::CoolingReadiness, StageKind::NetworkReadiness,
          StageKind::HardwareInventoryClosure, StageKind::HealthValidation,
          StageKind::CompatibilityClosure, StageKind::TurnupAuthorization,
          StageKind::ActivationObservation, StageKind::CommissionedState};
}

bool stage_has_subsystem(StageKind kind) noexcept {
  return kind >= StageKind::IdentityCompositionValidation && kind <= StageKind::CompatibilityClosure;
}

SubsystemKind subsystem_for_stage(StageKind kind) {
  // The first seven rungs are one-to-one with the seven subsystems, in the same
  // order, by construction of the two enumerations.
  const auto value = static_cast<std::uint8_t>(kind);
  if (value >= 1 && value <= static_cast<std::uint8_t>(kSubsystemKindCount)) {
    return static_cast<SubsystemKind>(value);
  }
  return SubsystemKind::IdentityComposition;
}

StageKind stage_for_subsystem(SubsystemKind kind) {
  return static_cast<StageKind>(static_cast<std::uint8_t>(kind));
}

std::size_t stage_index(StageKind kind) noexcept {
  const auto value = static_cast<std::size_t>(kind);
  return (value == 0) ? 0 : (value - 1);
}

std::string_view readiness_state_name(ReadinessState state) noexcept {
  switch (state) {
    case ReadinessState::Unknown:
      return "unknown";
    case ReadinessState::Ready:
      return "ready";
    case ReadinessState::Degraded:
      return "degraded";
    case ReadinessState::Blocked:
      return "blocked";
  }
  return "unknown";
}

Result<ReadinessState> readiness_state_from_name(std::string_view name) {
  const ReadinessState states[] = {ReadinessState::Unknown, ReadinessState::Ready,
                                   ReadinessState::Degraded, ReadinessState::Blocked};
  for (const ReadinessState state : states) {
    if (readiness_state_name(state) == name) {
      return state;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown readiness state name",
                    ErrorDetail{.operation = "readiness_state_from_name",
                                .subject = std::string(name)});
}

namespace {

// Severity used for worst-of combination and for blocker weights.
[[nodiscard]] constexpr int readiness_severity(ReadinessState state) noexcept {
  switch (state) {
    case ReadinessState::Ready:
      return 0;
    case ReadinessState::Degraded:
      return 1;
    case ReadinessState::Unknown:
      return 2;
    case ReadinessState::Blocked:
      return 3;
  }
  return 3;
}

}  // namespace

ReadinessState combine_readiness(ReadinessState left, ReadinessState right) noexcept {
  return (readiness_severity(left) >= readiness_severity(right)) ? left : right;
}

bool readiness_permits_turnup(ReadinessState state, bool allow_degraded) noexcept {
  if (state == ReadinessState::Ready) {
    return true;
  }
  return state == ReadinessState::Degraded && allow_degraded;
}

std::string_view stage_state_name(StageState state) noexcept {
  switch (state) {
    case StageState::Pending:
      return "pending";
    case StageState::Satisfied:
      return "satisfied";
    case StageState::Degraded:
      return "degraded";
    case StageState::Unknown:
      return "unknown";
    case StageState::Blocked:
      return "blocked";
  }
  return "pending";
}

ReadinessState readiness_for_stage_state(StageState state) noexcept {
  switch (state) {
    case StageState::Satisfied:
      return ReadinessState::Ready;
    case StageState::Degraded:
      return ReadinessState::Degraded;
    case StageState::Blocked:
      return ReadinessState::Blocked;
    case StageState::Pending:
    case StageState::Unknown:
      return ReadinessState::Unknown;
  }
  return ReadinessState::Unknown;
}

StageState stage_state_for_readiness(ReadinessState state) noexcept {
  switch (state) {
    case ReadinessState::Ready:
      return StageState::Satisfied;
    case ReadinessState::Degraded:
      return StageState::Degraded;
    case ReadinessState::Blocked:
      return StageState::Blocked;
    case ReadinessState::Unknown:
      return StageState::Unknown;
  }
  return StageState::Unknown;
}

std::string_view turnup_verdict_name(TurnupVerdict verdict) noexcept {
  switch (verdict) {
    case TurnupVerdict::Blocked:
      return "blocked";
    case TurnupVerdict::NotReady:
      return "not_ready";
    case TurnupVerdict::ReadyToAuthorize:
      return "ready_to_authorize";
    case TurnupVerdict::Authorized:
      return "authorized";
    case TurnupVerdict::ActiveObserved:
      return "active_observed";
    case TurnupVerdict::Commissioned:
      return "commissioned";
    case TurnupVerdict::FencedAuthority:
      return "fenced_authority";
  }
  return "not_ready";
}

bool GenerationStamp::operator==(const GenerationStamp& other) const noexcept {
  return rack == other.rack && composition == other.composition && topology == other.topology &&
         dependency == other.dependency && power == other.power && cooling == other.cooling &&
         network == other.network && inventory == other.inventory && health == other.health &&
         capacity == other.capacity && maintenance == other.maintenance && policy == other.policy &&
         firmware == other.firmware;
}

std::vector<GenerationMismatch> GenerationStamp::compare(const GenerationStamp& other) const {
  std::vector<GenerationMismatch> mismatches;
  const auto record = [&mismatches](std::string_view field, std::uint64_t expected,
                                    std::uint64_t actual) {
    if (expected != actual) {
      mismatches.push_back(GenerationMismatch{std::string(field), expected, actual});
    }
  };
  record("rack", rack.value(), other.rack.value());
  record("composition", composition.value(), other.composition.value());
  record("topology", topology.value(), other.topology.value());
  record("dependency", dependency.value(), other.dependency.value());
  record("power", power.value(), other.power.value());
  record("cooling", cooling.value(), other.cooling.value());
  record("network", network.value(), other.network.value());
  record("inventory", inventory.value(), other.inventory.value());
  record("health", health.value(), other.health.value());
  record("capacity", capacity.value(), other.capacity.value());
  record("maintenance", maintenance.value(), other.maintenance.value());
  record("policy", policy.value(), other.policy.value());
  record("firmware", firmware.value(), other.firmware.value());
  return mismatches;
}

void GenerationStamp::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kGenerationStampModel);
  hasher.update_u64(rack.value());
  hasher.update_u64(composition.value());
  hasher.update_u64(topology.value());
  hasher.update_u64(dependency.value());
  hasher.update_u64(power.value());
  hasher.update_u64(cooling.value());
  hasher.update_u64(network.value());
  hasher.update_u64(inventory.value());
  hasher.update_u64(health.value());
  hasher.update_u64(capacity.value());
  hasher.update_u64(maintenance.value());
  hasher.update_u64(policy.value());
  hasher.update_u64(firmware.value());
}

std::string GenerationStamp::describe() const {
  std::string out;
  const auto append = [&out](std::string_view name, std::uint64_t value) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(name);
    out.push_back('=');
    out.append(std::to_string(value));
  };
  append("rack", rack.value());
  append("composition", composition.value());
  append("topology", topology.value());
  append("dependency", dependency.value());
  append("power", power.value());
  append("cooling", cooling.value());
  append("network", network.value());
  append("inventory", inventory.value());
  append("health", health.value());
  append("capacity", capacity.value());
  append("maintenance", maintenance.value());
  append("policy", policy.value());
  append("firmware", firmware.value());
  return out;
}

std::string describe_generation_mismatches(const std::vector<GenerationMismatch>& mismatches) {
  std::string out;
  for (const GenerationMismatch& mismatch : mismatches) {
    if (!out.empty()) {
      out.append(", ");
    }
    out.append(mismatch.field);
    out.append(" expected=");
    out.append(std::to_string(mismatch.expected));
    out.append(" actual=");
    out.append(std::to_string(mismatch.actual));
  }
  return out;
}

std::string_view authorization_state_name(AuthorizationState state) noexcept {
  switch (state) {
    case AuthorizationState::None:
      return "none";
    case AuthorizationState::Active:
      return "active";
    case AuthorizationState::Fenced:
      return "fenced";
    case AuthorizationState::Cancelled:
      return "cancelled";
    case AuthorizationState::Consumed:
      return "consumed";
  }
  return "none";
}

std::string_view activation_outcome_name(ActivationOutcome outcome) noexcept {
  switch (outcome) {
    case ActivationOutcome::Unknown:
      return "unknown";
    case ActivationOutcome::Active:
      return "active";
    case ActivationOutcome::NotActive:
      return "not_active";
  }
  return "unknown";
}

Result<ActivationOutcome> activation_outcome_from_name(std::string_view name) {
  const ActivationOutcome outcomes[] = {ActivationOutcome::Unknown, ActivationOutcome::Active,
                                        ActivationOutcome::NotActive};
  for (const ActivationOutcome outcome : outcomes) {
    if (activation_outcome_name(outcome) == name) {
      return outcome;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown activation outcome name",
                    ErrorDetail{.operation = "activation_outcome_from_name",
                                .subject = std::string(name)});
}

std::string_view rack_lifecycle_state_name(RackLifecycleState state) noexcept {
  switch (state) {
    case RackLifecycleState::Registered:
      return "registered";
    case RackLifecycleState::Planned:
      return "planned";
    case RackLifecycleState::Authorized:
      return "authorized";
    case RackLifecycleState::Active:
      return "active";
    case RackLifecycleState::Commissioned:
      return "commissioned";
    case RackLifecycleState::Draining:
      return "draining";
    case RackLifecycleState::Drained:
      return "drained";
    case RackLifecycleState::Decommissioned:
      return "decommissioned";
  }
  return "registered";
}

Result<RackLifecycleState> rack_lifecycle_state_from_name(std::string_view name) {
  for (std::uint8_t value = 1; value <= 8; ++value) {
    const auto state = static_cast<RackLifecycleState>(value);
    if (rack_lifecycle_state_name(state) == name) {
      return state;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown rack lifecycle state name",
                    ErrorDetail{.operation = "rack_lifecycle_state_from_name",
                                .subject = std::string(name)});
}

bool lifecycle_allows(RackLifecycleState from, RackLifecycleState to) noexcept {
  switch (from) {
    case RackLifecycleState::Registered:
      return to == RackLifecycleState::Planned;
    case RackLifecycleState::Planned:
      return to == RackLifecycleState::Planned || to == RackLifecycleState::Authorized;
    case RackLifecycleState::Authorized:
      return to == RackLifecycleState::Active || to == RackLifecycleState::Planned;
    case RackLifecycleState::Active:
      // A rack whose activation was observed but which is not yet commissioned
      // may still be rolled back, with an explicit acknowledgement recorded by
      // the service.
      return to == RackLifecycleState::Commissioned || to == RackLifecycleState::Planned;
    case RackLifecycleState::Commissioned:
      return to == RackLifecycleState::Draining || to == RackLifecycleState::Planned;
    case RackLifecycleState::Draining:
      return to == RackLifecycleState::Drained;
    case RackLifecycleState::Drained:
      return to == RackLifecycleState::Decommissioned || to == RackLifecycleState::Planned;
    case RackLifecycleState::Decommissioned:
      return to == RackLifecycleState::Planned;
  }
  return false;
}

std::string_view operation_kind_name(OperationKind operation) noexcept {
  switch (operation) {
    case OperationKind::Unknown:
      return "unknown";
    case OperationKind::RegisterRack:
      return "register_rack";
    case OperationKind::UpdateComposition:
      return "update_composition";
    case OperationKind::CreatePlan:
      return "create_plan";
    case OperationKind::ImportEvidence:
      return "import_evidence";
    case OperationKind::AuthorizeTurnup:
      return "authorize_turnup";
    case OperationKind::RecordActivation:
      return "record_activation";
    case OperationKind::Commission:
      return "commission";
    case OperationKind::Rollback:
      return "rollback";
    case OperationKind::BeginDrain:
      return "begin_drain";
    case OperationKind::CompleteDrain:
      return "complete_drain";
    case OperationKind::Decommission:
      return "decommission";
    case OperationKind::TakeControl:
      return "take_control";
  }
  return "unknown";
}

Result<OperationKind> operation_kind_from_name(std::string_view name) {
  for (std::uint8_t value = 0; value <= 12; ++value) {
    const auto operation = static_cast<OperationKind>(value);
    if (operation_kind_name(operation) == name) {
      return operation;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown operation name",
                    ErrorDetail{.operation = "operation_kind_from_name",
                                .subject = std::string(name)});
}

}  // namespace rackturnup
