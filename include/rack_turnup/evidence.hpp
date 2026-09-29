// Rack Turnup Manager - generation-stamped readiness evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/observation.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// One observation, stamped with the world it observed. The record digest
// covers every immutable field, so a record that was altered in place is
// detected the next time it is read.
struct EvidenceRecord {
  EvidenceId id;
  RackId rack;
  SubsystemKind subsystem = SubsystemKind::Power;
  GenerationStamp stamp;
  // Composition digest the observer saw. It must equal the one the plan is
  // bound to before the observation can contribute to a verdict.
  Digest composition;
  WallClock observed_at;
  Millis validity;
  SourceReference source;
  ActorId producer;
  Note note;
  PlanId plan;
  PlanRevision plan_revision;
  ObservationSequence sequence;
  EvidencePayload payload;
  Digest record_digest;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest compute_digest() const;
  [[nodiscard]] bool has_consistent_payload() const noexcept;
};

// Why one record did or did not contribute to a readiness answer.
enum class EvidenceEligibility : std::uint8_t {
  Eligible = 1,
  Superseded = 2,
  Expired = 3,
  NotYetObserved = 4,
  BindingMismatch = 5,
};

[[nodiscard]] RACK_TURNUP_API std::string_view evidence_eligibility_name(
    EvidenceEligibility eligibility) noexcept;

struct EvidenceAssessment {
  EvidenceId id;
  SubsystemKind subsystem = SubsystemKind::Power;
  ObservationSequence sequence;
  WallClock observed_at;
  EvidenceEligibility eligibility = EvidenceEligibility::Eligible;
  ErrorCode code = ErrorCode::Ok;
  std::string explanation;
};

// The outcome of reducing a rack's evidence to what may be used.
struct EvidenceSelection {
  // Newest eligible record per subsystem, or null when no eligible record
  // exists for that subsystem. Indexed by subsystem value minus one.
  std::array<const EvidenceRecord*, kSubsystemKindCount> latest{};
  std::vector<EvidenceAssessment> assessments;
  std::size_t eligible_count = 0;
  std::size_t superseded_count = 0;
  std::size_t rejected_count = 0;

  [[nodiscard]] const EvidenceRecord* for_subsystem(SubsystemKind kind) const noexcept;
};

// Freshness and binding of one record at an authority time, without the
// newest-wins rule.
[[nodiscard]] RACK_TURNUP_API EvidenceAssessment assess_evidence(const EvidenceRecord& record,
                                                                 const PlanBinding& binding,
                                                                 WallClock authority_time);

// Reduces a rack's evidence to the newest eligible record per subsystem. The
// assessment list is ordered by subsystem, then observation sequence, then
// evidence identity, so two runs over the same records produce the same report.
[[nodiscard]] RACK_TURNUP_API EvidenceSelection select_evidence(
    const std::vector<EvidenceRecord>& records, const PlanBinding& binding,
    WallClock authority_time);

}  // namespace rackturnup
