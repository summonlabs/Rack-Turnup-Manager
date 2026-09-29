// Rack Turnup Manager - readiness evaluation, stage ladder and blockers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/evaluation.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "rack_turnup/text.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

[[nodiscard]] std::size_t slot_of(SubsystemKind kind) noexcept {
  const auto value = static_cast<std::size_t>(kind);
  return (value >= 1 && value <= kSubsystemKindCount) ? (value - 1) : 0;
}

[[nodiscard]] int severity_rank(BlockerSeverity severity) noexcept {
  return static_cast<int>(severity);
}

void sort_and_cap_blockers(Evaluation& evaluation) {
  std::sort(evaluation.blockers.begin(), evaluation.blockers.end(), blocker_precedes);
  // Deduplicate exact repeats, keeping the first in the total order.
  std::vector<Blocker> unique_blockers;
  unique_blockers.reserve(evaluation.blockers.size());
  for (const Blocker& blocker : evaluation.blockers) {
    if (!unique_blockers.empty()) {
      const Blocker& previous = unique_blockers.back();
      if (previous.code == blocker.code && previous.stage == blocker.stage &&
          previous.subject == blocker.subject && previous.explanation == blocker.explanation &&
          previous.items == blocker.items) {
        continue;
      }
    }
    unique_blockers.push_back(blocker);
  }
  if (unique_blockers.size() > kMaxBlockersPerReport) {
    evaluation.suppressed_blockers = unique_blockers.size() - kMaxBlockersPerReport;
    unique_blockers.resize(kMaxBlockersPerReport);
  }
  evaluation.blockers = std::move(unique_blockers);
}

[[nodiscard]] Blocker make_blocker(BlockerSeverity severity, ErrorCode code, StageKind stage,
                                   SubsystemKind subsystem, std::string subject,
                                   std::string explanation, std::vector<std::string> items = {}) {
  Blocker blocker;
  blocker.severity = severity;
  blocker.code = code;
  blocker.stage = stage;
  blocker.has_subsystem = stage_has_subsystem(stage);
  blocker.subsystem = subsystem;
  blocker.subject = std::move(subject);
  blocker.explanation = std::move(explanation);
  blocker.items = std::move(items);
  return blocker;
}

[[nodiscard]] std::string counter_text(std::uint64_t value) { return std::to_string(value); }

// ---------------------------------------------------------------------------
// Per-subsystem rules
// ---------------------------------------------------------------------------

void evaluate_identity_subsystem(const RackComposition& composition,
                                 const IdentityCompositionObservation& observation,
                                 SubsystemVerdict& verdict) {
  if (!digests_equal(observation.registry_composition, composition.digest())) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::CompositionDigestMismatch;
    verdict.explanation = "the registry reports a composition digest the plan is not bound to";
    verdict.items.push_back("registry=" + observation.registry_composition.to_hex());
    verdict.items.push_back("bound=" + composition.digest().to_hex());
    return;
  }
  if (observation.unreadable_members != 0) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::InventoryUnreadable;
    verdict.explanation = "the registry could not read every member of the rack";
    verdict.items.push_back("unreadable_members=" + counter_text(observation.unreadable_members));
    return;
  }
  if (observation.declared_members != composition.size()) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::SnapshotIdentityMismatch;
    verdict.explanation = "the registry reports a different member count for the bound digest";
    verdict.items.push_back("reported=" + counter_text(observation.declared_members));
    verdict.items.push_back("expected=" + counter_text(composition.size()));
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "the registry composition digest matches the plan binding";
}

void evaluate_power_subsystem(const PlanRequirements& requirements, const PowerObservation& observation,
                              SubsystemVerdict& verdict) {
  verdict.items.push_back("available_milliwatts=" + counter_text(observation.available_milliwatts));
  verdict.items.push_back("applied_milliwatts=" + counter_text(observation.applied_milliwatts));
  verdict.items.push_back("required_milliwatts=" + counter_text(requirements.required_power_milliwatts));
  if (requirements.require_applied_power_verification && !observation.applied_verified) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::ReadinessUnknown;
    verdict.explanation = "applied power was not verified by measurement";
    return;
  }
  if (requirements.required_power_milliwatts > 0 && observation.energized_circuits == 0) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::HeadroomInsufficient;
    verdict.explanation = "no circuit is energized, so power is available but not applied";
    return;
  }
  if (observation.applied_milliwatts < requirements.required_power_milliwatts) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::HeadroomInsufficient;
    if (observation.available_milliwatts >= requirements.required_power_milliwatts) {
      verdict.explanation = "power is available but the applied power is below the plan requirement";
    } else {
      verdict.explanation = "applied power is below the plan requirement";
    }
    return;
  }
  if (requirements.require_redundant_feeds && !observation.redundant_feeds_present) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::HeadroomInsufficient;
    verdict.explanation = "redundant power feeds are required but were not observed";
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "applied power meets the plan requirement";
}

void evaluate_cooling_subsystem(const PlanRequirements& requirements,
                                const CoolingObservation& observation, SubsystemVerdict& verdict) {
  verdict.items.push_back("capacity_milliwatts=" + counter_text(observation.capacity_milliwatts));
  verdict.items.push_back("delivered_milliwatts=" + counter_text(observation.delivered_milliwatts));
  verdict.items.push_back("required_milliwatts=" + counter_text(requirements.required_cooling_milliwatts));
  if (requirements.require_cooling_delivery_verification && !observation.delivery_verified) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::ReadinessUnknown;
    verdict.explanation = "cooling delivery was not verified by measurement";
    return;
  }
  if (observation.delivered_milliwatts < requirements.required_cooling_milliwatts) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::HeadroomInsufficient;
    if (observation.capacity_milliwatts >= requirements.required_cooling_milliwatts) {
      verdict.explanation =
          "cooling capacity exists but the delivered cooling is below the plan requirement";
    } else {
      verdict.explanation = "delivered cooling is below the plan requirement";
    }
    return;
  }
  if (observation.max_inlet_millidegrees_c != 0 &&
      observation.inlet_millidegrees_c > observation.max_inlet_millidegrees_c) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::ReadinessBlocked;
    verdict.explanation = "measured inlet temperature exceeds the declared maximum";
    verdict.items.push_back("inlet_millidegrees_c=" + counter_text(observation.inlet_millidegrees_c));
    verdict.items.push_back("max_inlet_millidegrees_c=" +
                            counter_text(observation.max_inlet_millidegrees_c));
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "delivered cooling meets the plan requirement";
}

