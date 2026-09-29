// Rack Turnup Manager - subsystem, stage, readiness and generation vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/result.hpp"

namespace rackturnup {

// ---------------------------------------------------------------------------
// Subsystems
// ---------------------------------------------------------------------------

// The readiness domains a rack turnup coordinates. Each one is owned by another
// system; this library only composes the evidence that those owners publish.
// The numeric values are part of the durable encoding and never change.
enum class SubsystemKind : std::uint8_t {
  IdentityComposition = 1,
  Power = 2,
  Cooling = 3,
  Network = 4,
  Inventory = 5,
  Health = 6,
  Compatibility = 7,
};

inline constexpr std::size_t kSubsystemKindCount = 7;

[[nodiscard]] RACK_TURNUP_API std::string_view subsystem_kind_name(SubsystemKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<SubsystemKind> subsystem_kind_from_name(std::string_view name);
[[nodiscard]] RACK_TURNUP_API bool is_known_subsystem_kind(std::uint8_t value) noexcept;

// The subsystems in their canonical order. Readiness answers are reported in
// this order so that two runs over the same state produce the same text.
[[nodiscard]] RACK_TURNUP_API std::vector<SubsystemKind> all_subsystem_kinds();

// ---------------------------------------------------------------------------
// Stage ladder
// ---------------------------------------------------------------------------

// The ordered ladder a rack walks from an unproven composition to a
// commissioned state. A stage is never satisfied before every stage above it.
enum class StageKind : std::uint8_t {
  IdentityCompositionValidation = 1,
  PowerReadiness = 2,
  CoolingReadiness = 3,
  NetworkReadiness = 4,
  HardwareInventoryClosure = 5,
  HealthValidation = 6,
  CompatibilityClosure = 7,
  TurnupAuthorization = 8,
  ActivationObservation = 9,
  CommissionedState = 10,
};

inline constexpr std::size_t kPlanStageCount = 10;

[[nodiscard]] RACK_TURNUP_API std::string_view stage_kind_name(StageKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<StageKind> stage_kind_from_name(std::string_view name);
[[nodiscard]] RACK_TURNUP_API bool is_known_stage_kind(std::uint8_t value) noexcept;
[[nodiscard]] RACK_TURNUP_API std::vector<StageKind> plan_stage_ladder();

// True when the stage is gated by per-subsystem evidence, and when it is, the
// subsystem whose readiness state it reports. subsystem_for_stage() is defined
// for exactly those stages.
[[nodiscard]] RACK_TURNUP_API bool stage_has_subsystem(StageKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API SubsystemKind subsystem_for_stage(StageKind kind);
[[nodiscard]] RACK_TURNUP_API StageKind stage_for_subsystem(SubsystemKind kind);

// Zero-based position of a stage in the ladder.
[[nodiscard]] RACK_TURNUP_API std::size_t stage_index(StageKind kind) noexcept;

// ---------------------------------------------------------------------------
// Readiness and stage state
// ---------------------------------------------------------------------------

// Unknown is not a weaker kind of Ready. It means the library has no evidence
// that proves the subsystem either way, and it blocks turnup exactly as a
// proven failure does, while remaining distinguishable in every report.
enum class ReadinessState : std::uint8_t {
  Unknown = 0,
  Ready = 1,
  Degraded = 2,
  Blocked = 3,
};

[[nodiscard]] RACK_TURNUP_API std::string_view readiness_state_name(ReadinessState state) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<ReadinessState> readiness_state_from_name(std::string_view name);

// Worst-of combination, ordered Blocked, Unknown, Degraded, Ready. Blocked
// dominates everything, Unknown dominates Degraded and Ready because an
// unproven subsystem never becomes a proven one, and Degraded dominates Ready.
[[nodiscard]] RACK_TURNUP_API ReadinessState combine_readiness(ReadinessState left,
                                                             ReadinessState right) noexcept;

// True when the state permits a turnup authorization given the policy.
[[nodiscard]] RACK_TURNUP_API bool readiness_permits_turnup(ReadinessState state,
                                                           bool allow_degraded) noexcept;

enum class StageState : std::uint8_t {
  Pending = 0,
  Satisfied = 1,
  Degraded = 2,
  Unknown = 3,
  Blocked = 4,
};

[[nodiscard]] RACK_TURNUP_API std::string_view stage_state_name(StageState state) noexcept;
[[nodiscard]] RACK_TURNUP_API ReadinessState readiness_for_stage_state(StageState state) noexcept;
[[nodiscard]] RACK_TURNUP_API StageState stage_state_for_readiness(ReadinessState state) noexcept;

// ---------------------------------------------------------------------------
// Overall verdict
// ---------------------------------------------------------------------------

// The answer to the core question for one rack at one authority time.
enum class TurnupVerdict : std::uint8_t {
  Blocked = 0,
  NotReady = 1,
  ReadyToAuthorize = 2,
  Authorized = 3,
  ActiveObserved = 4,
  Commissioned = 5,
  FencedAuthority = 6,
};

[[nodiscard]] RACK_TURNUP_API std::string_view turnup_verdict_name(TurnupVerdict verdict) noexcept;

// ---------------------------------------------------------------------------
// Generation stamp
// ---------------------------------------------------------------------------

struct GenerationMismatch {
  std::string field;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
};

// Every generation this library consumes, in one value. Evidence records the
// stamp of the world they observed, and a plan records the stamp it is bound
// to; the two must match field for field before evidence can contribute to a
// readiness answer, which is what stops mixed-generation evidence from being
// combined into a ready verdict.
struct GenerationStamp {
  RackGeneration rack;
  CompositionGeneration composition;
  TopologyGeneration topology;
  DependencyGeneration dependency;
  PowerGeneration power;
  CoolingGeneration cooling;
  NetworkGeneration network;
  InventoryGeneration inventory;
  HealthGeneration health;
  CapacityGeneration capacity;
  MaintenanceGeneration maintenance;
  PolicyGeneration policy;
  FirmwareGeneration firmware;

  [[nodiscard]] bool operator==(const GenerationStamp& other) const noexcept;
  [[nodiscard]] bool operator!=(const GenerationStamp& other) const noexcept {
    return !(*this == other);
  }

  // Field-by-field difference against another stamp, in a fixed field order.
  [[nodiscard]] std::vector<GenerationMismatch> compare(const GenerationStamp& other) const;

  // Contributes the stamp to a preimage in the fixed order above.
  void update_digest(Sha256& hasher) const noexcept;

  [[nodiscard]] std::string describe() const;
};

[[nodiscard]] RACK_TURNUP_API std::string describe_generation_mismatches(
    const std::vector<GenerationMismatch>& mismatches);

// ---------------------------------------------------------------------------
// Authority and lifecycle vocabulary
// ---------------------------------------------------------------------------

// Lifecycle position of one turnup authorization. Fenced and Cancelled are
// terminal: neither can return to Active.
enum class AuthorizationState : std::uint8_t {
  None = 0,
  Active = 1,
  Fenced = 2,
  Cancelled = 3,
  Consumed = 4,
};

[[nodiscard]] RACK_TURNUP_API std::string_view authorization_state_name(
    AuthorizationState state) noexcept;

// What the post-action observation reported. Unknown means the rack was
// observed but its active state could not be determined.
enum class ActivationOutcome : std::uint8_t {
  Unknown = 0,
  Active = 1,
  NotActive = 2,
};

[[nodiscard]] RACK_TURNUP_API std::string_view activation_outcome_name(
    ActivationOutcome outcome) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<ActivationOutcome> activation_outcome_from_name(
    std::string_view name);

// Aggregate lifecycle position of a rack record. It advances only through
// explicit operations that record provenance; it is never inferred from
// evidence, and draining is never the same state as decommissioned.
enum class RackLifecycleState : std::uint8_t {
  Registered = 1,
  Planned = 2,
  Authorized = 3,
  Active = 4,
  Commissioned = 5,
  Draining = 6,
  Drained = 7,
  Decommissioned = 8,
};

[[nodiscard]] RACK_TURNUP_API std::string_view rack_lifecycle_state_name(
    RackLifecycleState state) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<RackLifecycleState> rack_lifecycle_state_from_name(
    std::string_view name);

// True when a rack in `from` may move to `to`. Every other transition is
// refused, so a lifecycle position can never be reached by accident.
[[nodiscard]] RACK_TURNUP_API bool lifecycle_allows(RackLifecycleState from,
                                                    RackLifecycleState to) noexcept;

// Kind of externally meaningful mutation, for provenance and idempotency.
enum class OperationKind : std::uint8_t {
  Unknown = 0,
  RegisterRack = 1,
  UpdateComposition = 2,
  CreatePlan = 3,
  ImportEvidence = 4,
  AuthorizeTurnup = 5,
  RecordActivation = 6,
  Commission = 7,
  Rollback = 8,
  BeginDrain = 9,
  CompleteDrain = 10,
  Decommission = 11,
  TakeControl = 12,
};

[[nodiscard]] RACK_TURNUP_API std::string_view operation_kind_name(
    OperationKind operation) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<OperationKind> operation_kind_from_name(
    std::string_view name);

}  // namespace rackturnup
