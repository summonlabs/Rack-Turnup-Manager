// Rack Turnup Manager - readiness evaluation and stage ladder tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/evaluation.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/version.hpp"

namespace {

namespace rtm = rackturnup;
using rtmtest::must;

constexpr std::int64_t kBaseTime = 1770000000000ll;

[[nodiscard]] rtm::WallClock clock_at(std::int64_t offset_ms) {
  return rtm::WallClock(kBaseTime + offset_ms);
}

[[nodiscard]] rtm::CompositionMember make_member(const char* device, std::uint32_t unit,
                                                 std::uint32_t slot,
                                                 std::vector<const char*> traits = {},
                                                 const char* baseline = "") {
  rtm::CompositionMember member;
  member.device = must(rtm::DeviceId::parse(device));
  member.slot = must(rtm::make_slot_coordinate(unit, slot));
  if (baseline[0] != 0) {
    member.firmware_baseline = must(rtm::FirmwareBaselineId::parse(baseline));
  }
  std::vector<rtm::Trait> parsed;
  parsed.reserve(traits.size());
  for (const char* trait : traits) {
    parsed.push_back(must(rtm::Trait::parse(trait)));
  }
  member.traits = must(rtm::TraitSet::create(std::move(parsed)));
  return member;
}

struct Fixture {
  rtm::RackComposition composition;
  rtm::RackTurnupPlan plan;
  std::vector<rtm::EvidenceRecord> evidence;
  std::vector<rtm::TurnupAuthorization> authorizations;
  std::vector<rtm::ActivationRecord> activations;
  std::vector<rtm::CommissionRecord> commissions;
  rtm::RackGeneration rack_generation;
  rtm::RackLifecycleState lifecycle = rtm::RackLifecycleState::Planned;
};

[[nodiscard]] Fixture make_fixture(bool allow_degraded = false) {
  Fixture fixture;
  std::vector<rtm::CompositionMember> members;
  members.push_back(make_member("dev:a", 1, 0, {"power.ac.208v"}, "fw:b1"));
  members.push_back(make_member("dev:b", 2, 0, {"power.ac.208v"}, "fw:b1"));
  fixture.composition = must(rtm::RackComposition::create(
      must(rtm::RackId::parse("rack:r1")), must(rtm::SiteId::parse("site:s1")),
      rtm::CompositionGeneration::initial(), std::move(members)));
  fixture.rack_generation = rtm::RackGeneration::initial();

  rtm::GenerationStamp stamp;
  stamp.rack = fixture.rack_generation;
  stamp.composition = fixture.composition.generation();
  stamp.topology = must(rtm::TopologyGeneration::create(2));
  stamp.power = must(rtm::PowerGeneration::create(3));
  stamp.cooling = must(rtm::CoolingGeneration::create(4));
  stamp.network = must(rtm::NetworkGeneration::create(5));
  stamp.inventory = must(rtm::InventoryGeneration::create(6));
  stamp.health = must(rtm::HealthGeneration::create(7));

  fixture.plan.id = must(rtm::PlanId::parse("tp:1"));
  fixture.plan.lifetime = rtm::PlanLifetime::initial();
  fixture.plan.revision = rtm::PlanRevision::initial();
  fixture.plan.binding.rack = fixture.composition.rack();
  fixture.plan.binding.composition = fixture.composition.digest();
  fixture.plan.binding.stamp = stamp;
  fixture.plan.binding.epoch = rtm::ControlEpoch::initial();
  fixture.plan.policy.generation = rtm::PolicyGeneration::initial();
  fixture.plan.policy.allow_degraded_subsystems = allow_degraded;
  fixture.plan.policy.authorization_validity = rtm::Millis::minutes(30);
  fixture.plan.policy.default_evidence_validity = rtm::Millis::minutes(15);
  rtm::PlanRequirements requirements;
  requirements.required_power_milliwatts = 10000;
  requirements.required_cooling_milliwatts = 5000;
  requirements.required_network_ports = 2;
  requirements.require_redundant_feeds = true;
  requirements.required_topology = must(rtm::FabricTopologyReference::parse("topo:a"));
  requirements.compatibility.push_back(must(rtm::make_compatibility_requirement(
      rtm::RequirementKind::TraitOnEveryMember, must(rtm::Trait::parse("power.ac.208v")),
      rtm::DeviceId{}, rtm::FirmwareBaselineId{}, true, rtm::Note{})));
  fixture.plan.requirements = must(rtm::make_plan_requirements(std::move(requirements)));
  fixture.plan.state = rtm::PlanState::Active;
  fixture.plan.requirements_digest = fixture.plan.compute_requirements_digest();
  fixture.plan.created_at = clock_at(0);
  fixture.plan.created_by = must(rtm::ActorId::parse("operator"));
  return fixture;
}

[[nodiscard]] rtm::EvidenceRecord make_evidence(const Fixture& fixture, rtm::SubsystemKind subsystem,
                                                rtm::EvidencePayload payload, std::int64_t observed,
                                                std::int64_t validity, std::uint64_t sequence,
                                                rtm::GenerationStamp stamp) {
  rtm::EvidenceRecord record;
  record.id = must(rtm::EvidenceId::parse("te:" + std::to_string(sequence)));
  record.rack = fixture.composition.rack();
  record.subsystem = subsystem;
  record.stamp = stamp;
  record.composition = fixture.composition.digest();
  record.observed_at = clock_at(observed);
  record.validity = rtm::Millis(validity);
  record.source = must(rtm::SourceReference::parse("dcp:plant"));
  record.producer = must(rtm::ActorId::parse("plant"));
  record.plan = fixture.plan.id;
  record.plan_revision = fixture.plan.revision;
  record.sequence = must(rtm::ObservationSequence::create(sequence));
  record.payload = std::move(payload);
  record.record_digest = record.compute_digest();
  return record;
}

[[nodiscard]] rtm::EvidencePayload identity_payload(const rtm::Digest& digest,
                                                   std::uint32_t members) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::IdentityComposition;
  payload.identity.registry_composition = digest;
  payload.identity.declared_members = members;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload power_payload(std::uint64_t available, std::uint64_t applied,
                                                bool redundant, bool verified) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Power;
  payload.power.domain = must(rtm::PowerDomainReference::parse("pdu:a"));
  payload.power.available_milliwatts = available;
  payload.power.applied_milliwatts = applied;
  payload.power.energized_circuits = 2;
  payload.power.redundant_feeds_present = redundant;
  payload.power.applied_verified = verified;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload cooling_payload(std::uint64_t capacity, std::uint64_t delivered,
                                                  bool verified) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Cooling;
  payload.cooling.domain = must(rtm::CoolingDomainReference::parse("cdu:a"));
  payload.cooling.capacity_milliwatts = capacity;
  payload.cooling.delivered_milliwatts = delivered;
  payload.cooling.inlet_millidegrees_c = 22000;
  payload.cooling.max_inlet_millidegrees_c = 27000;
  payload.cooling.delivery_verified = verified;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload network_payload(const char* topology, std::uint32_t attached,
                                                  std::uint32_t reachable, bool authority) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Network;
  payload.network.domain = must(rtm::NetworkDomainReference::parse("fabric:a"));
  payload.network.topology = must(rtm::FabricTopologyReference::parse(topology));
  payload.network.attached_ports = attached;
  payload.network.reachable_ports = reachable;
  payload.network.authority_verified = authority;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload inventory_payload(std::vector<std::string> devices) {
  std::vector<rtm::InventoryEntry> entries;
  std::uint32_t unit = 1;
  for (const std::string& device : devices) {
    rtm::InventoryEntry entry;
    entry.device = must(rtm::DeviceId::parse(device));
    entry.slot = must(rtm::make_slot_coordinate(unit, 0));
    entries.push_back(std::move(entry));
    ++unit;
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Inventory;
  payload.inventory = must(rtm::create_inventory_observation(std::move(entries), 0));
  return payload;
}

[[nodiscard]] rtm::EvidencePayload health_payload(
    std::vector<std::pair<const char*, rtm::HealthStatus>> samples) {
  std::vector<rtm::DeviceHealthSample> parsed;
  for (const auto& pair : samples) {
    rtm::DeviceHealthSample sample;
    sample.device = must(rtm::DeviceId::parse(pair.first));
    sample.status = pair.second;
    parsed.push_back(std::move(sample));
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Health;
  payload.health = must(rtm::create_health_observation(std::move(parsed), 0));
  return payload;
}

[[nodiscard]] rtm::EvidencePayload compatibility_payload(const Fixture& fixture,
                                                        const char* baseline, bool with_trait) {
  std::vector<rtm::CompatibilityMembership> memberships;
  for (const rtm::CompositionMember& member : fixture.composition.members()) {
    rtm::CompatibilityMembership membership;
    membership.device = member.device;
    membership.firmware_baseline = must(rtm::FirmwareBaselineId::parse(baseline));
    std::vector<rtm::Trait> traits;
    if (with_trait) {
      traits.push_back(must(rtm::Trait::parse("power.ac.208v")));
    }
    membership.traits = must(rtm::TraitSet::create(std::move(traits)));
    memberships.push_back(std::move(membership));
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Compatibility;
  payload.compatibility = must(rtm::create_compatibility_observation(std::move(memberships)));
  return payload;
}

void add_all_evidence(Fixture& fixture, std::int64_t validity = 900000) {
  const rtm::GenerationStamp stamp = fixture.plan.binding.stamp;
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::IdentityComposition,
                                           identity_payload(fixture.composition.digest(), 2), 10,
                                           validity, 1, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Power,
                                           power_payload(20000, 12000, true, true), 20, validity, 2,
                                           stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Cooling,
                                           cooling_payload(20000, 8000, true), 30, validity, 3, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Network,
                                           network_payload("topo:a", 4, 4, true), 40, validity, 4,
                                           stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Inventory,
                                           inventory_payload({"dev:a", "dev:b"}), 50, validity, 5,
                                           stamp));
  fixture.evidence.push_back(make_evidence(
      fixture, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Passing}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, validity, 6, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Compatibility,
                                           compatibility_payload(fixture, "fw:b1", true), 70, validity,
                                           7, stamp));
  fixture.plan.evidence_high_water = must(rtm::ObservationSequence::create(7));
}

[[nodiscard]] rtm::Evaluation evaluate_fixture(const Fixture& fixture, std::int64_t at) {
  const rtm::EvaluationContext context{
      fixture.composition,    fixture.plan,          fixture.evidence, fixture.authorizations,
      fixture.activations,    fixture.commissions,   fixture.rack_generation,
      fixture.lifecycle,      clock_at(at)};
  return rtm::evaluate_rack(context);
}

[[nodiscard]] rtm::TurnupAuthorization make_authorization(const Fixture& fixture,
                                                         std::int64_t issued,
                                                         std::int64_t validity) {
  rtm::TurnupAuthorization authorization;
  authorization.attempt = must(rtm::AttemptId::parse("at:1"));
  authorization.rack = fixture.composition.rack();
  authorization.plan = fixture.plan.id;
  authorization.lifetime = fixture.plan.lifetime;
  authorization.revision = fixture.plan.revision;
  authorization.composition = fixture.plan.binding.composition;
  authorization.stamp = fixture.plan.binding.stamp;
  authorization.epoch = fixture.plan.binding.epoch;
  authorization.policy = fixture.plan.policy.generation;
  authorization.evidence_high_water = fixture.plan.evidence_high_water;
  authorization.issued_at = clock_at(issued);
  authorization.validity = rtm::Millis(validity);
  authorization.actor = must(rtm::ActorId::parse("operator"));
  authorization.state = rtm::AuthorizationState::Active;
  authorization.authorization_digest = authorization.compute_digest();
  return authorization;
}

[[nodiscard]] rtm::ActivationRecord make_activation(const Fixture& fixture,
                                                   const rtm::TurnupAuthorization& authorization,
                                                   std::int64_t observed, std::uint64_t sequence,
                                                   rtm::ActivationOutcome outcome) {
  rtm::ActivationRecord activation;
  activation.attempt = authorization.attempt;
  activation.rack = fixture.composition.rack();
  activation.plan = fixture.plan.id;
  activation.revision = fixture.plan.revision;
  activation.authorization = authorization.authorization_digest;
  activation.observed_composition = fixture.plan.binding.composition;
  activation.sequence = must(rtm::ObservationSequence::create(sequence));
  activation.observed_at = clock_at(observed);
  activation.validity = rtm::Millis(600000);
  activation.outcome = outcome;
  activation.active_members = 2;
  activation.actor = must(rtm::ActorId::parse("operator"));
  activation.activation_digest = activation.compute_digest();
  return activation;
}

[[nodiscard]] rtm::CommissionRecord make_commission(const Fixture& fixture,
                                                   const rtm::ActivationRecord& activation) {
  rtm::CommissionRecord commission;
  commission.attempt = activation.attempt;
  commission.plan = fixture.plan.id;
  commission.revision = fixture.plan.revision;
  commission.activation = activation.activation_digest;
  commission.commissioned_at = clock_at(300);
  commission.actor = must(rtm::ActorId::parse("operator"));
  commission.commission_digest = commission.compute_digest();
  return commission;
}

}  // namespace

