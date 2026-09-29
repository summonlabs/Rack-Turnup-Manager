// Rack Turnup Manager - service lifecycle, authority and recovery tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/cli.hpp"
#include "rack_turnup/service.hpp"
#include "rack_turnup/state.hpp"

namespace {

namespace rtm = rackturnup;
using rtmtest::must;

constexpr std::int64_t kBaseTime = 1770000000000ll;

[[nodiscard]] rtm::WallClock clock_at(std::int64_t offset_ms) {
  return rtm::WallClock(kBaseTime + offset_ms);
}

[[nodiscard]] rtm::MutationContext make_context(std::int64_t at, const char* request = "") {
  rtm::MutationContext context;
  context.actor = must(rtm::ActorId::parse("operator"));
  context.source = must(rtm::SourceReference::parse("dcp:cli"));
  context.authority_time = clock_at(at);
  if (request[0] != 0) {
    context.request = must(rtm::RequestId::parse(request));
  }
  return context;
}

[[nodiscard]] rtm::StoreOptions store_options(const std::string& path, bool read_only = false) {
  rtm::StoreOptions options;
  options.path = path;
  options.read_only = read_only;
  return options;
}

[[nodiscard]] rtm::CompositionMember member(const char* device, std::uint32_t unit) {
  rtm::CompositionMember entry;
  entry.device = must(rtm::DeviceId::parse(device));
  entry.slot = must(rtm::make_slot_coordinate(unit, 0));
  entry.firmware_baseline = must(rtm::FirmwareBaselineId::parse("fw:b1"));
  entry.traits = must(rtm::TraitSet::create({must(rtm::Trait::parse("power.ac.208v"))}));
  return entry;
}

[[nodiscard]] rtm::RegisterRackRequest register_request(std::int64_t at, const char* request = "") {
  rtm::RegisterRackRequest registration;
  registration.context = make_context(at, request);
  registration.rack = must(rtm::RackId::parse("rack:r1"));
  registration.site = must(rtm::SiteId::parse("site:s1"));
  registration.label = must(rtm::parse_label_or_none("Rack one"));
  registration.members.push_back(member("dev:a", 1));
  registration.members.push_back(member("dev:b", 2));
  return registration;
}

[[nodiscard]] rtm::CreatePlanRequest plan_request(std::int64_t at, const char* request = "") {
  rtm::CreatePlanRequest creation;
  creation.context = make_context(at, request);
  creation.rack = must(rtm::RackId::parse("rack:r1"));
  creation.expected_rack_generation = rtm::RackGeneration::initial();
  creation.stamp.rack = rtm::RackGeneration::initial();
  creation.stamp.composition = rtm::CompositionGeneration::initial();
  creation.stamp.topology = must(rtm::TopologyGeneration::create(2));
  creation.stamp.power = must(rtm::PowerGeneration::create(3));
  creation.stamp.cooling = must(rtm::CoolingGeneration::create(4));
  creation.stamp.network = must(rtm::NetworkGeneration::create(5));
  creation.stamp.inventory = must(rtm::InventoryGeneration::create(6));
  creation.stamp.health = must(rtm::HealthGeneration::create(7));
  creation.policy.generation = rtm::PolicyGeneration::initial();
  creation.stamp.policy = rtm::PolicyGeneration::initial();
  creation.requirements.required_power_milliwatts = 10000;
  creation.requirements.required_cooling_milliwatts = 5000;
  creation.requirements.required_network_ports = 2;
  creation.requirements.require_redundant_feeds = true;
  creation.requirements.required_topology = must(rtm::FabricTopologyReference::parse("topo:a"));
  creation.requirements.compatibility.push_back(must(rtm::make_compatibility_requirement(
      rtm::RequirementKind::TraitOnEveryMember, must(rtm::Trait::parse("power.ac.208v")),
      rtm::DeviceId{}, rtm::FirmwareBaselineId{}, true, rtm::Note{})));
  return creation;
}

[[nodiscard]] rtm::EvidencePayload identity_payload(const rtm::Digest& digest) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::IdentityComposition;
  payload.identity.registry_composition = digest;
  payload.identity.declared_members = 2;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload power_payload(std::uint64_t available, std::uint64_t applied) {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Power;
  payload.power.domain = must(rtm::PowerDomainReference::parse("pdu:a"));
  payload.power.available_milliwatts = available;
  payload.power.applied_milliwatts = applied;
  payload.power.energized_circuits = 2;
  payload.power.redundant_feeds_present = true;
  payload.power.applied_verified = true;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload cooling_payload() {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Cooling;
  payload.cooling.domain = must(rtm::CoolingDomainReference::parse("cdu:a"));
  payload.cooling.capacity_milliwatts = 20000;
  payload.cooling.delivered_milliwatts = 8000;
  payload.cooling.delivery_verified = true;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload network_payload() {
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Network;
  payload.network.domain = must(rtm::NetworkDomainReference::parse("fabric:a"));
  payload.network.topology = must(rtm::FabricTopologyReference::parse("topo:a"));
  payload.network.attached_ports = 4;
  payload.network.reachable_ports = 4;
  payload.network.authority_verified = true;
  return payload;
}

[[nodiscard]] rtm::EvidencePayload inventory_payload() {
  std::vector<rtm::InventoryEntry> entries;
  std::uint32_t unit = 1;
  for (const char* device : {"dev:a", "dev:b"}) {
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

[[nodiscard]] rtm::EvidencePayload health_payload() {
  std::vector<rtm::DeviceHealthSample> samples;
  for (const char* device : {"dev:a", "dev:b"}) {
    rtm::DeviceHealthSample sample;
    sample.device = must(rtm::DeviceId::parse(device));
    sample.status = rtm::HealthStatus::Passing;
    samples.push_back(std::move(sample));
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Health;
  payload.health = must(rtm::create_health_observation(std::move(samples), 0));
  return payload;
}

[[nodiscard]] rtm::EvidencePayload compatibility_payload() {
  std::vector<rtm::CompatibilityMembership> memberships;
  for (const char* device : {"dev:a", "dev:b"}) {
    rtm::CompatibilityMembership membership;
    membership.device = must(rtm::DeviceId::parse(device));
    membership.firmware_baseline = must(rtm::FirmwareBaselineId::parse("fw:b1"));
    membership.traits = must(rtm::TraitSet::create({must(rtm::Trait::parse("power.ac.208v"))}));
    memberships.push_back(std::move(membership));
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Compatibility;
  payload.compatibility = must(rtm::create_compatibility_observation(std::move(memberships)));
  return payload;
}

[[nodiscard]] rtm::ImportEvidenceRequest evidence_request(std::int64_t at, rtm::PlanRevision revision,
                                                         rtm::SubsystemKind subsystem,
                                                         rtm::EvidencePayload payload,
                                                         std::int64_t observed,
                                                         const char* request = "") {
  rtm::ImportEvidenceRequest import;
  import.context = make_context(at, request);
  import.rack = must(rtm::RackId::parse("rack:r1"));
  import.expected_revision = revision;
  import.subsystem = subsystem;
  import.payload = std::move(payload);
  import.observed_at = clock_at(observed);
  import.validity_from_policy = true;
  return import;
}

// Registers a rack with a plan and imports the complete ready evidence set.
[[nodiscard]] rtm::TurnupService prepare_service(const std::string& path) {
  rtm::TurnupService service = must(rtm::TurnupService::open(store_options(path)));
  must(service.register_rack(register_request(0)));
  const rtm::RackSummary planned = must(service.create_plan(plan_request(1)));
  const rtm::RackId rack = planned.id;
  const rtm::Digest composition = planned.composition;
  rtm::PlanRevision revision = planned.has_plan ? planned.revision : rtm::PlanRevision::initial();
  must(service.import_evidence(evidence_request(10, revision, rtm::SubsystemKind::IdentityComposition,
                                                identity_payload(composition), 5)));
  revision = service.summary(rack, clock_at(10)).value().revision;
  must(service.import_evidence(evidence_request(11, revision, rtm::SubsystemKind::Power,
                                                power_payload(20000, 12000), 6)));
  revision = service.summary(rack, clock_at(11)).value().revision;
  must(service.import_evidence(evidence_request(12, revision, rtm::SubsystemKind::Cooling,
                                                cooling_payload(), 7)));
  revision = service.summary(rack, clock_at(12)).value().revision;
  must(service.import_evidence(evidence_request(13, revision, rtm::SubsystemKind::Network,
                                                network_payload(), 8)));
  revision = service.summary(rack, clock_at(13)).value().revision;
  must(service.import_evidence(evidence_request(14, revision, rtm::SubsystemKind::Inventory,
                                                inventory_payload(), 9)));
  revision = service.summary(rack, clock_at(14)).value().revision;
  must(service.import_evidence(evidence_request(15, revision, rtm::SubsystemKind::Health,
                                                health_payload(), 10)));
  revision = service.summary(rack, clock_at(15)).value().revision;
  must(service.import_evidence(evidence_request(16, revision, rtm::SubsystemKind::Compatibility,
                                                compatibility_payload(), 11)));
  return service;
}

[[nodiscard]] rtm::PlanRevision revision_of(rtm::TurnupService& service, const char* rack) {
  return must(service.summary(must(rtm::RackId::parse(rack)), rtm::WallClock::now())).revision;
}

}  // namespace

RTM_TEST(service, full_cycle_to_commissioned) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  const rtm::Evaluation ready = must(service.evaluate(rack, clock_at(200)));
  RTM_CHECK(ready.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
  RTM_CHECK_EQ(service.state().racks.size(), std::size_t{1});
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Planned);

  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200, "req-authorize");
  authorization.rack = rack;
  authorization.expected_revision = ready.revision;
  authorization.validity_from_policy = true;
  const rtm::AuthorizationReceipt issued = must(service.authorize_turnup(authorization));
  RTM_CHECK(!issued.replayed);
  RTM_CHECK(rtm::digests_equal(issued.verdict, ready.verdict_digest));
  RTM_CHECK_TEXT(issued.attempt.text(), "at:1");
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Authorized);

  rtm::RecordActivationRequest activation;
  activation.context = make_context(300, "req-activate");
  activation.rack = rack;
  activation.attempt = issued.attempt;
  activation.expected_revision = revision_of(service, "rack:r1");
  activation.outcome = rtm::ActivationOutcome::Active;
  activation.observed_at = clock_at(250);
  activation.validity_from_policy = true;
  activation.active_members = 2;
  const rtm::ActivationReceipt observed = must(service.record_activation(activation));
  RTM_CHECK(observed.outcome == rtm::ActivationOutcome::Active);
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Active);
  RTM_CHECK(service.state().racks[0].authorizations[0].state ==
             rtm::AuthorizationState::Consumed);

  rtm::CommissionRequest commission;
  commission.context = make_context(400, "req-commission");
  commission.rack = rack;
  commission.attempt = issued.attempt;
  commission.expected_revision = revision_of(service, "rack:r1");
  const rtm::CommissionReceipt commissioned = must(service.commission(commission));
  RTM_CHECK(!commissioned.replayed);
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Commissioned);
  const rtm::Evaluation final = must(service.evaluate(rack, clock_at(500)));
  RTM_CHECK(final.verdict == rtm::TurnupVerdict::Commissioned);
  const rtm::RackSummary summary = must(service.summary(rack, clock_at(500)));
  RTM_CHECK(summary.has_plan);
  RTM_CHECK(summary.lifecycle == rtm::RackLifecycleState::Commissioned);
  RTM_CHECK(summary.blocker_count == 0);
}

RTM_TEST(service, stale_revision_and_unknown_rack) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  const rtm::PlanRevision revision = revision_of(service, "rack:r1");
  RTM_CHECK_CODE(
      service.import_evidence(evidence_request(20, rtm::PlanRevision::initial(),
                                               rtm::SubsystemKind::Power,
                                               power_payload(20000, 12000), 12)),
      rtm::ErrorCode::StalePlanRevision);
  const rtm::RackId missing = must(rtm::RackId::parse("rack:none"));
  RTM_CHECK_CODE(service.evaluate(missing, clock_at(20)), rtm::ErrorCode::UnknownRackId);
  rtm::CreatePlanRequest duplicate = plan_request(20);
  duplicate.stamp.composition = must(rtm::CompositionGeneration::create(9));
  RTM_CHECK_CODE(service.create_plan(duplicate), rtm::ErrorCode::StaleCompositionGeneration);
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(20);
  authorization.rack = rack;
  authorization.expected_revision = revision;
  authorization.validity_from_policy = true;
  const rtm::AuthorizationReceipt issued = must(service.authorize_turnup(authorization));
  rtm::RollbackRequest rollback;
  rollback.context = make_context(30);
  rollback.rack = rack;
  rollback.expected_revision = rtm::PlanRevision::initial();
  RTM_CHECK_CODE(service.rollback(rollback), rtm::ErrorCode::StalePlanRevision);
  static_cast<void>(issued);
}

