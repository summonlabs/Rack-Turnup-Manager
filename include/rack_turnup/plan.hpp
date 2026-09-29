// Rack Turnup Manager - turnup plans, requirements and policy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/limits.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/observation.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// ---------------------------------------------------------------------------
// Compatibility requirements
// ---------------------------------------------------------------------------

enum class RequirementKind : std::uint8_t {
  TraitOnEveryMember = 1,
  TraitOnAnyMember = 2,
  TraitOnDevice = 3,
  BaselineOnEveryMember = 4,
  BaselineOnDevice = 5,
};

[[nodiscard]] RACK_TURNUP_API std::string_view requirement_kind_name(RequirementKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<RequirementKind> requirement_kind_from_name(std::string_view name);
[[nodiscard]] RACK_TURNUP_API bool requirement_is_trait_scoped(RequirementKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API bool requirement_is_baseline_scoped(RequirementKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API bool requirement_is_device_scoped(RequirementKind kind) noexcept;

// A single compatibility obligation. Exactly the fields the kind needs must be
// present: a trait requirement carries a trait, a device requirement carries a
// device, and a baseline requirement carries a baseline. Impossible
// combinations are rejected rather than interpreted.
struct CompatibilityRequirement {
  RequirementKind kind = RequirementKind::TraitOnEveryMember;
  Trait trait;
  DeviceId device;
  FirmwareBaselineId firmware_baseline;
  bool mandatory = true;
  Note note;

  [[nodiscard]] bool is_consistent() const noexcept;
  [[nodiscard]] std::string to_text() const;
  void update_digest(Sha256& hasher) const noexcept;
};

[[nodiscard]] RACK_TURNUP_API Result<CompatibilityRequirement> make_compatibility_requirement(
    RequirementKind kind, Trait trait, DeviceId device, FirmwareBaselineId firmware_baseline,
    bool mandatory, Note note);

// Parses the canonical text form written by to_text().
[[nodiscard]] RACK_TURNUP_API Result<CompatibilityRequirement> parse_compatibility_requirement(
    std::string_view text);

// ---------------------------------------------------------------------------
// Plan requirements
// ---------------------------------------------------------------------------

// What the plan demands of each subsystem. Zero means the plan demands nothing
// of that quantity, which is different from "the quantity is unknown": the
// plan states its requirement explicitly and the evidence states what was
// observed.
struct PlanRequirements {
  std::uint64_t required_power_milliwatts = 0;
  std::uint64_t required_cooling_milliwatts = 0;
  std::uint32_t required_network_ports = 0;
  bool require_redundant_feeds = false;
  bool require_applied_power_verification = true;
  bool require_cooling_delivery_verification = true;
  bool require_network_authority_verification = true;
  bool require_health_pass_for_every_member = true;
  FabricTopologyReference required_topology;
  std::vector<CompatibilityRequirement> compatibility;

  void update_digest(Sha256& hasher) const noexcept;
};

// Validates a requirement set, sorts and deduplicates its compatibility
// requirements, and enforces the documented bounds.
[[nodiscard]] RACK_TURNUP_API Result<PlanRequirements> make_plan_requirements(
    PlanRequirements requirements);

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

struct TurnupPolicy {
  PolicyGeneration generation;
  // When false, a degraded subsystem blocks authorization exactly as a proven
  // failure does, while remaining distinguishable in every report.
  bool allow_degraded_subsystems = false;
  // Validity window of an issued turnup authorization.
  Millis authorization_validity = Millis::minutes(30);
  // Validity applied to imported evidence that does not declare its own.
  Millis default_evidence_validity = Millis::minutes(15);

  void update_digest(Sha256& hasher) const noexcept;
};

[[nodiscard]] RACK_TURNUP_API Result<TurnupPolicy> make_turnup_policy(TurnupPolicy policy);

// ---------------------------------------------------------------------------
// Plan binding and plan
// ---------------------------------------------------------------------------

// The exact world a plan speaks about: the rack, the composition digest, every
// generation this library consumes, and the control epoch that issued it.
struct PlanBinding {
  RackId rack;
  Digest composition;
  GenerationStamp stamp;
  ControlEpoch epoch;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool operator==(const PlanBinding& other) const noexcept;
  [[nodiscard]] bool operator!=(const PlanBinding& other) const noexcept {
    return !(*this == other);
  }
};

enum class PlanState : std::uint8_t {
  Active = 1,
  Superseded = 2,
  Cancelled = 3,
  Completed = 4,
};

[[nodiscard]] RACK_TURNUP_API std::string_view plan_state_name(PlanState state) noexcept;

// A turnup plan. It stores facts only: the binding it speaks about, the
// requirements it imposes and the state its lifecycle reached. The stage ladder
// is always derived from those facts, so restarting the process cannot
// resurrect a stale verdict.
struct RackTurnupPlan {
  PlanId id;
  PlanLifetime lifetime;
  PlanRevision revision;
  PlanBinding binding;
  PlanRequirements requirements;
  TurnupPolicy policy;
  PlanState state = PlanState::Active;
  // Digest over requirements and policy, so a reader can prove that the
  // requirements were not altered after they were published.
  Digest requirements_digest;
  // Sequence of the newest evidence record imported under this plan.
  ObservationSequence evidence_high_water;
  WallClock created_at;
  ActorId created_by;
  SourceReference source;
  Note note;

  [[nodiscard]] bool is_active() const noexcept { return state == PlanState::Active; }
  [[nodiscard]] Digest compute_requirements_digest() const;
  void update_digest(Sha256& hasher) const noexcept;
};

}  // namespace rackturnup