RTM_TEST(evaluation, ready_rack_with_complete_evidence) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
  RTM_CHECK(evaluation.binding_matches);
  RTM_CHECK(evaluation.ready_to_authorize());
  RTM_CHECK_EQ(evaluation.blockers.size(), std::size_t{0});
  RTM_CHECK_EQ(evaluation.eligible_evidence, std::size_t{7});
  for (const rtm::StageKind stage : rtm::plan_stage_ladder()) {
    const rtm::StageVerdict& verdict = evaluation.stage(stage);
    if (rtm::stage_has_subsystem(stage)) {
      RTM_CHECK(verdict.state == rtm::StageState::Satisfied);
    } else {
      RTM_CHECK(verdict.state == rtm::StageState::Pending);
    }
  }
  RTM_CHECK_EQ(evaluation.expected_members.value(), std::uint64_t{2});
  RTM_CHECK_EQ(evaluation.observed_members.value(), std::uint64_t{2});
  RTM_CHECK_EQ(evaluation.healthy_members.value(), std::uint64_t{2});
}

RTM_TEST(evaluation, power_available_is_not_power_applied) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[1] = make_evidence(fixture, rtm::SubsystemKind::Power,
                                      power_payload(40000, 4000, true, true), 20, 900000, 11,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict power = evaluation.subsystem(rtm::SubsystemKind::Power);
  RTM_CHECK(power.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(power.code == rtm::ErrorCode::HeadroomInsufficient);
  RTM_CHECK(power.explanation.find("available") != std::string::npos);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::Blocked);
  RTM_CHECK(evaluation.primary_blocker() != nullptr);
  RTM_CHECK(evaluation.primary_blocker()->stage == rtm::StageKind::PowerReadiness);
}