RTM_TEST(service, idempotent_replay_and_conflict) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::PlanRevision revision = revision_of(service, "rack:r1");
  rtm::ImportEvidenceRequest first = evidence_request(
      30, revision, rtm::SubsystemKind::Power, power_payload(30000, 20000), 20, "req-evidence");
  // The policy validity is used so that a retry that leaves out the validity is
  // still the same intent.
  const rtm::EvidenceReceipt receipt = must(service.import_evidence(first));
  const rtm::PlanRevision after_first = revision_of(service, "rack:r1");
  const rtm::EvidenceReceipt replay = must(service.import_evidence(first));
  RTM_CHECK(replay.replayed);
  RTM_CHECK(replay.id == receipt.id);
  RTM_CHECK_EQ(revision_of(service, "rack:r1").value(), after_first.value());
  RTM_CHECK_EQ(service.state().racks[0].evidence.size(), std::size_t{8});

  rtm::ImportEvidenceRequest conflicting = first;
  conflicting.payload = power_payload(1000, 1000);
  RTM_CHECK_CODE(service.import_evidence(conflicting), rtm::ErrorCode::RequestIdConflict);
  static_cast<void>(rack);
}

RTM_TEST(service, evidence_after_authorization_fences_the_grant) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200);
  authorization.rack = rack;
  authorization.expected_revision = revision_of(service, "rack:r1");
  authorization.validity_from_policy = true;
  const rtm::AuthorizationReceipt issued = must(service.authorize_turnup(authorization));
  const rtm::PlanRevision revision = revision_of(service, "rack:r1");
  must(service.import_evidence(evidence_request(300, revision, rtm::SubsystemKind::Power,
                                                power_payload(25000, 15000), 250)));
  const rtm::RackState& state = service.state().racks[0];
  RTM_CHECK(state.authorizations[0].state == rtm::AuthorizationState::Fenced);
  RTM_CHECK_EQ(state.fences.size(), std::size_t{1});
  RTM_CHECK(state.fences[0].attempt == issued.attempt);
  RTM_CHECK(state.fences[0].reason == rtm::ErrorCode::AuthoritySuperseded);
  RTM_CHECK(state.rack.lifecycle == rtm::RackLifecycleState::Planned);
  const rtm::Evaluation evaluation = must(service.evaluate(rack, clock_at(400)));
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::FencedAuthority);
  rtm::RecordActivationRequest activation;
  activation.context = make_context(400);
  activation.rack = rack;
  activation.attempt = issued.attempt;
  activation.expected_revision = revision_of(service, "rack:r1");
  activation.outcome = rtm::ActivationOutcome::Active;
  activation.observed_at = clock_at(350);
  activation.validity_from_policy = true;
  RTM_CHECK_CODE(service.record_activation(activation), rtm::ErrorCode::TurnupAuthorityFenced);
}

