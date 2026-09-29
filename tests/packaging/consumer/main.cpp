// Exercises the installed Rack Turnup Manager artifact.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is deliberately built outside the library source tree: it only
// sees the installed headers, the installed library and the exported CMake
// target. It drives a complete turnup cycle so that the installed artifact is
// exercised rather than merely linked.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <rack_turnup/service.hpp>
#include <rack_turnup/version.hpp>

namespace {

namespace rtm = rackturnup;

[[nodiscard]] bool fail(const std::string& message) {
  std::cerr << "consumer: " << message << "\n";
  return false;
}

[[nodiscard]] bool run(const std::string& state_path) {
  std::cout << "version=" << rtm::version_string() << "\n";
  std::cout << "configuration=" << rtm::build_configuration() << "\n";
  std::cout << "compiler=" << rtm::build_compiler() << "\n";

  const rtm::Result<rtm::RackId> rack = rtm::RackId::parse("rack:consumer");
  if (!rack.has_value()) {
    return fail(rtm::describe(rack.error()));
  }
  const rtm::Result<rtm::SiteId> site = rtm::SiteId::parse("site:lab");
  if (!site.has_value()) {
    return fail(rtm::describe(site.error()));
  }
  const rtm::Result<rtm::DeviceId> device = rtm::DeviceId::parse("dev:one");
  if (!device.has_value()) {
    return fail(rtm::describe(device.error()));
  }
  const rtm::Result<rtm::SlotCoordinate> slot = rtm::make_slot_coordinate(1, 0);
  if (!slot.has_value()) {
    return fail(rtm::describe(slot.error()));
  }

  rtm::CompositionMember member;
  member.device = device.value();
  member.slot = slot.value();
  std::vector<rtm::CompositionMember> members;
  members.push_back(member);
  const rtm::Result<rtm::RackComposition> composition = rtm::RackComposition::create(
      rack.value(), site.value(), rtm::CompositionGeneration::initial(), members);
  if (!composition.has_value()) {
    return fail(rtm::describe(composition.error()));
  }
  std::cout << "composition=" << composition.value().digest().to_hex() << "\n";

  rtm::StoreOptions options;
  options.path = state_path;
  rtm::Result<rtm::TurnupService> opened = rtm::TurnupService::open(options);
  if (!opened.has_value()) {
    return fail(rtm::describe(opened.error()));
  }
  rtm::TurnupService service = std::move(opened).value();

  rtm::MutationContext context;
  context.actor = rtm::ActorId::parse("consumer").value();
  context.authority_time = rtm::WallClock(1770000000000ll);

  rtm::RegisterRackRequest registration;
  registration.context = context;
  registration.rack = rack.value();
  registration.site = site.value();
  registration.members = members;
  const rtm::Result<rtm::RackSummary> registered = service.register_rack(registration);
  if (!registered.has_value()) {
    return fail(rtm::describe(registered.error()));
  }

  rtm::CreatePlanRequest planning;
  planning.context = context;
  planning.rack = rack.value();
  planning.expected_rack_generation = registered.value().generation;
  planning.stamp.rack = registered.value().generation;
  planning.stamp.composition = registered.value().composition_generation;
  planning.policy.generation = rtm::PolicyGeneration::initial();
  const rtm::Result<rtm::RackSummary> planned = service.create_plan(planning);
  if (!planned.has_value()) {
    return fail(rtm::describe(planned.error()));
  }

  rtm::ImportEvidenceRequest identity;
  identity.context = context;
  identity.rack = rack.value();
  identity.expected_revision = planned.value().revision;
  identity.subsystem = rtm::SubsystemKind::IdentityComposition;
  identity.payload.kind = rtm::EvidencePayloadKind::IdentityComposition;
  identity.payload.identity.registry_composition = planned.value().composition;
  identity.payload.identity.declared_members = 1;
  identity.observed_at = context.authority_time;
  identity.validity_from_policy = true;
  const rtm::Result<rtm::EvidenceReceipt> imported = service.import_evidence(identity);
  if (!imported.has_value()) {
    return fail(rtm::describe(imported.error()));
  }

  const rtm::Result<rtm::Evaluation> evaluation = service.evaluate(rack.value(), context.authority_time);
  if (!evaluation.has_value()) {
    return fail(rtm::describe(evaluation.error()));
  }
  std::cout << "verdict=" << rtm::turnup_verdict_name(evaluation.value().verdict) << "\n";
  std::cout << "verdict_digest=" << evaluation.value().verdict_digest.to_hex() << "\n";
  std::cout << "stages=" << evaluation.value().stages.size() << "\n";
  std::cout << "blockers=" << evaluation.value().blockers.size() << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string state_path = (argc > 1) ? argv[1] : std::string("consumer-state.rtm");
  return run(state_path) ? 0 : 1;
}