void evaluate_network_subsystem(const PlanRequirements& requirements,
                                const NetworkObservation& observation, SubsystemVerdict& verdict) {
  verdict.items.push_back("attached_ports=" + counter_text(observation.attached_ports));
  verdict.items.push_back("reachable_ports=" + counter_text(observation.reachable_ports));
  verdict.items.push_back("required_ports=" + counter_text(requirements.required_network_ports));
  if (requirements.require_network_authority_verification && !observation.authority_verified) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::ReadinessUnknown;
    verdict.explanation = "network authority over the attachment was not verified";
    return;
  }
  if (!requirements.required_topology.empty() &&
      !(observation.topology == requirements.required_topology)) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::SnapshotIdentityMismatch;
    verdict.explanation = "the observed fabric topology is not the topology the plan requires";
    verdict.items.push_back("observed=" + observation.topology.text());
    verdict.items.push_back("required=" + requirements.required_topology.text());
    return;
  }
  if (observation.reachable_ports < requirements.required_network_ports) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::CompositionDigestMismatch;
    verdict.explanation = "reachable ports are below the plan requirement";
    if (observation.attached_ports >= requirements.required_network_ports) {
      verdict.explanation =
          "network attachment exists but reachability is below the plan requirement";
    }
    verdict.code = ErrorCode::HeadroomInsufficient;
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "reachable ports meet the plan requirement";
}

void evaluate_inventory_subsystem(const RackComposition& composition,
                                  const InventoryObservation& observation, Evaluation& evaluation,
                                  SubsystemVerdict& verdict) {
  const Result<InventoryClosure> closure = close_inventory(composition, observation);
  if (!closure.has_value()) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = closure.error().code;
    verdict.explanation = closure.error().message;
    return;
  }
  const InventoryClosure& result = closure.value();
  evaluation.observed_members = result.observed_members;
  if (result.is_closed()) {
    verdict.state = ReadinessState::Ready;
    verdict.code = ErrorCode::Ok;
    verdict.explanation = "every composition member was observed exactly once";
    return;
  }
  verdict.state = ReadinessState::Blocked;
  verdict.code = result.code;
  verdict.explanation = result.explain();
  for (const std::string& device : result.missing_devices) {
    verdict.items.push_back("missing:" + device);
  }
  for (const std::string& device : result.unknown_devices) {
    verdict.items.push_back("unknown:" + device);
  }
  for (const std::string& device : result.unreadable_devices) {
    verdict.items.push_back("unreadable:" + device);
  }
  for (const std::string& device : result.displaced_devices) {
    verdict.items.push_back("displaced:" + device);
  }
  for (const std::string& slot : result.duplicate_slots) {
    verdict.items.push_back("duplicate_slot:" + slot);
  }
}

void evaluate_health_subsystem(const RackComposition& composition, const PlanRequirements& requirements,
                               const HealthObservation& observation, Evaluation& evaluation,
                               SubsystemVerdict& verdict) {
  std::vector<std::string> failing;
  std::vector<std::string> warning;
  std::vector<std::string> unknown;
  std::vector<std::string> absent;
  std::size_t passing = 0;
  for (const CompositionMember& member : composition.members()) {
    const DeviceHealthSample* sample = nullptr;
    for (const DeviceHealthSample& candidate : observation.samples) {
      if (candidate.device == member.device) {
        sample = &candidate;
        break;
      }
    }
    if (sample == nullptr) {
      absent.push_back(member.device.text());
      continue;
    }
    switch (sample->status) {
      case HealthStatus::Passing:
        ++passing;
        break;
      case HealthStatus::Warning:
        warning.push_back(member.device.text());
        break;
      case HealthStatus::Failing:
        failing.push_back(member.device.text());
        break;
      case HealthStatus::Unknown:
        unknown.push_back(member.device.text());
        break;
    }
  }
  evaluation.healthy_members = HealthyDeviceCount::create(passing).value();

  if (!failing.empty()) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::HealthFailed;
    verdict.explanation = "health evidence reports failing members";
    for (const std::string& device : failing) {
      verdict.items.push_back("failing:" + device);
    }
    return;
  }

  const bool prove_every_member = requirements.require_health_pass_for_every_member;
  const bool incomplete = !absent.empty() || !unknown.empty() || observation.unchecked_devices != 0;
  if (incomplete && prove_every_member) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::EvidenceMissing;
    verdict.explanation = "health is not proven for every member of the composition";
    for (const std::string& device : absent) {
      verdict.items.push_back("no_sample:" + device);
    }
    for (const std::string& device : unknown) {
      verdict.items.push_back("unknown:" + device);
    }
    if (observation.unchecked_devices != 0) {
      verdict.items.push_back("unchecked=" + counter_text(observation.unchecked_devices));
    }
    return;
  }
  if (!warning.empty()) {
    verdict.state = ReadinessState::Degraded;
    verdict.code = ErrorCode::HealthDegraded;
    verdict.explanation = "health evidence reports members with warnings";
    for (const std::string& device : warning) {
      verdict.items.push_back("warning:" + device);
    }
    return;
  }
  if (incomplete) {
    verdict.state = ReadinessState::Degraded;
    verdict.code = ErrorCode::HealthDegraded;
    verdict.explanation =
        "the plan does not require health proof for every member, and some members are unproven";
    for (const std::string& device : absent) {
      verdict.items.push_back("no_sample:" + device);
    }
    for (const std::string& device : unknown) {
      verdict.items.push_back("unknown:" + device);
    }
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "every member reports passing health";
}