RTM_TEST(service, composition_change_supersedes_plan_and_fences) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200);
  authorization.rack = rack;
  authorization.expected_revision = revision_of(service, "rack:r1");
  authorization.validity_from_policy = true;
  must(service.authorize_turnup(authorization));

  rtm::UpdateCompositionRequest update;
  update.context = make_context(300);
  update.rack = rack;
  update.expected_rack_generation = rtm::RackGeneration::initial();
  update.composition_generation = must(rtm::CompositionGeneration::create(2));
  update.members.push_back(member("dev:a", 1));
  update.members.push_back(member("dev:c", 2));
  RTM_CHECK_CODE(service.update_composition(update), rtm::ErrorCode::TurnupAlreadyAuthorized);
  update.replace_commissioned = true;
  must(service.update_composition(update));
  const rtm::RackState& state = service.state().racks[0];
  RTM_CHECK(state.rack.generation.value() == 2);
  RTM_CHECK(state.rack.composition_generation().value() == 2);
  RTM_CHECK(state.plans[0].state == rtm::PlanState::Superseded);
  RTM_CHECK(state.authorizations[0].state == rtm::AuthorizationState::Fenced);
  RTM_CHECK(state.rack.lifecycle == rtm::RackLifecycleState::Planned);
  // The superseded plan still describes the rack, and the evaluation reports the
  // fenced authority instead of pretending the rack has no history.
  const rtm::Evaluation fenced = must(service.evaluate(rack, clock_at(400)));
  RTM_CHECK(fenced.verdict == rtm::TurnupVerdict::FencedAuthority);
  RTM_CHECK(!fenced.binding_matches);

  rtm::CreatePlanRequest replacement = plan_request(500);
  replacement.expected_rack_generation = must(rtm::RackGeneration::create(2));
  replacement.stamp.rack = must(rtm::RackGeneration::create(2));
  replacement.stamp.composition = must(rtm::CompositionGeneration::create(2));
  const rtm::RackSummary planned = must(service.create_plan(replacement));
  RTM_CHECK(planned.has_plan);
  RTM_CHECK_EQ(planned.lifetime.value(), std::uint64_t{2});
  RTM_CHECK(planned.revision.value() == 1);
  const rtm::Evaluation evaluation = must(service.evaluate(rack, clock_at(600)));
  RTM_CHECK(evaluation.verdict == rtm::TurnupVerdict::NotReady);
  RTM_CHECK(evaluation.binding_matches);
}

