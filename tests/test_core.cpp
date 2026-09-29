// Rack Turnup Manager - primitive, domain and evaluation tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/composition.hpp"
#include "rack_turnup/digest.hpp"
#include "rack_turnup/errors.hpp"
#include "rack_turnup/evaluation.hpp"
#include "rack_turnup/evidence.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/observation.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/time.hpp"
#include "rack_turnup/version.hpp"

namespace {

namespace rtm = rackturnup;
using rtmtest::must;

constexpr char kEmptySha256[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr char kAbcSha256[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr char kLongSha256[] =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr char kMillionASha256[] =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

[[nodiscard]] std::string hash_text(const std::string& text) {
  return rtm::sha256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()).to_hex();
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

constexpr std::int64_t kBaseTime = 1770000000000ll;  // 2026-02-02T02:40:00.000Z

[[nodiscard]] rtm::WallClock clock_at(std::int64_t offset_ms) {
  return rtm::WallClock(kBaseTime + offset_ms);
}

// ---------------------------------------------------------------------------
// A rack, a plan and a complete set of ready evidence.
// ---------------------------------------------------------------------------

struct Fixture {
  rtm::RackComposition composition;
  rtm::RackTurnupPlan plan;
  std::vector<rtm::EvidenceRecord> evidence;
  std::vector<rtm::TurnupAuthorization> authorizations;
  std::vector<rtm::ActivationRecord> activations;
  std::vector<rtm::CommissionRecord> commissions;
  rtm::RackGeneration rack_generation;
  rtm::RackLifecycleState lifecycle = rtm::RackLifecycleState::Planned;
  rtm::ObservationSequence sequence;
};

[[nodiscard]] Fixture make_fixture(bool allow_degraded = false,
                                   std::vector<rtm::CompatibilityRequirement> compatibility = {}) {
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
  stamp.dependency = rtm::DependencyGeneration::initial();
  stamp.power = must(rtm::PowerGeneration::create(3));
  stamp.cooling = must(rtm::CoolingGeneration::create(4));
  stamp.network = must(rtm::NetworkGeneration::create(5));
  stamp.inventory = must(rtm::InventoryGeneration::create(6));
  stamp.health = must(rtm::HealthGeneration::create(7));
  stamp.capacity = rtm::CapacityGeneration::initial();
  stamp.maintenance = rtm::MaintenanceGeneration::initial();
  stamp.policy = rtm::PolicyGeneration::initial();
  stamp.firmware = rtm::FirmwareGeneration::initial();

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
  rtm::CompatibilityRequirement requirement = must(rtm::make_compatibility_requirement(
      rtm::RequirementKind::TraitOnEveryMember, must(rtm::Trait::parse("power.ac.208v")),
      rtm::DeviceId{}, rtm::FirmwareBaselineId{}, true, rtm::Note{}));
  requirements.compatibility.push_back(std::move(requirement));
  for (rtm::CompatibilityRequirement& extra : compatibility) {
    requirements.compatibility.push_back(extra);
  }
  fixture.plan.requirements = must(rtm::make_plan_requirements(std::move(requirements)));
  fixture.plan.state = rtm::PlanState::Active;
  fixture.plan.requirements_digest = fixture.plan.compute_requirements_digest();
  fixture.plan.evidence_high_water = rtm::ObservationSequence::initial();
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
  record.source = must(rtm::SourceReference::parse("dcp:power"));
  record.producer = must(rtm::ActorId::parse("plant"));
  record.plan = fixture.plan.id;
  record.plan_revision = fixture.plan.revision;
  record.sequence = must(rtm::ObservationSequence::create(sequence));
  record.payload = std::move(payload);
  record.record_digest = record.compute_digest();
  return record;
}

[[nodiscard]] rtm::EvidencePayload identity_payload(const rtm::Digest& digest, std::uint32_t members) {
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

[[nodiscard]] rtm::EvidencePayload inventory_payload(const Fixture& fixture,
                                                    std::vector<std::string> devices) {
  std::vector<rtm::InventoryEntry> entries;
  std::uint32_t unit = 1;
  for (const std::string& device : devices) {
    rtm::InventoryEntry entry;
    entry.device = must(rtm::DeviceId::parse(device));
    entry.slot = must(rtm::make_slot_coordinate(unit, 0));
    entry.asset = must(rtm::AssetId::parse("asset:" + device));
    entries.push_back(std::move(entry));
    ++unit;
  }
  rtm::EvidencePayload payload;
  payload.kind = rtm::EvidencePayloadKind::Inventory;
  payload.inventory = must(rtm::create_inventory_observation(std::move(entries), 0));
  static_cast<void>(fixture);
  return payload;
}

[[nodiscard]] rtm::EvidencePayload health_payload(std::vector<std::pair<const char*, rtm::HealthStatus>> samples) {
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
  std::uint64_t sequence = 1;
  const rtm::GenerationStamp stamp = fixture.plan.binding.stamp;
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::IdentityComposition,
                                           identity_payload(fixture.composition.digest(), 2), 10,
                                           validity, sequence++, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Power,
                                           power_payload(20000, 12000, true, true), 20, validity,
                                           sequence++, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Cooling,
                                           cooling_payload(20000, 8000, true), 30, validity,
                                           sequence++, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Network,
                                           network_payload("topo:a", 4, 4, true), 40, validity,
                                           sequence++, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Inventory,
                                           inventory_payload(fixture, {"dev:a", "dev:b"}), 50,
                                           validity, sequence++, stamp));
  fixture.evidence.push_back(make_evidence(
      fixture, rtm::SubsystemKind::Health,
      health_payload({{"dev:a", rtm::HealthStatus::Passing}, {"dev:b", rtm::HealthStatus::Passing}}),
      60, validity, sequence++, stamp));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Compatibility,
                                           compatibility_payload(fixture, "fw:b1", true), 70,
                                           validity, sequence++, stamp));
  fixture.sequence = must(rtm::ObservationSequence::create(sequence - 1));
  fixture.plan.evidence_high_water = fixture.sequence;
}