RTM_TEST(evaluation, unverified_power_is_unknown) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[1] = make_evidence(fixture, rtm::SubsystemKind::Power,
                                      power_payload(40000, 40000, true, false), 20, 900000, 12,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).state == rtm::ReadinessState::Unknown);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).code == rtm::ErrorCode::ReadinessUnknown);
}

RTM_TEST(evaluation, cooling_capacity_is_not_cooling_delivery) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[2] = make_evidence(fixture, rtm::SubsystemKind::Cooling,
                                      cooling_payload(50000, 100, true), 30, 900000, 13,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict cooling = evaluation.subsystem(rtm::SubsystemKind::Cooling);
  RTM_CHECK(cooling.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(cooling.explanation.find("capacity") != std::string::npos);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::Blocked);
}

RTM_TEST(evaluation, network_attachment_is_not_reachability) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[3] = make_evidence(fixture, rtm::SubsystemKind::Network,
                                      network_payload("topo:a", 8, 1, true), 40, 900000, 14,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict network = evaluation.subsystem(rtm::SubsystemKind::Network);
  RTM_CHECK(network.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(network.explanation.find("reachability") != std::string::npos);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::Blocked);
}

RTM_TEST(evaluation, wrong_topology_blocks) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[3] = make_evidence(fixture, rtm::SubsystemKind::Network,
                                      network_payload("topo:other", 4, 4, true), 40, 900000, 15,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Network).code ==
             rtm::ErrorCode::SnapshotIdentityMismatch);
}