RTM_TEST(service, rollback_cancels_the_grant) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200);
  authorization.rack = rack;
  authorization.expected_revision = revision_of(service, "rack:r1");
  authorization.validity_from_policy = true;
  must(service.authorize_turnup(authorization));
  rtm::RollbackRequest rollback;
  rollback.context = make_context(300, "req-rollback");
  rollback.rack = rack;
  rollback.expected_revision = revision_of(service, "rack:r1");
  rollback.reason = must(rtm::parse_note_or_none("planned maintenance"));
  const rtm::RollbackReceipt receipt = must(service.rollback(rollback));
  RTM_CHECK_EQ(receipt.authorizations_fenced, std::size_t{1});
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Planned);
  RTM_CHECK(service.state().racks[0].authorizations[0].state ==
             rtm::AuthorizationState::Cancelled);
  const rtm::RollbackReceipt replayed = must(service.rollback(rollback));
  RTM_CHECK(replayed.replayed);
}

RTM_TEST(service, drain_then_decommission) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200);
  authorization.rack = rack;
  authorization.expected_revision = revision_of(service, "rack:r1");
  authorization.validity_from_policy = true;
  const rtm::AuthorizationReceipt issued = must(service.authorize_turnup(authorization));
  rtm::RecordActivationRequest activation;
  activation.context = make_context(300);
  activation.rack = rack;
  activation.attempt = issued.attempt;
  activation.expected_revision = revision_of(service, "rack:r1");
  activation.outcome = rtm::ActivationOutcome::Active;
  activation.observed_at = clock_at(250);
  activation.validity_from_policy = true;
  must(service.record_activation(activation));
  rtm::CommissionRequest commission;
  commission.context = make_context(400);
  commission.rack = rack;
  commission.attempt = issued.attempt;
  commission.expected_revision = revision_of(service, "rack:r1");
  must(service.commission(commission));

  rtm::LifecycleRequest drain;
  drain.context = make_context(500);
  drain.rack = rack;
  drain.expected_revision = revision_of(service, "rack:r1");
  RTM_CHECK_CODE(service.decommission(drain), rtm::ErrorCode::LifecycleTransitionNotAllowed);
  const rtm::LifecycleReceipt draining = must(service.begin_drain(drain));
  RTM_CHECK(draining.to == rtm::RackLifecycleState::Draining);
  drain.context = make_context(600);
  drain.expected_revision = revision_of(service, "rack:r1");
  const rtm::LifecycleReceipt drained = must(service.complete_drain(drain));
  RTM_CHECK(drained.to == rtm::RackLifecycleState::Drained);
  // Drained is not decommissioned: the rack is offline but still a managed
  // asset until decommissioning is recorded.
  RTM_CHECK(service.state().racks[0].rack.lifecycle == rtm::RackLifecycleState::Drained);
  drain.context = make_context(700);
  drain.expected_revision = revision_of(service, "rack:r1");
  const rtm::LifecycleReceipt decommissioned = must(service.decommission(drain));
  RTM_CHECK(decommissioned.to == rtm::RackLifecycleState::Decommissioned);
  RTM_CHECK(service.state().racks[0].commissions.size() == 1);
}