enum class RequirementOutcome : std::uint8_t {
  Satisfied = 0,
  Unsatisfied = 1,
  Unknown = 2,
};

[[nodiscard]] const CompatibilityMembership* find_membership(
    const CompatibilityObservation& observation, const DeviceId& device) noexcept {
  for (const CompatibilityMembership& membership : observation.members) {
    if (membership.device == device) {
      return &membership;
    }
  }
  return nullptr;
}

[[nodiscard]] RequirementOutcome evaluate_requirement(const RackComposition& composition,
                                                     const CompatibilityObservation& observation,
                                                     const CompatibilityRequirement& requirement,
                                                     std::string& explanation) {
  switch (requirement.kind) {
    case RequirementKind::TraitOnEveryMember: {
      bool missing_data = false;
      for (const CompositionMember& member : composition.members()) {
        const CompatibilityMembership* membership = find_membership(observation, member.device);
        if (membership == nullptr) {
          missing_data = true;
          continue;
        }
        if (!membership->traits.contains(requirement.trait)) {
          explanation = member.device.text() + " does not declare " + requirement.trait.text();
          return RequirementOutcome::Unsatisfied;
        }
      }
      if (missing_data) {
        explanation = "compatibility was not observed for every member";
        return RequirementOutcome::Unknown;
      }
      explanation = "every member declares " + requirement.trait.text();
      return RequirementOutcome::Satisfied;
    }
    case RequirementKind::TraitOnAnyMember: {
      bool missing_data = false;
      for (const CompositionMember& member : composition.members()) {
        const CompatibilityMembership* membership = find_membership(observation, member.device);
        if (membership == nullptr) {
          missing_data = true;
          continue;
        }
        if (membership->traits.contains(requirement.trait)) {
          explanation = member.device.text() + " declares " + requirement.trait.text();
          return RequirementOutcome::Satisfied;
        }
      }
      if (missing_data) {
        explanation = "compatibility was not observed for every member";
        return RequirementOutcome::Unknown;
      }
      explanation = "no member declares " + requirement.trait.text();
      return RequirementOutcome::Unsatisfied;
    }
    case RequirementKind::TraitOnDevice: {
      if (composition.find(requirement.device) == nullptr) {
        explanation = requirement.device.text() + " is not a member of the composition";
        return RequirementOutcome::Unsatisfied;
      }
      const CompatibilityMembership* membership =
          find_membership(observation, requirement.device);
      if (membership == nullptr) {
        explanation = "compatibility was not observed for " + requirement.device.text();
        return RequirementOutcome::Unknown;
      }
      if (!membership->traits.contains(requirement.trait)) {
        explanation = requirement.device.text() + " does not declare " + requirement.trait.text();
        return RequirementOutcome::Unsatisfied;
      }
      explanation = requirement.device.text() + " declares " + requirement.trait.text();
      return RequirementOutcome::Satisfied;
    }
    case RequirementKind::BaselineOnEveryMember: {
      bool missing_data = false;
      for (const CompositionMember& member : composition.members()) {
        const CompatibilityMembership* membership = find_membership(observation, member.device);
        if (membership == nullptr) {
          missing_data = true;
          continue;
        }
        if (!(membership->firmware_baseline == requirement.firmware_baseline)) {
          explanation = member.device.text() + " runs baseline " +
                        (membership->firmware_baseline.empty() ? std::string("<none>")
                                                              : membership->firmware_baseline.text());
          return RequirementOutcome::Unsatisfied;
        }
      }
      if (missing_data) {
        explanation = "compatibility was not observed for every member";
        return RequirementOutcome::Unknown;
      }
      explanation = "every member runs baseline " + requirement.firmware_baseline.text();
      return RequirementOutcome::Satisfied;
    }
    case RequirementKind::BaselineOnDevice: {
      if (composition.find(requirement.device) == nullptr) {
        explanation = requirement.device.text() + " is not a member of the composition";
        return RequirementOutcome::Unsatisfied;
      }
      const CompatibilityMembership* membership =
          find_membership(observation, requirement.device);
      if (membership == nullptr) {
        explanation = "compatibility was not observed for " + requirement.device.text();
        return RequirementOutcome::Unknown;
      }
      if (!(membership->firmware_baseline == requirement.firmware_baseline)) {
        explanation = requirement.device.text() + " runs a different firmware baseline";
        return RequirementOutcome::Unsatisfied;
      }
      explanation = requirement.device.text() + " runs baseline " +
                    requirement.firmware_baseline.text();
      return RequirementOutcome::Satisfied;
    }
  }
  explanation = "requirement kind is not understood";
  return RequirementOutcome::Unknown;
}

