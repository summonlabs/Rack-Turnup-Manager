// Rack Turnup Manager - evidence records, eligibility and selection.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/evidence.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

constexpr std::uint32_t kEvidenceRecordModel = 1;

[[nodiscard]] std::size_t subsystem_slot(SubsystemKind kind) noexcept {
  const auto value = static_cast<std::size_t>(kind);
  return (value >= 1 && value <= kSubsystemKindCount) ? (value - 1) : 0;
}

[[nodiscard]] EvidenceAssessment rejected(const EvidenceRecord& record,
                                          EvidenceEligibility eligibility, ErrorCode code,
                                          std::string explanation) {
  EvidenceAssessment assessment;
  assessment.id = record.id;
  assessment.subsystem = record.subsystem;
  assessment.sequence = record.sequence;
  assessment.observed_at = record.observed_at;
  assessment.eligibility = eligibility;
  assessment.code = code;
  assessment.explanation = std::move(explanation);
  return assessment;
}

}  // namespace

void EvidenceRecord::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kEvidenceRecordModel);
  hasher.update_len_text(id.text());
  hasher.update_len_text(rack.text());
  hasher.update_byte(static_cast<std::uint8_t>(subsystem));
  stamp.update_digest(hasher);
  const std::vector<std::uint8_t>& composition_bytes = composition.bytes();
  hasher.update(composition_bytes.data(), composition_bytes.size());
  hasher.update_u64(static_cast<std::uint64_t>(observed_at.unix_milliseconds()));
  hasher.update_u64(static_cast<std::uint64_t>(validity.milliseconds()));
  hasher.update_len_text(source.text());
  hasher.update_len_text(producer.text());
  hasher.update_len_text(note.text());
  hasher.update_len_text(plan.text());
  hasher.update_u64(plan_revision.value());
  hasher.update_u64(sequence.value());
  payload.update_digest(hasher);
}

Digest EvidenceRecord::compute_digest() const {
  Sha256 hasher;
  update_digest(hasher);
  return hasher.finish();
}

bool EvidenceRecord::has_consistent_payload() const noexcept {
  return payload.is_consistent() && subsystem == subsystem_for_payload_kind(payload.kind);
}

std::string_view evidence_eligibility_name(EvidenceEligibility eligibility) noexcept {
  switch (eligibility) {
    case EvidenceEligibility::Eligible:
      return "eligible";
    case EvidenceEligibility::Superseded:
      return "superseded";
    case EvidenceEligibility::Expired:
      return "expired";
    case EvidenceEligibility::NotYetObserved:
      return "not_yet_observed";
    case EvidenceEligibility::BindingMismatch:
      return "binding_mismatch";
  }
  return "eligibility_unknown";
}

const EvidenceRecord* EvidenceSelection::for_subsystem(SubsystemKind kind) const noexcept {
  return latest[subsystem_slot(kind)];
}

EvidenceAssessment assess_evidence(const EvidenceRecord& record, const PlanBinding& binding,
                                   WallClock authority_time) {
  if (!record.has_consistent_payload()) {
    return rejected(record, EvidenceEligibility::BindingMismatch, ErrorCode::CorruptState,
                    "evidence payload does not match the subsystem it claims");
  }

  if (record.record_digest.is_zero()) {
    return rejected(record, EvidenceEligibility::BindingMismatch, ErrorCode::IntegrityCheckFailed,
                    "evidence record carries no digest");
  }
  if (!digests_equal(record.record_digest, record.compute_digest())) {
    return rejected(record, EvidenceEligibility::BindingMismatch, ErrorCode::IntegrityCheckFailed,
                    "evidence record digest does not match its contents");
  }

  const std::vector<GenerationMismatch> mismatches = record.stamp.compare(binding.stamp);
  if (!mismatches.empty()) {
    return rejected(record, EvidenceEligibility::BindingMismatch,
                    ErrorCode::EvidenceBindingMismatch,
                    "evidence was observed in a different generation set: " +
                        describe_generation_mismatches(mismatches));
  }
  if (!digests_equal(record.composition, binding.composition)) {
    return rejected(record, EvidenceEligibility::BindingMismatch,
                    ErrorCode::CompositionDigestMismatch,
                    "evidence was observed against a different rack composition");
  }

  const FreshnessVerdict freshness =
      evaluate_freshness(record.observed_at, record.validity, authority_time);
  if (freshness == FreshnessVerdict::NotYetObserved) {
    return rejected(record, EvidenceEligibility::NotYetObserved, ErrorCode::InvalidTimestamp,
                    "evidence was observed after the authority time it is evaluated at");
  }
  if (freshness == FreshnessVerdict::Expired) {
    return rejected(record, EvidenceEligibility::Expired, ErrorCode::EvidenceStale,
                    "evidence validity window closed at " + record.observed_at.to_text() +
                        " plus " + record.validity.to_text());
  }

  EvidenceAssessment assessment;
  assessment.id = record.id;
  assessment.subsystem = record.subsystem;
  assessment.sequence = record.sequence;
  assessment.observed_at = record.observed_at;
  assessment.eligibility = EvidenceEligibility::Eligible;
  assessment.code = ErrorCode::Ok;
  return assessment;
}

EvidenceSelection select_evidence(const std::vector<EvidenceRecord>& records,
                                  const PlanBinding& binding, WallClock authority_time) {
  EvidenceSelection selection;

  std::vector<const EvidenceRecord*> ordered;
  ordered.reserve(records.size());
  for (const EvidenceRecord& record : records) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const EvidenceRecord* left, const EvidenceRecord* right) {
              if (left->subsystem != right->subsystem) {
                return left->subsystem < right->subsystem;
              }
              if (left->sequence != right->sequence) {
                return left->sequence < right->sequence;
              }
              if (left->observed_at != right->observed_at) {
                return left->observed_at < right->observed_at;
              }
              return byte_less(left->id.text(), right->id.text());
            });

  selection.assessments.reserve(ordered.size());
  for (const EvidenceRecord* record : ordered) {
    selection.assessments.push_back(assess_evidence(*record, binding, authority_time));
  }

  // Within one subsystem the ordering above is ascending by observation
  // sequence, then observation time, then evidence identity, so the last
  // eligible record is the newest one.
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (selection.assessments[i].eligibility == EvidenceEligibility::Eligible) {
      selection.latest[subsystem_slot(ordered[i]->subsystem)] = ordered[i];
    }
  }

  for (std::size_t i = 0; i < ordered.size(); ++i) {
    EvidenceAssessment& assessment = selection.assessments[i];
    if (assessment.eligibility == EvidenceEligibility::Eligible) {
      if (selection.latest[subsystem_slot(ordered[i]->subsystem)] != ordered[i]) {
        assessment.eligibility = EvidenceEligibility::Superseded;
        assessment.code = ErrorCode::EvidenceSuperseded;
        assessment.explanation = "newer evidence for this subsystem replaced this record";
        ++selection.superseded_count;
      } else {
        ++selection.eligible_count;
      }
    } else {
      ++selection.rejected_count;
    }
  }

  return selection;
}

}  // namespace rackturnup