RTM_TEST(service, take_control_fences_everything) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  rtm::AuthorizeRequest authorization;
  authorization.context = make_context(200);
  authorization.rack = rack;
  authorization.expected_revision = revision_of(service, "rack:r1");
  authorization.validity_from_policy = true;
  must(service.authorize_turnup(authorization));
  rtm::TakeControlRequest takeover;
  takeover.context = make_context(300);
  const rtm::TakeoverReport report = must(service.take_control(takeover));
  RTM_CHECK_EQ(report.previous.value(), std::uint64_t{1});
  RTM_CHECK_EQ(report.current.value(), std::uint64_t{2});
  RTM_CHECK_EQ(report.authorizations_fenced, std::size_t{1});
  const rtm::RackState& state = service.state().racks[0];
  RTM_CHECK(state.authorizations[0].state == rtm::AuthorizationState::Fenced);
  RTM_CHECK(state.plans[0].state == rtm::PlanState::Superseded);
  RTM_CHECK(service.state().control_epoch.value() == 2);
  // The superseded plan cannot authorize anything, and a new plan must be
  // created under the new control epoch.
  rtm::AuthorizeRequest stale_authorization;
  stale_authorization.context = make_context(400);
  stale_authorization.rack = rack;
  stale_authorization.expected_revision = rtm::PlanRevision::initial();
  stale_authorization.validity_from_policy = true;
  RTM_CHECK_CODE(service.authorize_turnup(stale_authorization), rtm::ErrorCode::PlanNotActive);

  rtm::CreatePlanRequest fresh = plan_request(500);
  const rtm::RackSummary planned = must(service.create_plan(fresh));
  RTM_CHECK(planned.has_plan);
  RTM_CHECK_EQ(planned.lifetime.value(), std::uint64_t{2});
  const rtm::Evaluation evaluation = must(service.evaluate(rack, clock_at(600)));
  RTM_CHECK(evaluation.epoch.value() == 2);
}