void evaluate_compatibility_subsystem(const RackComposition& composition,
                                      const PlanRequirements& requirements,
                                      const CompatibilityObservation& observation,
                                      SubsystemVerdict& verdict) {
  if (requirements.compatibility.empty()) {
    verdict.state = ReadinessState::Ready;
    verdict.code = ErrorCode::Ok;
    verdict.explanation = "the plan declares no compatibility requirements";
    return;
  }
  std::vector<std::string> blocked;
  std::vector<std::string> unproven;
  std::vector<std::string> advisory;
  for (const CompatibilityRequirement& requirement : requirements.compatibility) {
    std::string explanation;
    const RequirementOutcome outcome =
        evaluate_requirement(composition, observation, requirement, explanation);
    if (outcome == RequirementOutcome::Satisfied) {
      continue;
    }
    const std::string entry = requirement.to_text() + " => " + explanation;
    if (!requirement.mandatory) {
      advisory.push_back(entry);
    } else if (outcome == RequirementOutcome::Unsatisfied) {
      blocked.push_back(entry);
    } else {
      unproven.push_back(entry);
    }
  }
  if (!blocked.empty()) {
    verdict.state = ReadinessState::Blocked;
    verdict.code = ErrorCode::CompatibilityUnsatisfied;
    verdict.explanation = "mandatory compatibility requirements are not satisfied";
    verdict.items = std::move(blocked);
    return;
  }
  if (!unproven.empty()) {
    verdict.state = ReadinessState::Unknown;
    verdict.code = ErrorCode::ReadinessUnknown;
    verdict.explanation = "mandatory compatibility requirements are not proven";
    verdict.items = std::move(unproven);
    return;
  }
  if (!advisory.empty()) {
    verdict.state = ReadinessState::Degraded;
    verdict.code = ErrorCode::CompatibilityAdvisory;
    verdict.explanation = "advisory compatibility requirements are not satisfied";
    verdict.items = std::move(advisory);
    return;
  }
  verdict.state = ReadinessState::Ready;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "every compatibility requirement is satisfied";
}

}  // namespace

bool blocker_precedes(const Blocker& left, const Blocker& right) noexcept {
  const int left_rank = severity_rank(left.severity);
  const int right_rank = severity_rank(right.severity);
  if (left_rank != right_rank) {
    return left_rank > right_rank;
  }
  if (left.stage != right.stage) {
    return left.stage < right.stage;
  }
  if (left.has_subsystem != right.has_subsystem) {
    return !left.has_subsystem;
  }
  if (left.subsystem != right.subsystem) {
    return left.subsystem < right.subsystem;
  }
  if (left.subject != right.subject) {
    return byte_less(left.subject, right.subject);
  }
  return code_value(left.code) < code_value(right.code);
}

std::string_view blocker_severity_name(BlockerSeverity severity) noexcept {
  switch (severity) {
    case BlockerSeverity::Advisory:
      return "advisory";
    case BlockerSeverity::Unknown:
      return "unknown";
    case BlockerSeverity::Blocking:
      return "blocking";
  }
  return "blocking";
}

std::string BlockerReport::explain() const {
  std::string out = "rack ";
  out.append(rack.text());
  out.append(" is ");
  out.append(turnup_verdict_name(verdict));
  if (blockers.empty()) {
    out.append(" with no blocking findings");
    return out;
  }
  out.append("; primary blocker: ");
  out.append(code_name(blockers.front().code));
  out.push_back(' ');
  out.append(blockers.front().explanation);
  if (blockers.size() > 1) {
    out.append("; plus ");
    out.append(std::to_string(blockers.size() - 1));
    out.append(" further findings");
  }
  if (suppressed != 0) {
    out.append("; ");
    out.append(std::to_string(suppressed));
    out.append(" findings suppressed by the report bound");
  }
  return out;
}

std::string explain_verdict(TurnupVerdict verdict) {
  switch (verdict) {
    case TurnupVerdict::Blocked:
      return "a subsystem is proven not ready for turnup";
    case TurnupVerdict::NotReady:
      return "readiness is not proven, so turnup cannot be authorized yet";
    case TurnupVerdict::ReadyToAuthorize:
      return "every readiness stage is satisfied, so turnup may be authorized now";
    case TurnupVerdict::Authorized:
      return "turnup is authorized and the rack has not been observed active yet";
    case TurnupVerdict::ActiveObserved:
      return "the rack was observed active after the authorized action";
    case TurnupVerdict::Commissioned:
      return "the rack is commissioned";
    case TurnupVerdict::FencedAuthority:
      return "the previous turnup authority was fenced and a new plan revision is required";
  }
  return "readiness is not proven";
}

std::string describe_subsystem_verdict(const SubsystemVerdict& verdict) {
  std::string out = std::string(subsystem_kind_name(verdict.subsystem));
  out.append("=");
  out.append(readiness_state_name(verdict.state));
  if (verdict.has_evidence) {
    out.append(" evidence=");
    out.append(verdict.evidence.text());
    out.append(" observed_at=");
    out.append(verdict.observed_at.to_text());
  } else {
    out.append(" evidence=none");
  }
  out.append(" code=");
  out.append(code_name(verdict.code));
  return out;
}

const SubsystemVerdict& Evaluation::subsystem(SubsystemKind kind) const noexcept {
  return subsystems[slot_of(kind)];
}

const StageVerdict& Evaluation::stage(StageKind kind) const noexcept {
  const std::size_t index = stage_index(kind);
  return stages[index < stages.size() ? index : 0];
}

bool Evaluation::ready_to_authorize() const noexcept {
  for (const StageVerdict& stage_verdict : stages) {
    if (!stage_has_subsystem(stage_verdict.stage)) {
      break;
    }
    if (stage_verdict.state == StageState::Satisfied) {
      continue;
    }
    if (stage_verdict.state == StageState::Degraded && allow_degraded) {
      continue;
    }
    return false;
  }
  return binding_matches;
}

BlockerReport Evaluation::to_report() const {
  BlockerReport report;
  report.rack = rack;
  report.plan = plan;
  report.revision = revision;
  report.authority_time = authority_time;
  report.verdict = verdict;
  report.blockers = blockers;
  report.suppressed = suppressed_blockers;
  return report;
}