RTM_TEST(evaluation, hardware_present_is_not_healthy) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[5] = make_evidence(
      fixture, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Failing}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, 900000, 16, fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict health = evaluation.subsystem(rtm::SubsystemKind::Health);
  RTM_CHECK(health.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(health.code == rtm::ErrorCode::HealthFailed);
  RTM_CHECK_EQ(health.items.size(), std::size_t{1});
  RTM_CHECK_TEXT(health.items[0], "failing:dev:a");

  Fixture warning = make_fixture();
  add_all_evidence(warning);
  warning.evidence[5] = make_evidence(
      warning, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Warning}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, 900000, 17, warning.plan.binding.stamp);
  const rtm::Evaluation degraded = evaluate_fixture(warning, 100);
  RTM_CHECK(degraded.subsystem(rtm::SubsystemKind::Health).state == rtm::ReadinessState::Degraded);
  RTM_CHECK(degraded.verdict == rtm::TurnupVerdict::Blocked);
  RTM_CHECK(!degraded.ready_to_authorize());
  RTM_CHECK(degraded.primary_blocker() != nullptr);
  RTM_CHECK(degraded.primary_blocker()->severity == rtm::BlockerSeverity::Blocking);

  Fixture permissive = make_fixture(true);
  add_all_evidence(permissive);
  permissive.evidence[5] = make_evidence(
      permissive, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Warning}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, 900000, 18, permissive.plan.binding.stamp);
  const rtm::Evaluation allowed = evaluate_fixture(permissive, 100);
  RTM_CHECK(allowed.allow_degraded);
  RTM_CHECK(allowed.ready_to_authorize());
  RTM_CHECK(allowed.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
  RTM_CHECK_EQ(allowed.blockers.size(), std::size_t{1});
  RTM_CHECK(allowed.blockers[0].severity == rtm::BlockerSeverity::Advisory);
}