RTM_TEST(service, restart_recovers_exactly_one_generation) {
  rtmtest::TempDir directory("service");
  const std::string path = directory.file("state.rtm");
  std::uint64_t sequence = 0;
  rtm::Digest digest;
  {
    rtm::TurnupService service = prepare_service(path);
    const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
    sequence = service.state().store_sequence.value();
    digest = rtm::compute_state_digest(service.state());
    const rtm::Evaluation before = must(service.evaluate(rack, clock_at(100)));
    RTM_CHECK(before.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
  }
  {
    rtm::TurnupService service = must(rtm::TurnupService::open(store_options(path)));
    const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
    RTM_CHECK(service.state().store_sequence.value() == sequence);
    RTM_CHECK(service.state().incarnation.value() >= 2);
    RTM_CHECK(service.open_report().racks == 1);
    const rtm::Evaluation after = must(service.evaluate(rack, clock_at(100)));
    RTM_CHECK(after.verdict == rtm::TurnupVerdict::ReadyToAuthorize);
    RTM_CHECK(rtm::digests_equal(rtm::compute_state_digest(service.state()), digest) == false);
    const rtm::RecoveryReport report = must(service.recover());
    RTM_CHECK_EQ(report.racks, std::size_t{1});
    RTM_CHECK_EQ(report.fences_added, std::size_t{0});
  }
}

RTM_TEST(service, read_only_store_refuses_mutation) {
  rtmtest::TempDir directory("service");
  const std::string path = directory.file("state.rtm");
  {
    rtm::TurnupService service = prepare_service(path);
    static_cast<void>(service);
  }
  rtm::TurnupService reader = must(rtm::TurnupService::open(store_options(path, true)));
  RTM_CHECK(!reader.is_writable());
  RTM_CHECK(reader.state().racks.size() == 1);
  RTM_CHECK_CODE(reader.register_rack(register_request(0)), rtm::ErrorCode::ReadOnlyStore);
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  RTM_CHECK(must(reader.evaluate(rack, clock_at(100))).verdict ==
             rtm::TurnupVerdict::ReadyToAuthorize);
  RTM_CHECK_CODE(reader.recover(), rtm::ErrorCode::ReadOnlyStore);
}

RTM_TEST(service, rejection_journal_records_refusals) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  RTM_CHECK(service.state().rejections.empty());
  RTM_CHECK_CODE(service.register_rack(register_request(100)), rtm::ErrorCode::DuplicateRackId);
  const std::vector<rtm::RejectionRecord> journal = must(service.rejections());
  RTM_CHECK_EQ(journal.size(), std::size_t{1});
  RTM_CHECK(journal[0].code == rtm::ErrorCode::DuplicateRackId);
  RTM_CHECK(journal[0].operation == rtm::OperationKind::RegisterRack);
  const std::vector<rtm::ProvenanceRecord> provenance =
      must(service.provenance(must(rtm::RackId::parse("rack:r1"))));
  RTM_CHECK(provenance.size() >= 8);
}

RTM_TEST(service, seeded_state_machine_keeps_invariants) {
  rtmtest::Rng rng(rtmtest::global_seed() ^ 0x5A5A5A5Au);
  rtmtest::TempDir directory("machine");
  rtm::TurnupService service = prepare_service(directory.file("state.rtm"));
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  for (int step = 0; step < 40; ++step) {
    const std::uint64_t choice = rng.uniform(6);
    const std::int64_t at = 1000 + step * 10;
    if (choice == 0) {
      rtm::AuthorizeRequest request;
      request.context = make_context(at);
      request.rack = rack;
      request.expected_revision = revision_of(service, "rack:r1");
      request.validity_from_policy = true;
      static_cast<void>(service.authorize_turnup(request));
    } else if (choice == 1) {
      rtm::ImportEvidenceRequest request = evidence_request(
          at, revision_of(service, "rack:r1"), rtm::SubsystemKind::Power,
          power_payload(20000 + rng.uniform(5000), 12000 + rng.uniform(5000)), at - 5);
      static_cast<void>(service.import_evidence(request));
    } else if (choice == 2) {
      rtm::RollbackRequest request;
      request.context = make_context(at);
      request.rack = rack;
      request.expected_revision = revision_of(service, "rack:r1");
      request.acknowledge_active = true;
      static_cast<void>(service.rollback(request));
    } else if (choice == 3) {
      rtm::LifecycleRequest request;
      request.context = make_context(at);
      request.rack = rack;
      request.expected_revision = revision_of(service, "rack:r1");
      static_cast<void>(service.begin_drain(request));
    } else if (choice == 4) {
      RTM_CHECK(must(service.evaluate(rack, clock_at(at))).stages.size() ==
                 rtm::kPlanStageCount);
    } else {
      const rtm::Status validated = rtm::validate_state(service.state());
      RTM_CHECK(validated.has_value());
    }

    // Invariants that must hold after every accepted or refused operation.
    const rtm::Status validated = rtm::validate_state(service.state());
    RTM_CHECK(validated.has_value());
    const rtm::RackState& state = service.state().racks[0];
    std::size_t active_authorizations = 0;
    for (const rtm::TurnupAuthorization& authorization : state.authorizations) {
      if (authorization.state == rtm::AuthorizationState::Active) {
        ++active_authorizations;
        RTM_CHECK(authorization.revision == state.plans.back().revision);
        RTM_CHECK(authorization.epoch == service.state().control_epoch);
      }
    }
    RTM_CHECK(active_authorizations <= 1);
    for (std::size_t index = 1; index < state.evidence.size(); ++index) {
      RTM_CHECK(state.evidence[index - 1].sequence < state.evidence[index].sequence);
    }
    const std::vector<std::uint8_t> encoded = must(rtm::encode_state(service.state()));
    const rtm::ServiceState decoded = must(rtm::decode_state(encoded));
    RTM_CHECK(rtm::digests_equal(rtm::compute_state_digest(decoded),
                                 rtm::compute_state_digest(service.state())));
    const std::vector<std::uint8_t> reencoded = must(rtm::encode_state(decoded));
    RTM_CHECK(encoded == reencoded);
  }
}