Digest compute_evidence_set_digest(const EvidenceSelection& selection, const PlanBinding& binding) {
  Sha256 hasher;
  hasher.update_u32(kEvidenceSetDigestModel);
  binding.update_digest(hasher);
  for (const SubsystemKind kind : all_subsystem_kinds()) {
    const EvidenceRecord* record = selection.for_subsystem(kind);
    if (record == nullptr) {
      hasher.update_byte(0u);
      continue;
    }
    hasher.update_byte(1u);
    hasher.update_len_text(record->id.text());
    hasher.update_u64(record->sequence.value());
    hasher.update_u64(static_cast<std::uint64_t>(record->observed_at.unix_milliseconds()));
    hasher.update_u64(static_cast<std::uint64_t>(record->validity.milliseconds()));
    hasher.update_byte(static_cast<std::uint8_t>(record->payload.kind));
    const std::vector<std::uint8_t>& bytes = record->record_digest.bytes();
    hasher.update(bytes.data(), bytes.size());
  }
  return hasher.finish();
}

Digest compute_verdict_digest(const Evaluation& evaluation) {
  Sha256 hasher;
  hasher.update_u32(kVerdictDigestModel);
  hasher.update_len_text(evaluation.rack.text());
  hasher.update_len_text(evaluation.plan.text());
  hasher.update_u64(evaluation.lifetime.value());
  hasher.update_u64(evaluation.revision.value());
  hasher.update_u64(evaluation.policy.value());
  hasher.update_u64(evaluation.epoch.value());
  hasher.update_u64(evaluation.rack_generation.value());
  hasher.update_u64(evaluation.composition_generation.value());
  const std::vector<std::uint8_t>& composition_bytes = evaluation.composition.bytes();
  hasher.update(composition_bytes.data(), composition_bytes.size());
  const std::vector<std::uint8_t>& requirements_bytes = evaluation.requirements.bytes();
  hasher.update(requirements_bytes.data(), requirements_bytes.size());
  const std::vector<std::uint8_t>& evidence_bytes = evaluation.evidence_set.bytes();
  hasher.update(evidence_bytes.data(), evidence_bytes.size());
  hasher.update_byte(static_cast<std::uint8_t>(evaluation.verdict));
  hasher.update_byte(evaluation.binding_matches ? 1u : 0u);
  hasher.update_byte(evaluation.allow_degraded ? 1u : 0u);
  hasher.update_u64(static_cast<std::uint64_t>(evaluation.authority_time.unix_milliseconds()));
  for (const StageVerdict& stage : evaluation.stages) {
    hasher.update_byte(static_cast<std::uint8_t>(stage.stage));
    hasher.update_byte(static_cast<std::uint8_t>(stage.state));
    hasher.update_u32(static_cast<std::uint32_t>(stage.code));
  }
  const Blocker* primary = evaluation.primary_blocker();
  hasher.update_u32(primary == nullptr ? 0u : static_cast<std::uint32_t>(primary->code));
  hasher.update_u64(primary == nullptr ? 0u : static_cast<std::uint64_t>(primary->stage));
  return hasher.finish();
}


namespace {

// Newest record for one plan, ordered by issue time and then by identity so
// that two records issued at the same instant still have a deterministic order.
[[nodiscard]] const TurnupAuthorization* newest_authorization(
    const std::vector<TurnupAuthorization>& authorizations, const PlanId& plan) noexcept {
  const TurnupAuthorization* best = nullptr;
  for (const TurnupAuthorization& candidate : authorizations) {
    if (!(candidate.plan == plan)) {
      continue;
    }
    if (best == nullptr || best->issued_at < candidate.issued_at ||
        (best->issued_at == candidate.issued_at &&
         byte_less(best->attempt.text(), candidate.attempt.text()))) {
      best = &candidate;
    }
  }
  return best;
}

[[nodiscard]] const ActivationRecord* newest_activation(
    const std::vector<ActivationRecord>& activations, const AttemptId& attempt) noexcept {
  const ActivationRecord* best = nullptr;
  for (const ActivationRecord& candidate : activations) {
    if (!(candidate.attempt == attempt)) {
      continue;
    }
    if (best == nullptr || best->sequence < candidate.sequence ||
        (best->sequence == candidate.sequence && best->observed_at < candidate.observed_at)) {
      best = &candidate;
    }
  }
  return best;
}

[[nodiscard]] const CommissionRecord* find_commission(
    const std::vector<CommissionRecord>& commissions, const AttemptId& attempt) noexcept {
  for (const CommissionRecord& candidate : commissions) {
    if (candidate.attempt == attempt) {
      return &candidate;
    }
  }
  return nullptr;
}

void evaluate_authorization_stage(const Evaluation& evaluation, const PlanBinding& binding,
                                  ObservationSequence evidence_high_water,
                                  const TurnupAuthorization* authorization,
                                  StageVerdict& verdict) {
  if (authorization == nullptr) {
    verdict.state = StageState::Pending;
    verdict.code = ErrorCode::TurnupNotAuthorized;
    verdict.explanation = "no turnup authorization has been issued for this plan";
    return;
  }
  switch (authorization->state) {
    case AuthorizationState::Fenced:
      verdict.state = StageState::Blocked;
      verdict.code = ErrorCode::TurnupAuthorityFenced;
      verdict.explanation = "the turnup authorization was fenced and can never be used again";
      return;
    case AuthorizationState::Cancelled:
      verdict.state = StageState::Blocked;
      verdict.code = ErrorCode::TurnupNotAuthorized;
      verdict.explanation = "the turnup authorization was cancelled";
      return;
    case AuthorizationState::Consumed:
      verdict.state = StageState::Satisfied;
      verdict.code = ErrorCode::Ok;
      verdict.explanation = "the turnup authorization was consumed by an observed activation";
      return;
    case AuthorizationState::None:
      verdict.state = StageState::Unknown;
      verdict.code = ErrorCode::TurnupNotAuthorized;
      verdict.explanation = "the stored authorization has no state";
      return;
    case AuthorizationState::Active:
      break;
  }

  if (authorization->epoch != binding.epoch) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::StaleAuthorityEpoch;
    verdict.explanation = "the authorization was issued under an older control epoch";
    return;
  }
  if (!digests_equal(authorization->composition, evaluation.composition)) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::CompositionDigestMismatch;
    verdict.explanation = "the authorization was issued against a different rack composition";
    return;
  }
  if (authorization->revision != evaluation.revision) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::StalePlanRevision;
    verdict.explanation = "the plan advanced after the authorization was issued";
    return;
  }
  if (authorization->evidence_high_water != evidence_high_water) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::AuthoritySuperseded;
    verdict.explanation = "evidence was imported after the authorization was issued";
    return;
  }
  const FreshnessVerdict freshness =
      evaluate_freshness(authorization->issued_at, authorization->validity, evaluation.authority_time);
  if (freshness == FreshnessVerdict::NotYetObserved) {
    verdict.state = StageState::Unknown;
    verdict.code = ErrorCode::InvalidTimestamp;
    verdict.explanation = "the authorization was issued after the authority time";
    return;
  }
  if (freshness == FreshnessVerdict::Expired) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::TurnupNotAuthorized;
    verdict.explanation = "the turnup authorization expired before the authority time";
    return;
  }
  verdict.state = StageState::Satisfied;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "turnup is authorized for this exact composition and revision";
}