RTM_TEST(evaluation, inventory_gap_blocks) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[4] = make_evidence(fixture, rtm::SubsystemKind::Inventory,
                                      inventory_payload({"dev:a"}), 50, 900000, 19,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict inventory = evaluation.subsystem(rtm::SubsystemKind::Inventory);
  RTM_CHECK(inventory.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(inventory.code == rtm::ErrorCode::InventoryGap);
  RTM_CHECK_EQ(evaluation.observed_members.value(), std::uint64_t{1});
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::Blocked);
}

RTM_TEST(evaluation, compatibility_requirement_closure) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[6] = make_evidence(fixture, rtm::SubsystemKind::Compatibility,
                                      compatibility_payload(fixture, "fw:b1", false), 70, 900000, 20,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  const rtm::SubsystemVerdict compatibility = evaluation.subsystem(rtm::SubsystemKind::Compatibility);
  RTM_CHECK(compatibility.state == rtm::ReadinessState::Blocked);
  RTM_CHECK(compatibility.code == rtm::ErrorCode::CompatibilityUnsatisfied);
  RTM_CHECK_EQ(compatibility.items.size(), std::size_t{1});
}

RTM_TEST(evaluation, missing_and_mixed_generation_evidence_is_unknown) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence.erase(fixture.evidence.begin() + 1);
  rtm::GenerationStamp shifted = fixture.plan.binding.stamp;
  shifted.health = must(rtm::HealthGeneration::create(99));
  fixture.evidence[4] = make_evidence(
      fixture, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Passing}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, 900000, 21, shifted);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).state == rtm::ReadinessState::Unknown);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).code == rtm::ErrorCode::EvidenceMissing);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Health).state == rtm::ReadinessState::Unknown);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::NotReady);
  RTM_CHECK(evaluation.stage(rtm::StageKind::HealthValidation).gated_by_predecessor);
  RTM_CHECK(evaluation.stage(rtm::StageKind::CommissionedState).state == rtm::StageState::Pending);
}

RTM_TEST(evaluation, blocker_order_is_total_and_deterministic) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence.erase(fixture.evidence.begin() + 1);
  fixture.evidence[3] = make_evidence(fixture, rtm::SubsystemKind::Inventory,
                                      inventory_payload({"dev:a"}), 50, 900000, 22,
                                      fixture.plan.binding.stamp);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(evaluation.blockers.size() >= 2);
  RTM_CHECK(evaluation.primary_blocker() != nullptr);
  RTM_CHECK(evaluation.primary_blocker()->severity == rtm::BlockerSeverity::Blocking);
  RTM_CHECK(evaluation.primary_blocker()->stage == rtm::StageKind::HardwareInventoryClosure);
  for (std::size_t index = 1; index < evaluation.blockers.size(); ++index) {
    RTM_CHECK(!rtm::blocker_precedes(evaluation.blockers[index], evaluation.blockers[index - 1]));
  }
  const rtm::Evaluation repeated = evaluate_fixture(fixture, 100);
  RTM_CHECK(rtm::digests_equal(evaluation.verdict_digest, repeated.verdict_digest));
  RTM_CHECK(repeated.primary_blocker()->code == evaluation.primary_blocker()->code);
  RTM_CHECK(!rtm::explain_verdict(evaluation.verdict).empty());
  RTM_CHECK(!evaluation.to_report().explain().empty());
  RTM_CHECK(!rtm::describe_subsystem_verdict(
                  evaluation.subsystem(rtm::SubsystemKind::Inventory))
                  .empty());
}