RTM_TEST(service, plan_policy_generation_is_derived_from_the_request) {
  rtmtest::TempDir directory("service");
  rtm::TurnupService service =
      must(rtm::TurnupService::open(store_options(directory.file("state.rtm"))));
  must(service.register_rack(register_request(0)));
  rtm::CreatePlanRequest request = plan_request(1);
  request.stamp.policy = rtm::PolicyGeneration{};  // left out on purpose
  const rtm::RackSummary planned = must(service.create_plan(request));
  RTM_CHECK(planned.has_plan);
  const rtm::RackState& state = service.state().racks[0];
  RTM_CHECK(state.plans[0].binding.stamp.policy.value() == request.policy.generation.value());
  RTM_CHECK(state.plans[0].policy.generation.value() == request.policy.generation.value());
  // A stamp that contradicts the policy it carries is still refused.
  rtm::CreatePlanRequest contradictory = plan_request(2);
  contradictory.stamp.policy = must(rtm::PolicyGeneration::create(9));
  RTM_CHECK_CODE(service.create_plan(contradictory), rtm::ErrorCode::StalePolicyGeneration);
}

RTM_TEST(cli, in_process_full_cycle) {
  rtmtest::TempDir directory("cli");
  const std::string store = directory.file("state.rtm");
  const std::string digest = must(rtm::RackComposition::create(
                                     must(rtm::RackId::parse("rack:r1")),
                                     must(rtm::SiteId::parse("site:s1")),
                                     rtm::CompositionGeneration::initial(),
                                     {member("dev:a", 1), member("dev:b", 2)}))
                                     .digest()
                                     .to_hex();
  std::ostringstream output;
  std::ostringstream errors;
  std::string now_text = clock_at(0).to_text();
  const auto run = [&](const std::vector<std::string>& arguments) {
    std::vector<std::string> full = {"--store", store, "--now", now_text};
    full.insert(full.end(), arguments.begin(), arguments.end());
    output.str(std::string());
    output.clear();
    errors.str(std::string());
    errors.clear();
    return rtm::run_cli(full, output, errors);
  };
  RTM_CHECK_EQ(run({"--store", store, "rack", "register", "--rack", "rack:r1", "--site",
                    "site:s1", "--actor", "tester", "--member",
                    "device=dev:a,unit=1,slot=0,baseline=fw:b1,trait=power.ac.208v", "--member",
                    "device=dev:b,unit=2,slot=0,baseline=fw:b1,trait=power.ac.208v"}),
               0);
  RTM_CHECK(output.str().find("rack=rack:r1") != std::string::npos);
  now_text = clock_at(1).to_text();
  RTM_CHECK_EQ(run({"--store", store, "plan", "create", "--rack", "rack:r1", "--actor",
                    "tester", "--require-power-mw", "10000", "--require-network-ports", "2",
                    "--require-redundant-feeds", "--require-topology", "topo:a", "--compat",
                    "kind=trait_on_every_member,trait=power.ac.208v,mandatory=true"}),
               0);
  RTM_CHECK(output.str().find("verdict=") != std::string::npos);
  const std::vector<std::pair<const char*, std::vector<std::string>>> evidence = {
      {"identity", {"--registry-digest", digest, "--declared-members", "2"}},
      {"power", {"--domain", "pdu:a", "--available-mw", "20000", "--applied-mw", "12000",
                  "--energized-circuits", "2", "--redundant", "--applied-verified"}},
      {"cooling", {"--domain", "cdu:a", "--capacity-mw", "20000", "--delivered-mw", "8000",
                    "--delivery-verified"}},
      {"network", {"--domain", "fabric:a", "--topology", "topo:a", "--attached-ports", "4",
                    "--reachable-ports", "4", "--authority-verified"}},
      {"inventory", {"--entry", "device=dev:a,unit=1,slot=0", "--entry",
                      "device=dev:b,unit=2,slot=0"}},
      {"health", {"--sample", "dev:a=passing", "--sample", "dev:b=passing"}},
      {"compatibility", {"--member-spec", "device=dev:a,baseline=fw:b1,trait=power.ac.208v",
                          "--member-spec", "device=dev:b,baseline=fw:b1,trait=power.ac.208v"}},
  };
  now_text = clock_at(20).to_text();
  for (const auto& item : evidence) {
    std::vector<std::string> arguments = {"--store", store, "evidence", item.first, "--rack",
                                          "rack:r1", "--actor", "tester", "--observed-at",
                                          clock_at(5).to_text()};
    for (const std::string& extra : item.second) {
      arguments.push_back(extra);
    }
    RTM_CHECK_EQ(run(arguments), 0);
  }
  now_text = clock_at(100).to_text();
  RTM_CHECK_EQ(run({"--store", store, "evaluate", "--rack", "rack:r1", "--at",
                    clock_at(100).to_text()}),
               0);
  RTM_CHECK(output.str().find("verdict=ready_to_authorize") != std::string::npos);
  now_text = clock_at(200).to_text();
  RTM_CHECK_EQ(run({"--store", store, "authorize", "--rack", "rack:r1", "--actor", "tester",
                    "--request-id", "cli-authorize"}),
               0);
  RTM_CHECK(output.str().find("attempt=at:1") != std::string::npos);
  now_text = clock_at(300).to_text();
  RTM_CHECK_EQ(run({"--store", store, "activate", "--rack", "rack:r1", "--actor", "tester",
                    "--attempt", "at:1", "--outcome", "active", "--observed-at",
                    clock_at(250).to_text()}),
               0);
  now_text = clock_at(400).to_text();
  RTM_CHECK_EQ(run({"--store", store, "commission", "--rack", "rack:r1", "--actor", "tester",
                    "--attempt", "at:1"}),
               0);
  RTM_CHECK_EQ(run({"--store", store, "summary", "--rack", "rack:r1", "--format", "json"}),
               0);
  RTM_CHECK(output.str().find("commissioned") != std::string::npos);
  RTM_CHECK_EQ(run({"--store", store, "store", "verify"}), 0);
}