void evaluate_activation_stage(const Evaluation& evaluation, const TurnupAuthorization* authorization,
                               const ActivationRecord* activation, StageVerdict& verdict) {
  if (authorization == nullptr) {
    verdict.state = StageState::Pending;
    verdict.code = ErrorCode::ActivationNotObserved;
    verdict.explanation = "no authorization exists, so no activation can be observed";
    return;
  }
  if (activation == nullptr) {
    verdict.state = StageState::Pending;
    verdict.code = ErrorCode::ActivationNotObserved;
    verdict.explanation = "no post-action activation observation has been recorded";
    return;
  }
  if (!digests_equal(activation->authorization, authorization->authorization_digest)) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::ActivationAttemptConflict;
    verdict.explanation = "the activation observation names a different authorization";
    return;
  }
  if (activation->observed_at <= authorization->issued_at) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::InvalidTimestamp;
    verdict.explanation = "the activation observation predates the authorization it claims";
    return;
  }
  if (activation->sequence <= authorization->evidence_high_water) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::SequenceRegression;
    verdict.explanation = "the activation observation is not newer than the evidence it claims";
    return;
  }
  if (!digests_equal(activation->observed_composition, evaluation.composition)) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::CompositionDigestMismatch;
    verdict.explanation = "the activation observation describes a different rack composition";
    return;
  }
  const FreshnessVerdict freshness =
      evaluate_freshness(activation->observed_at, activation->validity, evaluation.authority_time);
  if (freshness == FreshnessVerdict::NotYetObserved) {
    verdict.state = StageState::Unknown;
    verdict.code = ErrorCode::InvalidTimestamp;
    verdict.explanation = "the activation observation is newer than the authority time";
    return;
  }
  if (freshness == FreshnessVerdict::Expired) {
    verdict.state = StageState::Unknown;
    verdict.code = ErrorCode::EvidenceStale;
    verdict.explanation = "the activation observation has expired at the authority time";
    return;
  }
  switch (activation->outcome) {
    case ActivationOutcome::Active:
      verdict.state = StageState::Satisfied;
      verdict.code = ErrorCode::Ok;
      verdict.explanation = "the post-action observation reports the rack as active";
      return;
    case ActivationOutcome::NotActive:
      verdict.state = StageState::Blocked;
      verdict.code = ErrorCode::ActivationNotConfirmed;
      verdict.explanation = "the post-action observation reports the rack as not active";
      return;
    case ActivationOutcome::Unknown:
      verdict.state = StageState::Unknown;
      verdict.code = ErrorCode::ReadinessUnknown;
      verdict.explanation = "the post-action observation could not determine the rack state";
      return;
  }
}

void evaluate_commission_stage(const ActivationRecord* activation,
                               const CommissionRecord* commission, StageVerdict& verdict) {
  if (commission == nullptr) {
    verdict.state = StageState::Pending;
    verdict.code = ErrorCode::NotCommissioned;
    verdict.explanation = "the rack is not commissioned";
    return;
  }
  if (activation == nullptr || !digests_equal(commission->activation, activation->activation_digest)) {
    verdict.state = StageState::Blocked;
    verdict.code = ErrorCode::ActivationNotObserved;
    verdict.explanation = "the commission record names an activation observation that is not present";
    return;
  }
  verdict.state = StageState::Satisfied;
  verdict.code = ErrorCode::Ok;
  verdict.explanation = "the rack is commissioned against an observed activation";
}

}  // namespace