RTM_TEST(evaluation, composition_change_fences_authority) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.authorizations.push_back(make_authorization(fixture, 50, 600000));
  fixture.rack_generation = must(rtm::RackGeneration::create(2));
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 100);
  RTM_CHECK(!evaluation.binding_matches);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::FencedAuthority);
  RTM_CHECK_EQ(evaluation.binding_mismatches.size(), std::size_t{1});
  RTM_CHECK_TEXT(evaluation.binding_mismatches[0].field, "rack");
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).state == rtm::ReadinessState::Unknown);
  RTM_CHECK(evaluation.primary_blocker() != nullptr);
  RTM_CHECK(evaluation.primary_blocker()->code == rtm::ErrorCode::TurnupAuthorityFenced);
}

RTM_TEST(evaluation, authorization_activation_and_commission_stages) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  const rtm::TurnupAuthorization authorization = make_authorization(fixture, 100, 600000);
  fixture.authorizations.push_back(authorization);
  const rtm::Evaluation authorized = evaluate_fixture(fixture, 200);
  RTM_CHECK(authorized.verdict == rtm::TurnupVerdict::Authorized);
  RTM_CHECK(authorized.stage(rtm::StageKind::TurnupAuthorization).state == rtm::StageState::Satisfied);
  RTM_CHECK(authorized.stage(rtm::StageKind::ActivationObservation).state == rtm::StageState::Pending);
  RTM_CHECK(authorized.authorization.present);
  RTM_CHECK(!authorized.authorization.consumed);

  const rtm::ActivationRecord activation =
      make_activation(fixture, authorization, 150, 8, rtm::ActivationOutcome::Active);
  fixture.activations.push_back(activation);
  fixture.lifecycle = rtm::RackLifecycleState::Active;
  const rtm::Evaluation active = evaluate_fixture(fixture, 200);
  RTM_CHECK(active.verdict == rtm::TurnupVerdict::ActiveObserved);
  RTM_CHECK(active.activation_observed);
  RTM_CHECK(active.stage(rtm::StageKind::ActivationObservation).state == rtm::StageState::Satisfied);
  RTM_CHECK(active.stage(rtm::StageKind::CommissionedState).state == rtm::StageState::Pending);

  fixture.commissions.push_back(make_commission(fixture, activation));
  fixture.lifecycle = rtm::RackLifecycleState::Commissioned;
  const rtm::Evaluation commissioned = evaluate_fixture(fixture, 400);
  RTM_CHECK(commissioned.verdict == rtm::TurnupVerdict::Commissioned);
  RTM_CHECK(commissioned.commissioned);
  RTM_CHECK(commissioned.stage(rtm::StageKind::CommissionedState).state ==
             rtm::StageState::Satisfied);
}

RTM_TEST(evaluation, expired_authorization_and_fenced_attempt) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.authorizations.push_back(make_authorization(fixture, 100, 50));
  const rtm::Evaluation expired = evaluate_fixture(fixture, 200);
  RTM_CHECK(expired.stage(rtm::StageKind::TurnupAuthorization).state == rtm::StageState::Blocked);
  RTM_CHECK(expired.stage(rtm::StageKind::TurnupAuthorization).code ==
             rtm::ErrorCode::TurnupNotAuthorized);
  RTM_CHECK(expired.verdict == rtm::TurnupVerdict::Blocked);

  Fixture fenced = make_fixture();
  add_all_evidence(fenced);
  rtm::TurnupAuthorization authorization = make_authorization(fenced, 100, 600000);
  authorization.state = rtm::AuthorizationState::Fenced;
  authorization.authorization_digest = authorization.compute_digest();
  fenced.authorizations.push_back(authorization);
  const rtm::Evaluation evaluation = evaluate_fixture(fenced, 200);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::FencedAuthority);
  RTM_CHECK(evaluation.stage(rtm::StageKind::TurnupAuthorization).code ==
             rtm::ErrorCode::TurnupAuthorityFenced);
}

RTM_TEST(evaluation, activation_requires_post_action_evidence) {
  Fixture early_fixture = make_fixture();
  add_all_evidence(early_fixture);
  const rtm::TurnupAuthorization early_authorization =
      make_authorization(early_fixture, 100, 600000);
  early_fixture.authorizations.push_back(early_authorization);
  early_fixture.activations.push_back(
      make_activation(early_fixture, early_authorization, 50, 8, rtm::ActivationOutcome::Active));
  const rtm::Evaluation early = evaluate_fixture(early_fixture, 200);
  RTM_CHECK(early.stage(rtm::StageKind::ActivationObservation).state == rtm::StageState::Blocked);
  RTM_CHECK(early.stage(rtm::StageKind::ActivationObservation).code ==
             rtm::ErrorCode::InvalidTimestamp);

  Fixture not_active = make_fixture();
  add_all_evidence(not_active);
  const rtm::TurnupAuthorization other = make_authorization(not_active, 100, 600000);
  not_active.authorizations.push_back(other);
  not_active.activations.push_back(
      make_activation(not_active, other, 150, 8, rtm::ActivationOutcome::NotActive));
  const rtm::Evaluation evaluation = evaluate_fixture(not_active, 200);
  RTM_CHECK(evaluation.stage(rtm::StageKind::ActivationObservation).code ==
             rtm::ErrorCode::ActivationNotConfirmed);
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::Blocked);
}