RTM_TEST(cli, exit_codes) {
  rtmtest::TempDir directory("cli");
  const std::string store = directory.file("state.rtm");
  std::ostringstream output;
  std::ostringstream errors;
  RTM_CHECK_EQ(rtm::run_cli({"nonsense"}, output, errors), 1);
  RTM_CHECK_EQ(rtm::run_cli({"--store", store, "evaluate", "--rack", "rack:none"}, output, errors),
               2);
  RTM_CHECK(errors.str().find("unknown_rack_id") != std::string::npos);
  RTM_CHECK_EQ(rtm::run_cli({"--store", store, "rack", "register", "--rack", "rack:r1",
                             "--member", "device=dev:a,unit=1,slot=0"}, output, errors), 0);
  RTM_CHECK_EQ(rtm::run_cli({"--store", store, "rack", "register", "--rack", "rack:r1",
                             "--member", "device=dev:a,unit=1,slot=0"}, output, errors), 2);
  RTM_CHECK_EQ(rtm::run_cli({"--store", directory.file("missing.rtm"), "--read-only", "summary"},
                             output, errors),
               3);
}

RTM_TEST(cli, real_process_round_trip) {
  const std::string executable = rtmtest::cli_executable();
  RTM_CHECK(!executable.empty());
  rtmtest::TempDir directory("cli-process");
  const std::string store = directory.file("state.rtm");
  const rtmtest::ProcessResult result = rtmtest::run_child(
      executable, {"--store", store, "rack", "register", "--rack", "rack:p1", "--site",
                   "site:s1", "--member", "device=dev:a,unit=1,slot=0", "--member",
                   "device=dev:b,unit=2,slot=0"});
  RTM_CHECK(result.started);
  RTM_CHECK_EQ(result.exit_code, 0);
  RTM_CHECK(result.standard_output.find("rack=rack:p1") != std::string::npos);
  RTM_CHECK(rtmtest::file_exists(store));
  const rtmtest::ProcessResult version =
      rtmtest::run_child(executable, {"--version"});
  RTM_CHECK_EQ(version.exit_code, 0);
  RTM_CHECK(version.standard_output.find("1.0.0") != std::string::npos);
}

