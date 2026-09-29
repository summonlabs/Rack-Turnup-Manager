// Rack Turnup Manager - readiness evaluation, blocker aggregation and digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rack_turnup/authorization.hpp"
#include "rack_turnup/composition.hpp"
#include "rack_turnup/digest.hpp"
#include "rack_turnup/evidence.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// How strongly one finding argues against a turnup.
enum class BlockerSeverity : std::uint8_t {
  Advisory = 0,
  Unknown = 1,
  Blocking = 2,
};

[[nodiscard]] RACK_TURNUP_API std::string_view blocker_severity_name(
    BlockerSeverity severity) noexcept;

// One reason a rack is not ready, or a warning that it is only conditionally
// ready. Every blocker names the subsystem and stage it belongs to, so a reader
// never has to guess where a finding came from.
struct Blocker {
  BlockerSeverity severity = BlockerSeverity::Blocking;
  ErrorCode code = ErrorCode::Ok;
  StageKind stage = StageKind::IdentityCompositionValidation;
  // False for stages that are not gated by one subsystem (authorization,
  // activation, commissioned state).
  bool has_subsystem = false;
  SubsystemKind subsystem = SubsystemKind::IdentityComposition;
  std::string subject;
  std::string explanation;
  std::vector<std::string> items;
};

// Total order used to sort a blocker report. Severity first, then the stage
// ladder, then the subsystem (subsystem-less blockers sort first within a
// stage), then the subject text, then the code value. The
// order is a function of the blockers alone, so the primary blocker of a given
// state never depends on map ordering or thread scheduling.
[[nodiscard]] RACK_TURNUP_API bool blocker_precedes(const Blocker& left,
                                                    const Blocker& right) noexcept;

struct BlockerReport {
  RackId rack;
  PlanId plan;
  PlanRevision revision;
  WallClock authority_time;
  TurnupVerdict verdict = TurnupVerdict::NotReady;
  std::vector<Blocker> blockers;
  std::size_t suppressed = 0;

  [[nodiscard]] const Blocker* primary() const noexcept {
    return blockers.empty() ? nullptr : &blockers.front();
  }
  [[nodiscard]] std::string explain() const;
};

// What one subsystem's evidence proves at an authority time.
struct SubsystemVerdict {
  SubsystemKind subsystem = SubsystemKind::IdentityComposition;
  ReadinessState state = ReadinessState::Unknown;
  ErrorCode code = ErrorCode::Ok;
  std::string explanation;
  bool has_evidence = false;
  EvidenceId evidence;
  ObservationSequence sequence;
  WallClock observed_at;
  Millis validity;
  FreshnessVerdict freshness = FreshnessVerdict::Fresh;
  std::vector<std::string> items;
};

// What one rung of the ladder proves.
struct StageVerdict {
  StageKind stage = StageKind::IdentityCompositionValidation;
  StageState state = StageState::Pending;
  ErrorCode code = ErrorCode::Ok;
  std::string explanation;
  // True when the stage itself might be satisfied but an earlier stage is not,
  // so the ladder holds it back.
  bool gated_by_predecessor = false;
};

// Summary of the authorization the evaluation considered.
struct AuthorizationView {
  bool present = false;
  AttemptId attempt;
  AuthorizationState state = AuthorizationState::None;
  WallClock issued_at;
  Millis validity;
  FreshnessVerdict freshness = FreshnessVerdict::Fresh;
  Digest verdict;
  bool consumed = false;
};

// The complete answer to the core question for one rack at one authority time.
struct Evaluation {
  RackId rack;
  SiteId site;
  RackLifecycleState lifecycle = RackLifecycleState::Registered;
  RackGeneration rack_generation;
  CompositionGeneration composition_generation;
  Digest composition;
  PlanId plan;
  PlanLifetime lifetime;
  PlanRevision revision;
  PolicyGeneration policy;
  ControlEpoch epoch;
  WallClock authority_time;
  // False when the plan binding no longer matches the rack record, which means
  // every authorization issued under it is fenced.
  bool binding_matches = true;
  bool allow_degraded = false;
  std::vector<GenerationMismatch> binding_mismatches;
  // Non-numeric binding problems, such as a composition digest or rack identity
  // that no longer matches the plan.
  std::vector<std::string> binding_problems;
  std::array<SubsystemVerdict, kSubsystemKindCount> subsystems{};
  std::vector<StageVerdict> stages;
  std::vector<Blocker> blockers;
  std::size_t suppressed_blockers = 0;
  TurnupVerdict verdict = TurnupVerdict::NotReady;
  std::string explanation;
  Digest evidence_set;
  Digest verdict_digest;
  Digest requirements;
  AuthorizationView authorization;
  bool activation_observed = false;
  bool commissioned = false;
  Digest activation;
  ObservationSequence evidence_high_water;
  std::vector<EvidenceAssessment> evidence_assessments;
  std::size_t eligible_evidence = 0;
  std::size_t superseded_evidence = 0;
  std::size_t rejected_evidence = 0;
  ExpectedDeviceCount expected_members;
  ObservedDeviceCount observed_members;
  HealthyDeviceCount healthy_members;

  [[nodiscard]] const SubsystemVerdict& subsystem(SubsystemKind kind) const noexcept;
  [[nodiscard]] const StageVerdict& stage(StageKind kind) const noexcept;
  [[nodiscard]] const Blocker* primary_blocker() const noexcept {
    return blockers.empty() ? nullptr : &blockers.front();
  }
  // True when every stage up to and including the authorization stage is
  // satisfied.
  [[nodiscard]] bool ready_to_authorize() const noexcept;
  [[nodiscard]] BlockerReport to_report() const;
};

// Everything the evaluation reads. The evaluation is a pure function of this
// context: it performs no I/O, consults no clock and mutates nothing, so the
// same context always produces the same verdict and the same digest.
struct EvaluationContext {
  const RackComposition& composition;
  const RackTurnupPlan& plan;
  const std::vector<EvidenceRecord>& evidence;
  const std::vector<TurnupAuthorization>& authorizations;
  const std::vector<ActivationRecord>& activations;
  const std::vector<CommissionRecord>& commissions;
  RackGeneration rack_generation;
  RackLifecycleState lifecycle = RackLifecycleState::Registered;
  WallClock authority_time;
};

[[nodiscard]] RACK_TURNUP_API Evaluation evaluate_rack(const EvaluationContext& context);

// Digest over the evidence records that were eligible, in deterministic order.
[[nodiscard]] RACK_TURNUP_API Digest compute_evidence_set_digest(const EvidenceSelection& selection,
                                                                 const PlanBinding& binding);

// Digest over the whole decision. An authorization binds to this value, so a
// change in any input that could change the answer changes the digest.
[[nodiscard]] RACK_TURNUP_API Digest compute_verdict_digest(const Evaluation& evaluation);

[[nodiscard]] RACK_TURNUP_API std::string explain_verdict(TurnupVerdict verdict);

// Renders a subsystem verdict as one deterministic line, used by the CLI and
// by the tests.
[[nodiscard]] RACK_TURNUP_API std::string describe_subsystem_verdict(
    const SubsystemVerdict& verdict);

}  // namespace rackturnup