RTM_TEST(evaluation, stale_evidence_is_not_ready) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture, 1000);
  const rtm::Evaluation fresh = evaluate_fixture(fixture, 500);
  RTM_CHECK(fresh.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
  const rtm::Evaluation stale = evaluate_fixture(fixture, 5000);
  RTM_CHECK(stale.verdict == rtm::TurnupVerdict::NotReady);
  RTM_CHECK_EQ(stale.eligible_evidence, std::size_t{0});
  RTM_CHECK_EQ(stale.rejected_evidence, std::size_t{7});
  RTM_CHECK(stale.subsystem(rtm::SubsystemKind::Power).code == rtm::ErrorCode::EvidenceMissing);
}

RTM_TEST(evaluation, identity_evidence_must_match_the_bound_composition) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  fixture.evidence[0] = make_evidence(fixture, rtm::SubsystemKind::IdentityComposition,
                                      identity_payload(rtm::Digest{}, 2), 10, 900000, 23,
                                      fixture.plan.binding.stamp);
  RTM_CHECK(evaluate_fixture(fixture, 100).subsystem(rtm::SubsystemKind::IdentityComposition).code ==
             rtm::ErrorCode::CompositionDigestMismatch);

  Fixture counted = make_fixture();
  add_all_evidence(counted);
  counted.evidence[0] = make_evidence(counted, rtm::SubsystemKind::IdentityComposition,
                                      identity_payload(counted.composition.digest(), 3), 10, 900000, 24,
                                      counted.plan.binding.stamp);
  RTM_CHECK(evaluate_fixture(counted, 100).subsystem(rtm::SubsystemKind::IdentityComposition).code ==
             rtm::ErrorCode::SnapshotIdentityMismatch);
}

RTM_TEST(evaluation, requirements_and_policy_validation) {
  rtm::CompatibilityRequirement archived = must(rtm::make_compatibility_requirement(
      rtm::RequirementKind::BaselineOnDevice, rtm::Trait{},
      must(rtm::DeviceId::parse("dev:a")), must(rtm::FirmwareBaselineId::parse("fw:b1")), false,
      rtm::Note{}));
  const std::string text = archived.to_text();
  RTM_CHECK(text.find("kind=baseline_on_device") == 0);
  RTM_CHECK(text.find("mandatory=false") != std::string::npos);
  const rtm::CompatibilityRequirement parsed = must(rtm::parse_compatibility_requirement(text));
  RTM_CHECK_TEXT(parsed.to_text(), text);
  RTM_CHECK_CODE(rtm::parse_compatibility_requirement("trait=power.ac.208v"),
                 rtm::ErrorCode::InvalidArgument);
  RTM_CHECK_CODE(rtm::parse_compatibility_requirement("kind=nope"),
                 rtm::ErrorCode::InvalidEnumValue);
  RTM_CHECK_CODE(rtm::make_compatibility_requirement(
                     rtm::RequirementKind::TraitOnDevice, must(rtm::Trait::parse("a")),
                     rtm::DeviceId{}, rtm::FirmwareBaselineId{}, true, rtm::Note{}),
                 rtm::ErrorCode::MalformedCompatibilityProfile);

  rtm::PlanRequirements requirements;
  requirements.compatibility.push_back(parsed);
  requirements.compatibility.push_back(parsed);
  RTM_CHECK_CODE(rtm::make_plan_requirements(requirements),
                 rtm::ErrorCode::DuplicateCompatibilityRequirement);
  rtm::TurnupPolicy policy;
  policy.authorization_validity = rtm::Millis(0);
  RTM_CHECK_CODE(rtm::make_turnup_policy(policy), rtm::ErrorCode::InvalidDuration);
}