[[nodiscard]] rtm::Evaluation evaluate_fixture(const Fixture& fixture, std::int64_t at) {
  const rtm::EvaluationContext context{fixture.composition, fixture.plan,       fixture.evidence,
                                       fixture.authorizations, fixture.activations, fixture.commissions,
                                       fixture.rack_generation, fixture.lifecycle, clock_at(at)};
  return rtm::evaluate_rack(context);
}

[[nodiscard]] rtm::SubsystemVerdict subsystem_of(const rtm::Evaluation& evaluation,
                                                rtm::SubsystemKind kind) {
  return evaluation.subsystem(kind);
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256 and digests
// ---------------------------------------------------------------------------

RTM_TEST(digest, known_answer_vectors) {
  RTM_CHECK_TEXT(hash_text(""), kEmptySha256);
  RTM_CHECK_TEXT(hash_text("abc"), kAbcSha256);
  RTM_CHECK_TEXT(hash_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 kLongSha256);
  std::string million(1000000, 'a');
  RTM_CHECK_TEXT(hash_text(million), kMillionASha256);
}

RTM_TEST(digest, incremental_matches_one_shot) {
  std::string text;
  for (int index = 0; index < 1000; ++index) {
    text.push_back(static_cast<char>('a' + (index % 26)));
  }
  const std::string expected = hash_text(text);
  for (std::size_t split = 0; split <= text.size(); split += 37) {
    rtm::Sha256 hasher;
    hasher.update(reinterpret_cast<const std::uint8_t*>(text.data()), split);
    hasher.update(reinterpret_cast<const std::uint8_t*>(text.data()) + split, text.size() - split);
    RTM_CHECK_TEXT(hasher.finish().to_hex(), expected);
  }
  rtm::Sha256 byte_wise;
  for (const char character : text) {
    byte_wise.update_byte(static_cast<std::uint8_t>(character));
  }
  RTM_CHECK_TEXT(byte_wise.finish().to_hex(), expected);
}

RTM_TEST(digest, text_form_and_zero) {
  const rtm::Digest digest = must(rtm::Digest::parse(kAbcSha256));
  RTM_CHECK_TEXT(digest.to_hex(), kAbcSha256);
  RTM_CHECK(!digest.is_zero());
  RTM_CHECK(rtm::Digest{}.is_zero());
  RTM_CHECK(!rtm::digests_equal(rtm::Digest{}, digest));
  RTM_CHECK(rtm::digests_equal(digest, digest));
  RTM_CHECK_CODE(rtm::Digest::parse("abc"), rtm::ErrorCode::InvalidDigest);
  std::string uppercase = kAbcSha256;
  uppercase[0] = 'B';
  RTM_CHECK_CODE(rtm::Digest::parse(uppercase), rtm::ErrorCode::InvalidDigest);
  RTM_CHECK(rtm::Digest::of(std::vector<std::uint8_t>(rtm::Digest::kBytes, 0)).is_zero());
  RTM_CHECK(!rtm::Digest::of(std::vector<std::uint8_t>(rtm::Digest::kBytes, 7)).is_zero());
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

RTM_TEST(text, utf8_validation) {
  RTM_CHECK(rtm::is_valid_utf8(""));
  RTM_CHECK(rtm::is_valid_utf8("plain ascii"));
  RTM_CHECK(rtm::is_valid_utf8("caf\xC3\xA9"));
  RTM_CHECK(rtm::is_valid_utf8("\xF0\x9F\x9A\x80"));
  RTM_CHECK(!rtm::is_valid_utf8("\xC0\x80"));
  RTM_CHECK(!rtm::is_valid_utf8("\xC1\xBF"));
  RTM_CHECK(!rtm::is_valid_utf8("\xED\xA0\x80"));
  RTM_CHECK(!rtm::is_valid_utf8("\xF4\x90\x80\x80"));
  RTM_CHECK(!rtm::is_valid_utf8("\xE2\x82"));
  RTM_CHECK(!rtm::is_valid_utf8("\xFF"));
  RTM_CHECK(!rtm::is_valid_utf8("\x80"));
  RTM_CHECK(rtm::has_no_control_characters("plain text"));
  RTM_CHECK(rtm::has_no_control_characters("caf\xC3\xA9"));
  RTM_CHECK(!rtm::has_no_control_characters("line\nbreak"));
  RTM_CHECK(!rtm::has_no_control_characters("tab\there"));
  RTM_CHECK(!rtm::has_no_control_characters("del\x7F"));
  RTM_CHECK(!rtm::has_no_control_characters("nel\xC2\x85"));
  RTM_CHECK(!rtm::has_no_control_characters("\xFF"));
}

RTM_TEST(text, splitting_joining_and_ordering) {
  const std::vector<std::string> parts = rtm::split("a,,b", ',');
  RTM_CHECK_EQ(parts.size(), std::size_t{3});
  RTM_CHECK_TEXT(parts[1], "");
  RTM_CHECK_TEXT(rtm::join({"a", "b", "c"}, '-'), "a-b-c");
  RTM_CHECK_TEXT(rtm::join({}, '-'), "");
  std::vector<std::string> values = {"b", "a", "b"};
  RTM_CHECK(!rtm::canonicalize(values));
  RTM_CHECK_EQ(values.size(), std::size_t{2});
  RTM_CHECK_TEXT(values[0], "a");
  std::vector<std::string> sorted = {"a", "b"};
  RTM_CHECK(rtm::canonicalize(sorted));
  RTM_CHECK(rtm::byte_less("A", "a"));
  RTM_CHECK(rtm::byte_less("ab", "abc"));
  RTM_CHECK(!rtm::byte_less("abc", "abc"));
}

RTM_TEST(text, integer_parsing) {
  std::uint64_t unsigned_value = 0;
  RTM_CHECK(rtm::parse_u64("0", unsigned_value));
  RTM_CHECK_EQ(unsigned_value, std::uint64_t{0});
  RTM_CHECK(rtm::parse_u64("18446744073709551615", unsigned_value));
  RTM_CHECK(!rtm::parse_u64("18446744073709551616", unsigned_value));
  RTM_CHECK(!rtm::parse_u64("", unsigned_value));
  RTM_CHECK(!rtm::parse_u64("-1", unsigned_value));
  RTM_CHECK(!rtm::parse_u64(" 1", unsigned_value));
  RTM_CHECK(!rtm::parse_u64("1 ", unsigned_value));
  RTM_CHECK(!rtm::parse_u64("1a", unsigned_value));
  std::int64_t signed_value = 0;
  RTM_CHECK(rtm::parse_i64("-9223372036854775808", signed_value));
  RTM_CHECK_EQ(signed_value, std::numeric_limits<std::int64_t>::min());
  RTM_CHECK(rtm::parse_i64("9223372036854775807", signed_value));
  RTM_CHECK(!rtm::parse_i64("9223372036854775808", signed_value));
  RTM_CHECK(!rtm::parse_i64("+1", signed_value));
  RTM_CHECK(!rtm::parse_i64("-", signed_value));
}

RTM_TEST(text, argument_splitting) {
  std::vector<std::string> arguments;
  RTM_CHECK(rtm::split_arguments("one  two\tthree", arguments));
  RTM_CHECK_EQ(arguments.size(), std::size_t{3});
  RTM_CHECK(rtm::split_arguments("--label \"two words\"", arguments));
  RTM_CHECK_EQ(arguments.size(), std::size_t{2});
  RTM_CHECK_TEXT(arguments[1], "two words");
  RTM_CHECK(rtm::split_arguments("--label 'single quoted'", arguments));
  RTM_CHECK_TEXT(arguments[1], "single quoted");
  RTM_CHECK(rtm::split_arguments("--x \"a\\\"b\"", arguments));
  RTM_CHECK_TEXT(arguments[1], "a\"b");
  RTM_CHECK(!rtm::split_arguments("--x \"unterminated", arguments));
  RTM_CHECK(arguments.empty());
  RTM_CHECK(rtm::split_arguments("", arguments));
  RTM_CHECK(arguments.empty());
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

RTM_TEST(time, canonical_text_round_trip) {
  RTM_CHECK_TEXT(rtm::WallClock(0).to_text(), "1970-01-01T00:00:00.000Z");
  RTM_CHECK_TEXT(rtm::WallClock(-1).to_text(), "1969-12-31T23:59:59.999Z");
  RTM_CHECK_TEXT(rtm::WallClock(1500).to_text(), "1970-01-01T00:00:01.500Z");
  const rtm::WallClock parsed = must(rtm::WallClock::parse("2026-02-11T08:15:00.250Z"));
  RTM_CHECK_TEXT(parsed.to_text(), "2026-02-11T08:15:00.250Z");
  RTM_CHECK(rtm::WallClock::parse("2026-02-11T08:15:00Z").has_value());
  RTM_CHECK(rtm::WallClock::parse("2026-02-11T08:15:00.2Z").has_value());
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-11T08:15:00.2000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-13-01T00:00:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-30T00:00:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-11T24:00:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-11T08:15:60.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-11 08:15:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("2026-02-11T08:15:00.000+01:00"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse("0000-01-01T00:00:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK_CODE(rtm::WallClock::parse(""), rtm::ErrorCode::InvalidTimestamp);
  RTM_CHECK(rtm::WallClock::parse("2024-02-29T00:00:00.000Z").has_value());
  RTM_CHECK_CODE(rtm::WallClock::parse("2023-02-29T00:00:00.000Z"),
                 rtm::ErrorCode::InvalidTimestamp);
}

RTM_TEST(time, durations_and_freshness) {
  RTM_CHECK_EQ(must(rtm::parse_duration("15m")).milliseconds(), 900000ll);
  RTM_CHECK_EQ(must(rtm::parse_duration("2h")).milliseconds(), 7200000ll);
  RTM_CHECK_EQ(must(rtm::parse_duration("1d")).milliseconds(), 86400000ll);
  RTM_CHECK_EQ(must(rtm::parse_duration("500ms")).milliseconds(), 500ll);
  RTM_CHECK_EQ(must(rtm::parse_duration("0")).milliseconds(), 0ll);
  RTM_CHECK_CODE(rtm::parse_duration("-1s"), rtm::ErrorCode::InvalidDuration);
  RTM_CHECK_CODE(rtm::parse_duration("1x"), rtm::ErrorCode::InvalidDuration);
  RTM_CHECK_CODE(rtm::parse_duration("900000000000d"), rtm::ErrorCode::InvalidDuration);
  RTM_CHECK_CODE(rtm::parse_duration(""), rtm::ErrorCode::InvalidDuration);
  RTM_CHECK_EQ(rtm::evaluate_freshness(clock_at(0), rtm::Millis(1000), clock_at(999)),
               rtm::FreshnessVerdict::Fresh);
  RTM_CHECK_EQ(rtm::evaluate_freshness(clock_at(0), rtm::Millis(1000), clock_at(1000)),
               rtm::FreshnessVerdict::Fresh);
  RTM_CHECK_EQ(rtm::evaluate_freshness(clock_at(0), rtm::Millis(1000), clock_at(1001)),
               rtm::FreshnessVerdict::Expired);
  RTM_CHECK_EQ(rtm::evaluate_freshness(clock_at(10), rtm::Millis(1000), clock_at(0)),
               rtm::FreshnessVerdict::NotYetObserved);
  RTM_CHECK_EQ(rtm::age_milliseconds(clock_at(0), clock_at(250)), 250ll);
  RTM_CHECK_EQ(rtm::age_milliseconds(clock_at(250), clock_at(0)), -250ll);
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

RTM_TEST(errors, taxonomy_is_stable) {
  RTM_CHECK_EQ(rtm::code_value(rtm::ErrorCode::Ok), std::uint16_t{0});
  RTM_CHECK_EQ(rtm::code_value(rtm::ErrorCode::TurnupNotReady), std::uint16_t{404});
  RTM_CHECK_TEXT(rtm::code_name(rtm::ErrorCode::StalePlanRevision), "stale_plan_revision");
  RTM_CHECK(rtm::category_of(rtm::ErrorCode::StalePlanRevision) == rtm::ErrorCategory::Authority);
  RTM_CHECK(rtm::category_of(rtm::ErrorCode::InventoryGap) == rtm::ErrorCategory::Readiness);
  RTM_CHECK(rtm::category_of(rtm::ErrorCode::WriterLockHeld) == rtm::ErrorCategory::Writer);
  RTM_CHECK(rtm::is_staleness_code(rtm::ErrorCode::CompositionDigestMismatch));
  RTM_CHECK(!rtm::is_staleness_code(rtm::ErrorCode::InvalidArgument));
  RTM_CHECK(rtm::is_readiness_code(rtm::ErrorCode::HealthFailed));
  RTM_CHECK(!rtm::is_readiness_code(rtm::ErrorCode::IoFailure));
  RTM_CHECK(!rtm::explain(rtm::ErrorCode::InventoryGap).empty());
  const rtm::TurnupError error = rtm::make_error(
      rtm::ErrorCode::StalePlanRevision, "plan revision does not match",
      rtm::ErrorDetail{.operation = "authorize_turnup", .expected = 3, .actual = 2});
  const std::string text = rtm::describe(error);
  RTM_CHECK(text.find("stale_plan_revision(302)") == 0);
  RTM_CHECK(text.find("expected=3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Identities and composition
// ---------------------------------------------------------------------------

RTM_TEST(ids, parsing_policy) {
  RTM_CHECK_TEXT(must(rtm::RackId::parse("rack:r1")).text(), "rack:r1");
  RTM_CHECK_CODE(rtm::RackId::parse("rack:"), rtm::ErrorCode::EmptyValue);
  RTM_CHECK_CODE(rtm::RackId::parse("r1"), rtm::ErrorCode::MalformedIdentity);
  RTM_CHECK_CODE(rtm::RackId::parse(""), rtm::ErrorCode::EmptyValue);
  RTM_CHECK_CODE(rtm::RackId::parse("rack:a b"), rtm::ErrorCode::InvalidCharacter);
  RTM_CHECK_CODE(rtm::RackId::parse(std::string(300, 'a')), rtm::ErrorCode::IdentityTooLong);
  RTM_CHECK_CODE(rtm::Trait::parse("Power"), rtm::ErrorCode::InvalidCharacter);
  RTM_CHECK(rtm::Trait::parse("power.ac.208v").has_value());
  RTM_CHECK(rtm::parse_label_or_none("").has_value());
  RTM_CHECK(rtm::parse_label_or_none("Rack one \xC3\xA9").has_value());
  RTM_CHECK_CODE(rtm::parse_label_or_none("bad\x01label"), rtm::ErrorCode::InvalidCharacter);
  RTM_CHECK(rtm::parse_note_or_none("").has_value());
  RTM_CHECK_EQ(rtm::RackGeneration::create(0).has_value(), false);
  RTM_CHECK(rtm::RackGeneration::initial().value() == 1);
  RTM_CHECK(rtm::ObservationSequence::initial().value() == 0);
  const rtm::RackGeneration generation = rtm::RackGeneration::initial();
  RTM_CHECK(generation.next().value() == 2);
}

RTM_TEST(composition, validation_and_digest) {
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  const rtm::SiteId site = must(rtm::SiteId::parse("site:s1"));
  std::vector<rtm::CompositionMember> members;
  members.push_back(make_member("dev:b", 2, 0, {"power.ac.208v"}, "fw:b1"));
  members.push_back(make_member("dev:a", 1, 0, {"power.ac.208v"}, "fw:b1"));
  const rtm::RackComposition composition = must(rtm::RackComposition::create(
      rack, site, rtm::CompositionGeneration::initial(), members));
  RTM_CHECK_EQ(composition.size(), std::size_t{2});
  RTM_CHECK_TEXT(composition.members()[0].device.text(), "dev:a");
  RTM_CHECK_TEXT(composition.members()[1].device.text(), "dev:b");
  RTM_CHECK(composition.find(must(rtm::DeviceId::parse("dev:a"))) != nullptr);
  RTM_CHECK(composition.find(must(rtm::DeviceId::parse("dev:z"))) == nullptr);
  RTM_CHECK(rtm::digests_equal(composition.digest(), composition.compute_digest()));

  std::vector<rtm::CompositionMember> reordered;
  reordered.push_back(members[1]);
  reordered.push_back(members[0]);
  const rtm::RackComposition same = must(rtm::RackComposition::create(
      rack, site, rtm::CompositionGeneration::initial(), reordered));
  RTM_CHECK(rtm::digests_equal(composition.digest(), same.digest()));

  std::vector<rtm::CompositionMember> labelled = reordered;
  labelled[0].label = must(rtm::DisplayLabel::parse("left"));
  const rtm::RackComposition with_label = must(rtm::RackComposition::create(
      rack, site, rtm::CompositionGeneration::initial(), labelled));
  RTM_CHECK(!rtm::digests_equal(composition.digest(), with_label.digest()));

  RTM_CHECK_CODE(rtm::RackComposition::create(rack, site, rtm::CompositionGeneration::initial(), {}),
                 rtm::ErrorCode::CompositionEmpty);
  std::vector<rtm::CompositionMember> duplicated;
  duplicated.push_back(make_member("dev:a", 1, 0));
  duplicated.push_back(make_member("dev:a", 2, 0));
  RTM_CHECK_CODE(
      rtm::RackComposition::create(rack, site, rtm::CompositionGeneration::initial(), duplicated),
      rtm::ErrorCode::DuplicateDeviceId);
  std::vector<rtm::CompositionMember> same_slot;
  same_slot.push_back(make_member("dev:a", 1, 0));
  same_slot.push_back(make_member("dev:b", 1, 0));
  RTM_CHECK_CODE(
      rtm::RackComposition::create(rack, site, rtm::CompositionGeneration::initial(), same_slot),
      rtm::ErrorCode::InvalidCoordinate);
  std::vector<rtm::CompositionMember> bad_slot;
  rtm::CompositionMember member = make_member("dev:a", 1, 0);
  member.slot.unit = 0;
  bad_slot.push_back(member);
  RTM_CHECK_CODE(
      rtm::RackComposition::create(rack, site, rtm::CompositionGeneration::initial(), bad_slot),
      rtm::ErrorCode::InvalidCoordinate);
  RTM_CHECK_CODE(rtm::make_slot_coordinate(0, 0), rtm::ErrorCode::InvalidCoordinate);
  RTM_CHECK_CODE(rtm::make_slot_coordinate(1, 256), rtm::ErrorCode::InvalidCoordinate);
  RTM_CHECK_TEXT(must(rtm::parse_slot_coordinate("u12.s3")).to_text(), "u12.s3");
  RTM_CHECK_CODE(rtm::parse_slot_coordinate("12.3"), rtm::ErrorCode::InvalidCoordinate);
}

RTM_TEST(composition, trait_sets) {
  RTM_CHECK(must(rtm::TraitSet::create({})).empty());
  std::vector<rtm::Trait> traits;
  traits.push_back(must(rtm::Trait::parse("z.last")));
  traits.push_back(must(rtm::Trait::parse("a.first")));
  const rtm::TraitSet set = must(rtm::TraitSet::create(std::move(traits)));
  RTM_CHECK_EQ(set.size(), std::size_t{2});
  RTM_CHECK_TEXT(set.traits()[0].text(), "a.first");
  RTM_CHECK(set.contains(must(rtm::Trait::parse("z.last"))));
  RTM_CHECK(!set.contains(must(rtm::Trait::parse("nope"))));
  RTM_CHECK_TEXT(set.to_text(','), "a.first,z.last");
  std::vector<rtm::Trait> duplicated;
  duplicated.push_back(must(rtm::Trait::parse("a")));
  duplicated.push_back(must(rtm::Trait::parse("a")));
  RTM_CHECK_CODE(rtm::TraitSet::create(std::move(duplicated)), rtm::ErrorCode::InvalidArgument);
}

RTM_TEST(composition, inventory_closure) {
  const rtm::RackId rack = must(rtm::RackId::parse("rack:r1"));
  const rtm::SiteId site = must(rtm::SiteId::parse("site:s1"));
  std::vector<rtm::CompositionMember> members;
  members.push_back(make_member("dev:a", 1, 0));
  members.push_back(make_member("dev:b", 2, 0));
  const rtm::RackComposition composition = must(rtm::RackComposition::create(
      rack, site, rtm::CompositionGeneration::initial(), members));

  const auto entry = [](const char* device, std::uint32_t unit, bool readable) {
    rtm::InventoryEntry parsed;
    parsed.device = must(rtm::DeviceId::parse(device));
    parsed.slot = must(rtm::make_slot_coordinate(unit, 0));
    parsed.readable = readable;
    return parsed;
  };

  const rtm::InventoryObservation closed = must(rtm::create_inventory_observation(
      {entry("dev:a", 1, true), entry("dev:b", 2, true)}, 0));
  const rtm::InventoryClosure closed_result = must(rtm::close_inventory(composition, closed));
  RTM_CHECK(closed_result.is_closed());
  RTM_CHECK(closed_result.code == rtm::ErrorCode::Ok);
  RTM_CHECK_EQ(closed_result.observed_members.value(), std::uint64_t{2});

  const rtm::InventoryObservation missing = must(rtm::create_inventory_observation(
      {entry("dev:a", 1, true)}, 0));
  const rtm::InventoryClosure gap = must(rtm::close_inventory(composition, missing));
  RTM_CHECK(!gap.is_closed());
  RTM_CHECK(gap.code == rtm::ErrorCode::InventoryGap);
  RTM_CHECK_EQ(gap.missing_devices.size(), std::size_t{1});

  const rtm::InventoryObservation unknown = must(rtm::create_inventory_observation(
      {entry("dev:a", 1, true), entry("dev:b", 2, true), entry("dev:z", 3, true)}, 0));
  const rtm::InventoryClosure ambiguous = must(rtm::close_inventory(composition, unknown));
  RTM_CHECK(ambiguous.state == rtm::InventoryClosureState::Ambiguous);
  RTM_CHECK(ambiguous.code == rtm::ErrorCode::UnknownCompositionMember);

  const rtm::InventoryObservation displaced = must(rtm::create_inventory_observation(
      {entry("dev:a", 9, true), entry("dev:b", 2, true)}, 0));
  RTM_CHECK(must(rtm::close_inventory(composition, displaced)).code ==
             rtm::ErrorCode::InventoryAmbiguous);

  const rtm::InventoryObservation unreadable = must(rtm::create_inventory_observation(
      {entry("dev:a", 1, false), entry("dev:b", 2, true)}, 0));
  const rtm::InventoryClosure unreadable_result =
      must(rtm::close_inventory(composition, unreadable));
  RTM_CHECK(unreadable_result.code == rtm::ErrorCode::InventoryUnreadable);
  RTM_CHECK(unreadable_result.state == rtm::InventoryClosureState::Gap);

  const rtm::InventoryObservation duplicate_slot = must(rtm::create_inventory_observation(
      {entry("dev:a", 1, true), entry("dev:b", 1, true)}, 0));
  RTM_CHECK(must(rtm::close_inventory(composition, duplicate_slot)).code ==
             rtm::ErrorCode::DuplicateInventorySlot);

  RTM_CHECK_CODE(rtm::create_inventory_observation({entry("dev:a", 1, true),
                                                   entry("dev:a", 2, true)}, 0),
                 rtm::ErrorCode::DuplicateInventoryEntry);
}

// ---------------------------------------------------------------------------
// Evidence selection
// ---------------------------------------------------------------------------

RTM_TEST(evidence, newest_eligible_record_wins) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  const rtm::GenerationStamp stamp = fixture.plan.binding.stamp;
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Power,
                                           power_payload(99000, 99000, true, true), 100, 900000, 20,
                                           stamp));
  const rtm::EvidenceSelection selection = rtm::select_evidence(
      fixture.evidence, fixture.plan.binding, clock_at(120));
  RTM_CHECK(selection.for_subsystem(rtm::SubsystemKind::Power) != nullptr);
  RTM_CHECK_EQ(selection.for_subsystem(rtm::SubsystemKind::Power)->sequence.value(),
               std::uint64_t{20});
  RTM_CHECK_EQ(selection.superseded_count, std::size_t{1});
  RTM_CHECK_EQ(selection.eligible_count, std::size_t{7});
  RTM_CHECK_EQ(selection.rejected_count, std::size_t{0});
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 120);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).state == rtm::ReadinessState::Ready);
}

RTM_TEST(evidence, ineligible_records_are_reported) {
  Fixture fixture = make_fixture();
  add_all_evidence(fixture);
  const rtm::GenerationStamp stamp = fixture.plan.binding.stamp;
  // Expired, future dated, wrong generation and tampered records.
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Power,
                                           power_payload(1, 1, false, false), 10, 1, 30, stamp));
  rtm::GenerationStamp shifted = stamp;
  shifted.power = must(rtm::PowerGeneration::create(99));
  fixture.evidence.push_back(make_evidence(fixture, rtm::SubsystemKind::Cooling,
                                           cooling_payload(1, 1, true), 10, 900000, 31, shifted));
  rtm::EvidenceRecord future = make_evidence(fixture, rtm::SubsystemKind::Network,
                                             network_payload("topo:a", 1, 1, true), 5000, 900000, 32,
                                             stamp);
  fixture.evidence.push_back(future);
  rtm::EvidenceRecord tampered = make_evidence(fixture, rtm::SubsystemKind::Health,
                                               health_payload({{"dev:a", rtm::HealthStatus::Passing}}), 10,
                                               900000, 33, stamp);
  tampered.payload.health.samples[0].status = rtm::HealthStatus::Failing;
  fixture.evidence.push_back(tampered);

  const rtm::EvidenceSelection selection =
      rtm::select_evidence(fixture.evidence, fixture.plan.binding, clock_at(1000));
  RTM_CHECK(selection.rejected_count >= 4);
  const rtm::Evaluation evaluation = evaluate_fixture(fixture, 1000);
  RTM_CHECK(evaluation.rejected_evidence >= 4);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Power).state == rtm::ReadinessState::Ready);
  RTM_CHECK(evaluation.subsystem(rtm::SubsystemKind::Cooling).state == rtm::ReadinessState::Ready);
}

RTM_TEST(evidence, selection_is_order_independent) {
  Fixture first = make_fixture();
  add_all_evidence(first);
  Fixture second = make_fixture();
  add_all_evidence(second);
  std::reverse(second.evidence.begin(), second.evidence.end());
  const rtm::Evaluation left = evaluate_fixture(first, 100);
  const rtm::Evaluation right = evaluate_fixture(second, 100);
  RTM_CHECK(rtm::digests_equal(left.verdict_digest, right.verdict_digest));
  RTM_CHECK(rtm::digests_equal(left.evidence_set, right.evidence_set));
}