Evaluation evaluate_rack(const EvaluationContext& context) {
  const RackComposition& composition = context.composition;
  const RackTurnupPlan& plan = context.plan;

  Evaluation evaluation;
  evaluation.rack = composition.rack();
  evaluation.site = composition.site();
  evaluation.lifecycle = context.lifecycle;
  evaluation.rack_generation = context.rack_generation;
  evaluation.composition_generation = composition.generation();
  evaluation.composition = composition.digest();
  evaluation.plan = plan.id;
  evaluation.lifetime = plan.lifetime;
  evaluation.revision = plan.revision;
  evaluation.policy = plan.policy.generation;
  evaluation.epoch = plan.binding.epoch;
  evaluation.allow_degraded = plan.policy.allow_degraded_subsystems;
  evaluation.authority_time = context.authority_time;
  evaluation.requirements = plan.requirements_digest;
  evaluation.expected_members = ExpectedDeviceCount::create(composition.size()).value();
  evaluation.evidence_high_water = plan.evidence_high_water;

  if (!(plan.binding.rack == composition.rack())) {
    evaluation.binding_problems.push_back("the plan binds rack " + plan.binding.rack.text() +
                                          " but the rack record is " + composition.rack().text());
  }
  if (!digests_equal(plan.binding.composition, composition.digest())) {
    evaluation.binding_problems.push_back("the plan binds composition " +
                                          plan.binding.composition.to_hex() + " but the rack is " +
                                          composition.digest().to_hex());
  }
  if (plan.binding.stamp.rack != context.rack_generation) {
    evaluation.binding_mismatches.push_back(GenerationMismatch{
        "rack", plan.binding.stamp.rack.value(), context.rack_generation.value()});
  }
  if (plan.binding.stamp.composition != composition.generation()) {
    evaluation.binding_mismatches.push_back(GenerationMismatch{
        "composition", plan.binding.stamp.composition.value(), composition.generation().value()});
  }
  evaluation.binding_matches =
      evaluation.binding_problems.empty() && evaluation.binding_mismatches.empty();

  const EvidenceSelection selection =
      select_evidence(context.evidence, plan.binding, context.authority_time);
  evaluation.evidence_assessments = selection.assessments;
  evaluation.eligible_evidence = selection.eligible_count;
  evaluation.superseded_evidence = selection.superseded_count;
  evaluation.rejected_evidence = selection.rejected_count;
  evaluation.evidence_set = evaluation.binding_matches
                                 ? compute_evidence_set_digest(selection, plan.binding)
                                 : Digest{};

  for (const SubsystemKind kind : all_subsystem_kinds()) {
    SubsystemVerdict verdict;
    verdict.subsystem = kind;
    const EvidenceRecord* record = evaluation.binding_matches ? selection.for_subsystem(kind) : nullptr;
    if (!evaluation.binding_matches || record == nullptr) {
      if (!evaluation.binding_matches) {
        verdict.state = ReadinessState::Unknown;
        verdict.code = ErrorCode::TurnupAuthorityFenced;
        verdict.explanation =
            "the plan binding no longer matches the rack record, so readiness is not answered";
      } else {
        verdict.state = ReadinessState::Unknown;
        verdict.code = ErrorCode::EvidenceMissing;
        verdict.explanation = "no eligible " + std::string(subsystem_kind_name(kind)) +
                              " evidence exists for the plan binding";
      }
    } else {
      verdict.has_evidence = true;
      verdict.evidence = record->id;
      verdict.sequence = record->sequence;
      verdict.observed_at = record->observed_at;
      verdict.validity = record->validity;
      verdict.freshness = FreshnessVerdict::Fresh;
      switch (kind) {
        case SubsystemKind::IdentityComposition:
          evaluate_identity_subsystem(composition, record->payload.identity, verdict);
          break;
        case SubsystemKind::Power:
          evaluate_power_subsystem(plan.requirements, record->payload.power, verdict);
          break;
        case SubsystemKind::Cooling:
          evaluate_cooling_subsystem(plan.requirements, record->payload.cooling, verdict);
          break;
        case SubsystemKind::Network:
          evaluate_network_subsystem(plan.requirements, record->payload.network, verdict);
          break;
        case SubsystemKind::Inventory:
          evaluate_inventory_subsystem(composition, record->payload.inventory, evaluation, verdict);
          break;
        case SubsystemKind::Health:
          evaluate_health_subsystem(composition, plan.requirements, record->payload.health,
                                    evaluation, verdict);
          break;
        case SubsystemKind::Compatibility:
          evaluate_compatibility_subsystem(composition, plan.requirements,
                                           record->payload.compatibility, verdict);
          break;
      }
    }
    evaluation.subsystems[slot_of(kind)] = verdict;
  }

  const TurnupAuthorization* authorization =
      newest_authorization(context.authorizations, plan.id);
  const ActivationRecord* activation =
      (authorization == nullptr) ? nullptr
                                 : newest_activation(context.activations, authorization->attempt);
  const CommissionRecord* commission =
      (authorization == nullptr) ? nullptr
                                 : find_commission(context.commissions, authorization->attempt);

  if (authorization != nullptr) {
    evaluation.authorization.present = true;
    evaluation.authorization.attempt = authorization->attempt;
    evaluation.authorization.state = authorization->state;
    evaluation.authorization.issued_at = authorization->issued_at;
    evaluation.authorization.validity = authorization->validity;
    evaluation.authorization.freshness = evaluate_freshness(
        authorization->issued_at, authorization->validity, context.authority_time);
    evaluation.authorization.verdict = authorization->verdict;
    evaluation.authorization.consumed = activation != nullptr;
  }
  evaluation.activation_observed = activation != nullptr;
  evaluation.commissioned = commission != nullptr;
  if (activation != nullptr) {
    evaluation.activation = activation->activation_digest;
  }

  for (const StageKind stage : plan_stage_ladder()) {
    StageVerdict verdict;
    verdict.stage = stage;
    if (stage_has_subsystem(stage)) {
      const SubsystemVerdict& subsystem = evaluation.subsystem(subsystem_for_stage(stage));
      verdict.state = stage_state_for_readiness(subsystem.state);
      verdict.code = subsystem.code;
      verdict.explanation = subsystem.explanation;
    } else if (stage == StageKind::TurnupAuthorization) {
      evaluate_authorization_stage(evaluation, plan.binding, plan.evidence_high_water, authorization,
                                   verdict);
    } else if (stage == StageKind::ActivationObservation) {
      evaluate_activation_stage(evaluation, authorization, activation, verdict);
    } else {
      evaluate_commission_stage(activation, commission, verdict);
    }
    evaluation.stages.push_back(verdict);
  }

  // The ladder holds a stage back when any earlier stage is not satisfied. A
  // stage that is itself blocked keeps its own finding; only a stage that would
  // otherwise be satisfied is downgraded to pending.
  for (std::size_t i = 1; i < evaluation.stages.size(); ++i) {
    const StageVerdict& previous = evaluation.stages[i - 1];
    const bool previous_passes =
        previous.state == StageState::Satisfied ||
        (previous.state == StageState::Degraded && evaluation.allow_degraded);
    if (previous_passes) {
      continue;
    }
    StageVerdict& current = evaluation.stages[i];
    current.gated_by_predecessor = true;
    if (current.state == StageState::Satisfied) {
      current.state = StageState::Pending;
      current.explanation = "held by an earlier stage: " +
                            std::string(stage_kind_name(previous.stage)) + " is " +
                            std::string(stage_state_name(previous.state));
    }
  }

  // ---------------------------------------------------------------------
  // Blockers
  // ---------------------------------------------------------------------
  if (!evaluation.binding_matches) {
    std::vector<std::string> items = evaluation.binding_problems;
    for (const GenerationMismatch& mismatch : evaluation.binding_mismatches) {
      items.push_back(mismatch.field + " expected=" + std::to_string(mismatch.expected) +
                      " actual=" + std::to_string(mismatch.actual));
    }
    evaluation.blockers.push_back(make_blocker(
        BlockerSeverity::Blocking, ErrorCode::TurnupAuthorityFenced,
        StageKind::IdentityCompositionValidation, SubsystemKind::IdentityComposition, "",
        "the plan binding no longer matches the rack record", std::move(items)));
  }

  for (const SubsystemKind kind : all_subsystem_kinds()) {
    const SubsystemVerdict& verdict = evaluation.subsystem(kind);
    if (verdict.state == ReadinessState::Ready) {
      continue;
    }
    BlockerSeverity severity = BlockerSeverity::Unknown;
    if (verdict.state == ReadinessState::Blocked) {
      severity = BlockerSeverity::Blocking;
    } else if (verdict.state == ReadinessState::Degraded) {
      severity = evaluation.allow_degraded ? BlockerSeverity::Advisory : BlockerSeverity::Blocking;
    }
    evaluation.blockers.push_back(
        make_blocker(severity, verdict.code, stage_for_subsystem(kind), kind, verdict.evidence.text(),
                     verdict.explanation, verdict.items));
  }

  for (const StageVerdict& verdict : evaluation.stages) {
    if (stage_has_subsystem(verdict.stage)) {
      continue;
    }
    if (verdict.state != StageState::Blocked && verdict.state != StageState::Unknown) {
      continue;
    }
    const BlockerSeverity severity = (verdict.state == StageState::Blocked)
                                          ? BlockerSeverity::Blocking
                                          : BlockerSeverity::Unknown;
    evaluation.blockers.push_back(make_blocker(severity, verdict.code, verdict.stage,
                                               SubsystemKind::IdentityComposition, "",
                                               verdict.explanation));
  }

  sort_and_cap_blockers(evaluation);

  // ---------------------------------------------------------------------
  // Verdict
  // ---------------------------------------------------------------------
  const StageVerdict& authorization_stage = evaluation.stage(StageKind::TurnupAuthorization);
  const StageVerdict& activation_stage = evaluation.stage(StageKind::ActivationObservation);
  const StageVerdict& commission_stage = evaluation.stage(StageKind::CommissionedState);
  // A degraded stage blocks the verdict when policy does not permit degraded
  // subsystems: the rack is then proven not ready, not merely unproven.
  const bool any_blocked = [&evaluation]() {
    for (const StageVerdict& verdict : evaluation.stages) {
      if (verdict.state == StageState::Blocked) {
        return true;
      }
      if (verdict.state == StageState::Degraded && !evaluation.allow_degraded) {
        return true;
      }
    }
    return false;
  }();

  if (!evaluation.binding_matches) {
    evaluation.verdict = TurnupVerdict::FencedAuthority;
  } else if (commission_stage.state == StageState::Satisfied) {
    evaluation.verdict = TurnupVerdict::Commissioned;
  } else if (activation_stage.state == StageState::Satisfied) {
    evaluation.verdict = TurnupVerdict::ActiveObserved;
  } else if (authorization_stage.state == StageState::Satisfied ||
             authorization_stage.state == StageState::Pending) {
    if (authorization_stage.state == StageState::Satisfied) {
      evaluation.verdict = TurnupVerdict::Authorized;
    } else if (evaluation.ready_to_authorize() && !any_blocked) {
      evaluation.verdict = TurnupVerdict::ReadyToAuthorize;
    } else if (authorization_stage.code == ErrorCode::TurnupAuthorityFenced) {
      evaluation.verdict = TurnupVerdict::FencedAuthority;
    } else {
      evaluation.verdict = TurnupVerdict::NotReady;
    }
  } else if (authorization_stage.code == ErrorCode::TurnupAuthorityFenced) {
    evaluation.verdict = TurnupVerdict::FencedAuthority;
  } else {
    evaluation.verdict = TurnupVerdict::NotReady;
  }

  if (any_blocked && evaluation.verdict != TurnupVerdict::FencedAuthority &&
      evaluation.verdict != TurnupVerdict::Commissioned) {
    evaluation.verdict = TurnupVerdict::Blocked;
  }

  evaluation.explanation = explain_verdict(evaluation.verdict);
  evaluation.verdict_digest = compute_verdict_digest(evaluation);
  return evaluation;
}

}  // namespace rackturnup