RTM_TEST(evaluation, generation_stamp_comparison) {
  const rtm::GenerationStamp left;
  rtm::GenerationStamp right;
  RTM_CHECK(left == right);
  right.cooling = must(rtm::CoolingGeneration::create(5));
  RTM_CHECK(left != right);
  const std::vector<rtm::GenerationMismatch> mismatches = left.compare(right);
  RTM_CHECK_EQ(mismatches.size(), std::size_t{1});
  RTM_CHECK_TEXT(mismatches[0].field, "cooling");
  RTM_CHECK_EQ(mismatches[0].expected, std::uint64_t{0});
  RTM_CHECK_EQ(mismatches[0].actual, std::uint64_t{5});
  RTM_CHECK(!rtm::describe_generation_mismatches(mismatches).empty());
  RTM_CHECK_TEXT(rtm::GenerationStamp{}.describe(),
                 "rack=0 composition=0 topology=0 dependency=0 power=0 cooling=0 network=0 "
                 "inventory=0 health=0 capacity=0 maintenance=0 policy=0 firmware=0");
}

RTM_TEST(evaluation, vocabulary_helpers) {
  RTM_CHECK_TEXT(rtm::subsystem_kind_name(rtm::SubsystemKind::IdentityComposition),
                 "identity_composition");
  RTM_CHECK(rtm::subsystem_kind_from_name("cooling").has_value());
  RTM_CHECK_CODE(rtm::subsystem_kind_from_name("nope"), rtm::ErrorCode::InvalidEnumValue);
  RTM_CHECK_TEXT(rtm::stage_kind_name(rtm::StageKind::CommissionedState), "commissioned_state");
  RTM_CHECK(rtm::stage_kind_from_name("power_readiness").has_value());
  RTM_CHECK(rtm::stage_for_subsystem(rtm::SubsystemKind::Network) ==
             rtm::StageKind::NetworkReadiness);
  RTM_CHECK(rtm::subsystem_for_stage(rtm::StageKind::HealthValidation) ==
             rtm::SubsystemKind::Health);
  RTM_CHECK(!rtm::stage_has_subsystem(rtm::StageKind::TurnupAuthorization));
  RTM_CHECK(rtm::is_known_subsystem_kind(7));
  RTM_CHECK(!rtm::is_known_subsystem_kind(8));
  RTM_CHECK(rtm::is_known_stage_kind(10));
  RTM_CHECK(!rtm::is_known_stage_kind(11));
  RTM_CHECK_EQ(rtm::stage_index(rtm::StageKind::PowerReadiness), std::size_t{1});
  RTM_CHECK(rtm::lifecycle_allows(rtm::RackLifecycleState::Registered,
                                  rtm::RackLifecycleState::Planned));
  RTM_CHECK(!rtm::lifecycle_allows(rtm::RackLifecycleState::Registered,
                                   rtm::RackLifecycleState::Commissioned));
  RTM_CHECK(rtm::lifecycle_allows(rtm::RackLifecycleState::Drained,
                                  rtm::RackLifecycleState::Decommissioned));
  RTM_CHECK(!rtm::lifecycle_allows(rtm::RackLifecycleState::Commissioned,
                                   rtm::RackLifecycleState::Decommissioned));
  RTM_CHECK(rtm::readiness_permits_turnup(rtm::ReadinessState::Ready, false));
  RTM_CHECK(!rtm::readiness_permits_turnup(rtm::ReadinessState::Degraded, false));
  RTM_CHECK(rtm::readiness_permits_turnup(rtm::ReadinessState::Degraded, true));
  RTM_CHECK(!rtm::readiness_permits_turnup(rtm::ReadinessState::Unknown, true));
  RTM_CHECK(rtm::combine_readiness(rtm::ReadinessState::Degraded,
                                   rtm::ReadinessState::Unknown) == rtm::ReadinessState::Unknown);
  RTM_CHECK(rtm::combine_readiness(rtm::ReadinessState::Blocked,
                                   rtm::ReadinessState::Unknown) == rtm::ReadinessState::Blocked);
  RTM_CHECK(rtm::combine_readiness(rtm::ReadinessState::Degraded,
                                   rtm::ReadinessState::Ready) == rtm::ReadinessState::Degraded);
  RTM_CHECK_TEXT(rtm::operation_kind_name(rtm::OperationKind::ImportEvidence), "import_evidence");
  RTM_CHECK(rtm::operation_kind_from_name("take_control").has_value());
  RTM_CHECK_TEXT(rtm::version_string(), "1.0.0");
  RTM_CHECK(!rtm::build_compiler().empty());
  RTM_CHECK(!rtm::build_configuration().empty());
}
