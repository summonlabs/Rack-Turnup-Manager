// Rack Turnup Manager - canonical durable encoding of the service state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The payload is deterministic. Integers are fixed width and big-endian, a
// boolean is exactly one byte, text is a u32 byte count followed by its bytes,
// a collection is a u32 count followed by its elements, and a digest is 32 raw
// bytes. Nothing is padded and nothing is inferred, so two equal states always
// produce identical bytes. Map-like data is already held in sorted vectors, so
// it is written in the order the state stores it.
//
// Decoding is the exact inverse with one rule: every field is re-validated
// through the same vocabulary the in-memory API uses, so a value this library
// would never have produced is rejected rather than repaired.

#include "rack_turnup/state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rack_turnup/errors.hpp"
#include "rack_turnup/limits.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

// The payload opens with the durable format version and a reserved word. Both
// are checked before any other field is read, so a reader never guesses at a
// layout it does not implement and a future format cannot be read as this one.
constexpr std::uint32_t kReservedWord = 0;

// limits.hpp documents a bound for every text field of the state except the two
// untyped free-text fields, AuthorizationFence::explanation and
// RejectionRecord::message. A local bound applies to those as well: a declared
// length is never trusted on the strength of the remaining buffer alone.
constexpr std::size_t kMaxFreeTextBytes = 4096;

// Largest value a u32 count or length field can carry.
constexpr std::size_t kMaxEncodedCount = static_cast<std::size_t>(0xFFFFFFFFu);

[[nodiscard]] TurnupError error_invalid_enum(std::string_view field, std::uint64_t value) {
  return make_error(ErrorCode::InvalidEnumValue,
                    "an encoded enumeration value is not part of the vocabulary",
                    ErrorDetail{.operation = "decode_state", .subject = std::string(field),
                                .actual = value});
}

[[nodiscard]] TurnupError error_invalid_boolean(std::string_view field, std::uint8_t value) {
  return make_error(ErrorCode::CorruptState, "an encoded boolean is neither zero nor one",
                    ErrorDetail{.operation = "decode_state", .subject = std::string(field),
                                .actual = value});
}

[[nodiscard]] TurnupError error_truncated(std::string_view field) {
  return make_error(ErrorCode::TruncatedState,
                    "the payload ends before a declared field is complete",
                    ErrorDetail{.operation = "decode_state", .subject = std::string(field)});
}

[[nodiscard]] TurnupError error_limit_exceeded(std::string_view field, std::size_t bound,
                                               std::size_t actual) {
  return make_error(ErrorCode::LimitExceeded,
                    "a declared count or length is above its documented bound",
                    ErrorDetail{.operation = "decode_state", .subject = std::string(field),
                                .expected = bound, .actual = actual});
}

// True when a value names a code of the error taxonomy. Any other value would
// otherwise be accepted as an error code that no report could name.
[[nodiscard]] bool is_known_error_code(std::uint16_t value) noexcept {
  return code_name(static_cast<ErrorCode>(value)) != std::string_view{"unrecognized_error_code"};
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// A cursor over the payload. Every read is bounds checked, and the first
// failure is sticky: once a field cannot be read, the remaining reads return a
// default without touching the buffer, so a caller only has to check ok() after
// the outermost record it was reading.
class StateReader final {
 public:
  StateReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] const TurnupError& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }

  void fail(TurnupError error) {
    if (!failed_) {
      failed_ = true;
      error_ = std::move(error);
    }
  }

  [[nodiscard]] std::uint8_t u8(std::string_view field) {
    return static_cast<std::uint8_t>(unsigned_value(1u, field));
  }
  [[nodiscard]] std::uint16_t u16(std::string_view field) {
    return static_cast<std::uint16_t>(unsigned_value(2u, field));
  }
  [[nodiscard]] std::uint32_t u32(std::string_view field) {
    return static_cast<std::uint32_t>(unsigned_value(4u, field));
  }
  [[nodiscard]] std::uint64_t u64(std::string_view field) { return unsigned_value(8u, field); }

  // A signed 64-bit field is read as its two's complement bit pattern.
  [[nodiscard]] std::int64_t i64(std::string_view field) {
    return static_cast<std::int64_t>(unsigned_value(8u, field));
  }

  [[nodiscard]] bool boolean(std::string_view field) {
    const std::uint8_t value = u8(field);
    if (!ok()) {
      return false;
    }
    if (value > 1) {
      fail(error_invalid_boolean(field, value));
      return false;
    }
    return value == 1;
  }

  [[nodiscard]] Digest digest(std::string_view field) {
    if (!take(Digest::kBytes, field)) {
      return Digest{};
    }
    std::vector<std::uint8_t> bytes(data_ + offset_, data_ + offset_ + Digest::kBytes);
    offset_ += Digest::kBytes;
    return Digest::of(std::move(bytes));
  }

  // Length-prefixed text. The declared length is checked against the bound of
  // the field and against the bytes that remain before a single byte is copied.
  [[nodiscard]] std::string text(std::size_t bound, std::string_view field) {
    const std::uint32_t declared = u32(field);
    if (!ok()) {
      return std::string{};
    }
    if (declared > bound) {
      fail(error_limit_exceeded(field, bound, declared));
      return std::string{};
    }
    if (declared > remaining()) {
      fail(error_truncated(field));
      return std::string{};
    }
    const std::string value(reinterpret_cast<const char*>(data_ + offset_), declared);
    offset_ += declared;
    return value;
  }

  // Element count of a collection. Both checks happen before a container is
  // allowed to grow.
  [[nodiscard]] std::uint32_t count(std::size_t bound, std::string_view field) {
    const std::uint32_t declared = u32(field);
    if (!ok()) {
      return 0;
    }
    if (declared > bound) {
      fail(error_limit_exceeded(field, bound, declared));
      return 0;
    }
    if (declared > remaining()) {
      fail(error_truncated(field));
      return 0;
    }
    return declared;
  }

 private:
  [[nodiscard]] bool take(std::size_t width, std::string_view field) {
    if (failed_) {
      return false;
    }
    if (width > remaining()) {
      fail(error_truncated(field));
      return false;
    }
    return true;
  }

  [[nodiscard]] std::uint64_t unsigned_value(std::size_t width, std::string_view field) {
    if (!take(width, field)) {
      return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value = (value << 8) | static_cast<std::uint64_t>(data_[offset_ + index]);
    }
    offset_ += width;
    return value;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  bool failed_ = false;
  TurnupError error_;
};

// Reads length-prefixed text and rebuilds it through the parser of its own
// type, so an identity or a label can never be smuggled in as a raw string; a
// parser error is reported exactly as the parser produced it.
//
// An empty field decodes to the default value of its type, because several
// fields of the state are empty by construction: the fields a compatibility
// requirement's kind does not use, the payloads a tagged evidence record does
// not select, and the plan, evidence and attempt identities that a receipt
// leaves empty when the request it replayed had none. A non-empty field is
// always parsed, so no unvalidated text can ever become an identity.
template <typename Text, typename Parse>
[[nodiscard]] Text read_identity(StateReader& reader, std::size_t bound, std::string_view field,
                                 Parse parse) {
  const std::string text = reader.text(bound, field);
  if (!reader.ok()) {
    return Text{};
  }
  if (text.empty()) {
    return Text{};
  }
  const Result<Text> parsed = parse(std::string_view(text));
  if (!parsed.has_value()) {
    reader.fail(parsed.error());
    return Text{};
  }
  return parsed.value();
}

// Counter values have no public constructor from a raw integer, so a decoded
// value is rebuilt through the factory of its own type. A factory that refuses
// the value is a positive counter encoded as zero, and the default-constructed
// counter holds exactly that zero, so decoding stays a faithful inverse of
// encoding; validate_state rejects the epochs that must never be zero.
template <typename Counter>
[[nodiscard]] Counter read_counter(StateReader& reader, std::string_view field) {
  const std::uint64_t value = reader.u64(field);
  if (!reader.ok()) {
    return Counter{};
  }
  const Result<Counter> created = Counter::create(value);
  if (!created.has_value()) {
    return Counter{};
  }
  return created.value();
}

// Durations are bounded so that age and freshness arithmetic stays total.
[[nodiscard]] Millis read_duration(StateReader& reader, std::string_view field) {
  const std::int64_t value = reader.i64(field);
  if (!reader.ok()) {
    return Millis{};
  }
  const Result<Millis> duration = Millis::create(value);
  if (!duration.has_value()) {
    reader.fail(duration.error());
    return Millis{};
  }
  return duration.value();
}

// Coordinates are bounded so that a decoded position can never overflow an
// offset computation.
[[nodiscard]] SlotCoordinate read_slot(StateReader& reader, std::string_view field) {
  const std::uint64_t unit = reader.u32(field);
  const std::uint64_t slot = reader.u32(field);
  if (!reader.ok()) {
    return SlotCoordinate{};
  }
  const Result<SlotCoordinate> coordinate = make_slot_coordinate(unit, slot);
  if (!coordinate.has_value()) {
    reader.fail(coordinate.error());
    return SlotCoordinate{};
  }
  return coordinate.value();
}

[[nodiscard]] ErrorCode read_error_code(StateReader& reader, std::string_view field) {
  const std::uint16_t value = reader.u16(field);
  if (!reader.ok()) {
    return ErrorCode::Ok;
  }
  if (!is_known_error_code(value)) {
    reader.fail(error_invalid_enum(field, value));
    return ErrorCode::Ok;
  }
  return static_cast<ErrorCode>(value);
}

[[nodiscard]] SubsystemKind read_subsystem(StateReader& reader, std::string_view field) {
  const std::uint8_t value = reader.u8(field);
  if (!reader.ok()) {
    return SubsystemKind::IdentityComposition;
  }
  if (!is_known_subsystem_kind(value)) {
    reader.fail(error_invalid_enum(field, value));
    return SubsystemKind::IdentityComposition;
  }
  return static_cast<SubsystemKind>(value);
}

// A one-byte enumeration with a checked inclusive range.
template <typename Enum>
[[nodiscard]] Enum read_enum(StateReader& reader, std::string_view field, int lowest,
                             int highest) {
  const std::uint8_t value = reader.u8(field);
  if (!reader.ok()) {
    return static_cast<Enum>(lowest);
  }
  if (static_cast<int>(value) < lowest || static_cast<int>(value) > highest) {
    reader.fail(error_invalid_enum(field, value));
    return static_cast<Enum>(lowest);
  }
  return static_cast<Enum>(value);
}

void require_digest(StateReader& reader, std::string_view field, const Digest& decoded,
                    const Digest& computed) {
  if (digests_equal(decoded, computed)) {
    return;
  }
  reader.fail(make_error(ErrorCode::IntegrityCheckFailed,
                         "a decoded digest does not match the contents it covers",
                         ErrorDetail{.operation = "decode_state",
                                     .subject = std::string(field)}));
}

[[nodiscard]] GenerationStamp read_generation_stamp(StateReader& reader) {
  GenerationStamp stamp;
  stamp.rack = read_counter<RackGeneration>(reader, "stamp.rack");
  stamp.composition = read_counter<CompositionGeneration>(reader, "stamp.composition");
  stamp.topology = read_counter<TopologyGeneration>(reader, "stamp.topology");
  stamp.dependency = read_counter<DependencyGeneration>(reader, "stamp.dependency");
  stamp.power = read_counter<PowerGeneration>(reader, "stamp.power");
  stamp.cooling = read_counter<CoolingGeneration>(reader, "stamp.cooling");
  stamp.network = read_counter<NetworkGeneration>(reader, "stamp.network");
  stamp.inventory = read_counter<InventoryGeneration>(reader, "stamp.inventory");
  stamp.health = read_counter<HealthGeneration>(reader, "stamp.health");
  stamp.capacity = read_counter<CapacityGeneration>(reader, "stamp.capacity");
  stamp.maintenance = read_counter<MaintenanceGeneration>(reader, "stamp.maintenance");
  stamp.policy = read_counter<PolicyGeneration>(reader, "stamp.policy");
  stamp.firmware = read_counter<FirmwareGeneration>(reader, "stamp.firmware");
  return stamp;
}

[[nodiscard]] TraitSet read_trait_set(StateReader& reader, std::string_view field) {
  const std::uint32_t declared = reader.count(kMaxTraitsPerSet, field);
  std::vector<Trait> traits;
  if (reader.ok()) {
    traits.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    traits.push_back(read_identity<Trait>(reader, kMaxTraitBytes, "trait", Trait::parse));
  }
  if (!reader.ok()) {
    return TraitSet{};
  }
  const Result<TraitSet> created = TraitSet::create(std::move(traits));
  if (!created.has_value()) {
    reader.fail(created.error());
    return TraitSet{};
  }
  return created.value();
}

[[nodiscard]] IdentityCompositionObservation read_identity_observation(StateReader& reader) {
  IdentityCompositionObservation observation;
  observation.registry_composition = reader.digest("payload.identity.registry_digest");
  observation.declared_members = reader.u32("payload.identity.declared_members");
  observation.unreadable_members = reader.u32("payload.identity.unreadable_members");
  return observation;
}

[[nodiscard]] PowerObservation read_power_observation(StateReader& reader) {
  PowerObservation observation;
  observation.domain =
      read_identity<PowerDomainReference>(reader, kMaxOpaqueReferenceBytes, "payload.power.domain",
                                      PowerDomainReference::parse);
  observation.available_milliwatts = reader.u64("payload.power.available_milliwatts");
  observation.applied_milliwatts = reader.u64("payload.power.applied_milliwatts");
  observation.energized_circuits = reader.u32("payload.power.energized_circuits");
  observation.redundant_feeds_present = reader.boolean("payload.power.redundant_feeds_present");
  observation.applied_verified = reader.boolean("payload.power.applied_verified");
  return observation;
}

[[nodiscard]] CoolingObservation read_cooling_observation(StateReader& reader) {
  CoolingObservation observation;
  observation.domain =
      read_identity<CoolingDomainReference>(reader, kMaxOpaqueReferenceBytes,
                                            "payload.cooling.domain",
                                            CoolingDomainReference::parse);
  observation.capacity_milliwatts = reader.u64("payload.cooling.capacity_milliwatts");
  observation.delivered_milliwatts = reader.u64("payload.cooling.delivered_milliwatts");
  observation.inlet_millidegrees_c = reader.u32("payload.cooling.inlet_millidegrees_c");
  observation.max_inlet_millidegrees_c = reader.u32("payload.cooling.max_inlet_millidegrees_c");
  observation.delivery_verified = reader.boolean("payload.cooling.delivery_verified");
  return observation;
}

[[nodiscard]] NetworkObservation read_network_observation(StateReader& reader) {
  NetworkObservation observation;
  observation.domain =
      read_identity<NetworkDomainReference>(reader, kMaxOpaqueReferenceBytes,
                                            "payload.network.domain",
                                            NetworkDomainReference::parse);
  observation.topology = read_identity<FabricTopologyReference>(
      reader, kMaxOpaqueReferenceBytes, "payload.network.topology",
      FabricTopologyReference::parse);
  observation.attached_ports = reader.u32("payload.network.attached_ports");
  observation.reachable_ports = reader.u32("payload.network.reachable_ports");
  observation.authority_verified = reader.boolean("payload.network.authority_verified");
  return observation;
}

[[nodiscard]] InventoryEntry read_inventory_entry(StateReader& reader) {
  InventoryEntry entry;
  entry.device =
      read_identity<DeviceId>(reader, kMaxIdentityTextBytes, "inventory.device", DeviceId::parse);
  entry.asset = read_identity<AssetId>(reader, kMaxOpaqueReferenceBytes, "inventory.asset",
                                   AssetId::parse);
  entry.firmware_baseline = read_identity<FirmwareBaselineId>(
      reader, kMaxIdentityTextBytes, "inventory.firmware_baseline", FirmwareBaselineId::parse);
  entry.traits = read_trait_set(reader, "inventory.traits");
  entry.slot = read_slot(reader, "inventory.slot");
  entry.readable = reader.boolean("inventory.readable");
  return entry;
}

[[nodiscard]] InventoryObservation read_inventory_observation(StateReader& reader) {
  const std::uint32_t declared = reader.count(kMaxInventoryEntries, "payload.inventory.entries");
  std::vector<InventoryEntry> entries;
  if (reader.ok()) {
    entries.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    entries.push_back(read_inventory_entry(reader));
  }
  InventoryObservation observation;
  observation.unreadable_positions = reader.u32("payload.inventory.unreadable_positions");
  if (!reader.ok()) {
    return InventoryObservation{};
  }
  observation.entries = std::move(entries);
  return observation;
}

[[nodiscard]] DeviceHealthSample read_health_sample(StateReader& reader) {
  DeviceHealthSample sample;
  sample.device =
      read_identity<DeviceId>(reader, kMaxIdentityTextBytes, "health.device", DeviceId::parse);
  sample.status = read_enum<HealthStatus>(reader, "health.status", 0, 3);
  sample.note = read_identity<Note>(reader, kMaxNoteBytes, "health.note", parse_note_or_none);
  return sample;
}

[[nodiscard]] HealthObservation read_health_observation(StateReader& reader) {
  const std::uint32_t declared = reader.count(kMaxObservationEntries, "payload.health.samples");
  std::vector<DeviceHealthSample> samples;
  if (reader.ok()) {
    samples.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    samples.push_back(read_health_sample(reader));
  }
  HealthObservation observation;
  observation.unchecked_devices = reader.u32("payload.health.unchecked_devices");
  if (!reader.ok()) {
    return HealthObservation{};
  }
  observation.samples = std::move(samples);
  return observation;
}

[[nodiscard]] CompatibilityMembership read_compatibility_membership(StateReader& reader) {
  CompatibilityMembership membership;
  membership.device =
      read_identity<DeviceId>(reader, kMaxIdentityTextBytes, "compatibility.device",
                              DeviceId::parse);
  membership.firmware_baseline = read_identity<FirmwareBaselineId>(
      reader, kMaxIdentityTextBytes, "compatibility.firmware_baseline",
      FirmwareBaselineId::parse);
  membership.traits = read_trait_set(reader, "compatibility.traits");
  return membership;
}

[[nodiscard]] CompatibilityObservation read_compatibility_observation(StateReader& reader) {
  const std::uint32_t declared = reader.count(kMaxObservationEntries, "payload.compatibility");
  std::vector<CompatibilityMembership> members;
  if (reader.ok()) {
    members.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    members.push_back(read_compatibility_membership(reader));
  }
  CompatibilityObservation observation;
  if (!reader.ok()) {
    return CompatibilityObservation{};
  }
  observation.members = std::move(members);
  return observation;
}

// Every payload field is written and read, not only the one the tag selects, so
// the layout of a record never depends on its content. validate_state rejects a
// record whose other payload fields are not empty.
[[nodiscard]] EvidencePayload read_payload(StateReader& reader) {
  EvidencePayload payload;
  payload.kind = read_enum<EvidencePayloadKind>(reader, "payload.kind", 1, 7);
  payload.identity = read_identity_observation(reader);
  payload.power = read_power_observation(reader);
  payload.cooling = read_cooling_observation(reader);
  payload.network = read_network_observation(reader);
  payload.inventory = read_inventory_observation(reader);
  payload.health = read_health_observation(reader);
  payload.compatibility = read_compatibility_observation(reader);
  return payload;
}

[[nodiscard]] CompositionMember read_composition_member(StateReader& reader) {
  CompositionMember member;
  member.device = read_identity<DeviceId>(reader, kMaxIdentityTextBytes,
                                          "composition.member.device", DeviceId::parse);
  member.asset = read_identity<AssetId>(reader, kMaxOpaqueReferenceBytes,
                                        "composition.member.asset", AssetId::parse);
  member.firmware_baseline = read_identity<FirmwareBaselineId>(
      reader, kMaxIdentityTextBytes, "composition.member.firmware_baseline",
      FirmwareBaselineId::parse);
  member.traits = read_trait_set(reader, "composition.member.traits");
  member.slot = read_slot(reader, "composition.member.slot");
  member.label = read_identity<DisplayLabel>(reader, kMaxLabelBytes, "composition.member.label",
                                         parse_label_or_none);
  return member;
}

// The stored digest is a check value: it is compared with the digest of the
// composition that was rebuilt from the decoded fields.
[[nodiscard]] RackComposition read_composition(StateReader& reader) {
  const Digest check = reader.digest("composition.digest");
  const RackId rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "composition.rack", RackId::parse);
  const SiteId site =
      read_identity<SiteId>(reader, kMaxIdentityTextBytes, "composition.site", SiteId::parse);
  const CompositionGeneration generation =
      read_counter<CompositionGeneration>(reader, "composition.generation");
  const std::uint32_t declared = reader.count(kMaxCompositionMembers, "composition.members");
  std::vector<CompositionMember> members;
  if (reader.ok()) {
    members.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    members.push_back(read_composition_member(reader));
  }
  if (!reader.ok()) {
    return RackComposition{};
  }
  const Result<RackComposition> created =
      RackComposition::create(rack, site, generation, std::move(members));
  if (!created.has_value()) {
    reader.fail(created.error());
    return RackComposition{};
  }
  require_digest(reader, "composition.digest", check, created.value().digest());
  return created.value();
}

[[nodiscard]] CompatibilityRequirement read_requirement(StateReader& reader) {
  CompatibilityRequirement requirement;
  requirement.kind = read_enum<RequirementKind>(reader, "requirement.kind", 1, 5);
  requirement.trait =
      read_identity<Trait>(reader, kMaxTraitBytes, "requirement.trait", Trait::parse);
  requirement.device = read_identity<DeviceId>(reader, kMaxIdentityTextBytes, "requirement.device",
                                           DeviceId::parse);
  requirement.firmware_baseline = read_identity<FirmwareBaselineId>(
      reader, kMaxIdentityTextBytes, "requirement.firmware_baseline", FirmwareBaselineId::parse);
  requirement.mandatory = reader.boolean("requirement.mandatory");
  requirement.note =
      read_identity<Note>(reader, kMaxNoteBytes, "requirement.note", parse_note_or_none);
  return requirement;
}

[[nodiscard]] PlanRequirements read_requirements(StateReader& reader) {
  PlanRequirements requirements;
  requirements.required_power_milliwatts = reader.u64("requirements.required_power_milliwatts");
  requirements.required_cooling_milliwatts = reader.u64("requirements.required_cooling_milliwatts");
  requirements.required_network_ports = reader.u32("requirements.required_network_ports");
  requirements.require_redundant_feeds = reader.boolean("requirements.require_redundant_feeds");
  requirements.require_applied_power_verification =
      reader.boolean("requirements.require_applied_power_verification");
  requirements.require_cooling_delivery_verification =
      reader.boolean("requirements.require_cooling_delivery_verification");
  requirements.require_network_authority_verification =
      reader.boolean("requirements.require_network_authority_verification");
  requirements.require_health_pass_for_every_member =
      reader.boolean("requirements.require_health_pass_for_every_member");
  requirements.required_topology =
      read_identity<FabricTopologyReference>(reader, kMaxOpaqueReferenceBytes,
                                         "requirements.required_topology",
                                         FabricTopologyReference::parse);
  const std::uint32_t declared =
      reader.count(kMaxCompatibilityRequirements, "requirements.compatibility");
  std::vector<CompatibilityRequirement> compatibility;
  if (reader.ok()) {
    compatibility.reserve(declared);
  }
  for (std::uint32_t index = 0; index < declared && reader.ok(); ++index) {
    compatibility.push_back(read_requirement(reader));
  }
  if (!reader.ok()) {
    return PlanRequirements{};
  }
  requirements.compatibility = std::move(compatibility);
  return requirements;
}

[[nodiscard]] TurnupPolicy read_policy(StateReader& reader) {
  TurnupPolicy policy;
  policy.generation = read_counter<PolicyGeneration>(reader, "policy.generation");
  policy.allow_degraded_subsystems = reader.boolean("policy.allow_degraded_subsystems");
  policy.authorization_validity = read_duration(reader, "policy.authorization_validity");
  policy.default_evidence_validity = read_duration(reader, "policy.default_evidence_validity");
  return policy;
}

[[nodiscard]] PlanBinding read_binding(StateReader& reader) {
  PlanBinding binding;
  binding.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "binding.rack", RackId::parse);
  binding.composition = reader.digest("binding.composition");
  binding.stamp = read_generation_stamp(reader);
  binding.epoch = read_counter<ControlEpoch>(reader, "binding.epoch");
  return binding;
}

[[nodiscard]] RackTurnupPlan read_plan(StateReader& reader) {
  RackTurnupPlan plan;
  plan.id = read_identity<PlanId>(reader, kMaxIdentityTextBytes, "plan.id", PlanId::parse);
  plan.lifetime = read_counter<PlanLifetime>(reader, "plan.lifetime");
  plan.revision = read_counter<PlanRevision>(reader, "plan.revision");
  plan.binding = read_binding(reader);
  plan.requirements = read_requirements(reader);
  plan.policy = read_policy(reader);
  plan.state = read_enum<PlanState>(reader, "plan.state", 1, 4);
  plan.requirements_digest = reader.digest("plan.requirements_digest");
  plan.evidence_high_water = read_counter<ObservationSequence>(reader, "plan.evidence_high_water");
  plan.created_at = WallClock{reader.i64("plan.created_at")};
  plan.created_by =
      read_identity<ActorId>(reader, kMaxActorBytes, "plan.created_by", ActorId::parse);
  plan.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes, "plan.source",
                                           SourceReference::parse);
  plan.note = read_identity<Note>(reader, kMaxNoteBytes, "plan.note", parse_note_or_none);
  if (!reader.ok()) {
    return RackTurnupPlan{};
  }
  require_digest(reader, "plan.requirements_digest", plan.requirements_digest,
                 plan.compute_requirements_digest());
  return plan;
}

[[nodiscard]] RackRecord read_rack_record(StateReader& reader) {
  RackRecord record;
  record.id = read_identity<RackId>(reader, kMaxIdentityTextBytes, "rack.id", RackId::parse);
  record.label =
      read_identity<DisplayLabel>(reader, kMaxLabelBytes, "rack.label", parse_label_or_none);
  record.site = read_identity<SiteId>(reader, kMaxIdentityTextBytes, "rack.site", SiteId::parse);
  record.generation = read_counter<RackGeneration>(reader, "rack.generation");
  record.composition = read_composition(reader);
  record.lifecycle = read_enum<RackLifecycleState>(reader, "rack.lifecycle", 1, 8);
  record.registered_at = WallClock{reader.i64("rack.registered_at")};
  record.registered_by =
      read_identity<ActorId>(reader, kMaxActorBytes, "rack.registered_by", ActorId::parse);
  record.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes, "rack.source",
                                             SourceReference::parse);
  record.note = read_identity<Note>(reader, kMaxNoteBytes, "rack.note", parse_note_or_none);
  return record;
}

[[nodiscard]] EvidenceRecord read_evidence(StateReader& reader) {
  EvidenceRecord record;
  record.id =
      read_identity<EvidenceId>(reader, kMaxIdentityTextBytes, "evidence.id", EvidenceId::parse);
  record.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "evidence.rack", RackId::parse);
  record.subsystem = read_subsystem(reader, "evidence.subsystem");
  record.stamp = read_generation_stamp(reader);
  record.composition = reader.digest("evidence.composition");
  record.observed_at = WallClock{reader.i64("evidence.observed_at")};
  record.validity = read_duration(reader, "evidence.validity");
  record.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes,
                                                 "evidence.source", SourceReference::parse);
  record.producer =
      read_identity<ActorId>(reader, kMaxActorBytes, "evidence.producer", ActorId::parse);
  record.note = read_identity<Note>(reader, kMaxNoteBytes, "evidence.note", parse_note_or_none);
  record.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "evidence.plan", PlanId::parse);
  record.plan_revision = read_counter<PlanRevision>(reader, "evidence.plan_revision");
  record.sequence = read_counter<ObservationSequence>(reader, "evidence.sequence");
  record.payload = read_payload(reader);
  record.record_digest = reader.digest("evidence.record_digest");
  if (!reader.ok()) {
    return EvidenceRecord{};
  }
  require_digest(reader, "evidence.record_digest", record.record_digest, record.compute_digest());
  return record;
}

[[nodiscard]] TurnupAuthorization read_authorization(StateReader& reader) {
  TurnupAuthorization authorization;
  authorization.attempt = read_identity<AttemptId>(
      reader, kMaxIdentityTextBytes, "authorization.attempt", AttemptId::parse);
  authorization.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "authorization.rack", RackId::parse);
  authorization.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "authorization.plan", PlanId::parse);
  authorization.lifetime = read_counter<PlanLifetime>(reader, "authorization.lifetime");
  authorization.revision = read_counter<PlanRevision>(reader, "authorization.revision");
  authorization.composition = reader.digest("authorization.composition");
  authorization.stamp = read_generation_stamp(reader);
  authorization.epoch = read_counter<ControlEpoch>(reader, "authorization.epoch");
  authorization.policy = read_counter<PolicyGeneration>(reader, "authorization.policy");
  authorization.verdict = reader.digest("authorization.verdict");
  authorization.evidence_set = reader.digest("authorization.evidence_set");
  authorization.evidence_high_water =
      read_counter<ObservationSequence>(reader, "authorization.evidence_high_water");
  authorization.issued_at = WallClock{reader.i64("authorization.issued_at")};
  authorization.validity = read_duration(reader, "authorization.validity");
  authorization.actor =
      read_identity<ActorId>(reader, kMaxActorBytes, "authorization.actor", ActorId::parse);
  authorization.source = read_identity<SourceReference>(
      reader, kMaxSourceReferenceBytes, "authorization.source", SourceReference::parse);
  authorization.state = read_enum<AuthorizationState>(reader, "authorization.state", 1, 4);
  authorization.authorization_digest = reader.digest("authorization.authorization_digest");
  if (!reader.ok()) {
    return TurnupAuthorization{};
  }
  require_digest(reader, "authorization.authorization_digest", authorization.authorization_digest,
                 authorization.compute_digest());
  return authorization;
}

[[nodiscard]] AuthorizationFence read_fence(StateReader& reader) {
  AuthorizationFence fence;
  fence.attempt =
      read_identity<AttemptId>(reader, kMaxIdentityTextBytes, "fence.attempt", AttemptId::parse);
  fence.reason = read_error_code(reader, "fence.reason");
  fence.explanation = reader.text(kMaxFreeTextBytes, "fence.explanation");
  fence.sequence = read_counter<ObservationSequence>(reader, "fence.sequence");
  fence.fenced_at = WallClock{reader.i64("fence.fenced_at")};
  fence.actor = read_identity<ActorId>(reader, kMaxActorBytes, "fence.actor", ActorId::parse);
  fence.request = reader.digest("fence.request");
  return fence;
}

[[nodiscard]] ActivationRecord read_activation(StateReader& reader) {
  ActivationRecord activation;
  activation.attempt = read_identity<AttemptId>(reader, kMaxIdentityTextBytes, "activation.attempt",
                                            AttemptId::parse);
  activation.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "activation.rack", RackId::parse);
  activation.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "activation.plan", PlanId::parse);
  activation.revision = read_counter<PlanRevision>(reader, "activation.revision");
  activation.authorization = reader.digest("activation.authorization");
  activation.observed_composition = reader.digest("activation.observed_composition");
  activation.observed_state = reader.digest("activation.observed_state");
  activation.sequence = read_counter<ObservationSequence>(reader, "activation.sequence");
  activation.observed_at = WallClock{reader.i64("activation.observed_at")};
  activation.validity = read_duration(reader, "activation.validity");
  activation.outcome = read_enum<ActivationOutcome>(reader, "activation.outcome", 0, 2);
  activation.active_members = reader.u32("activation.active_members");
  activation.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes,
                                                 "activation.source", SourceReference::parse);
  activation.actor =
      read_identity<ActorId>(reader, kMaxActorBytes, "activation.actor", ActorId::parse);
  activation.note =
      read_identity<Note>(reader, kMaxNoteBytes, "activation.note", parse_note_or_none);
  activation.activation_digest = reader.digest("activation.activation_digest");
  if (!reader.ok()) {
    return ActivationRecord{};
  }
  require_digest(reader, "activation.activation_digest", activation.activation_digest,
                 activation.compute_digest());
  return activation;
}

[[nodiscard]] CommissionRecord read_commission(StateReader& reader) {
  CommissionRecord commission;
  commission.attempt = read_identity<AttemptId>(reader, kMaxIdentityTextBytes, "commission.attempt",
                                            AttemptId::parse);
  commission.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "commission.plan", PlanId::parse);
  commission.revision = read_counter<PlanRevision>(reader, "commission.revision");
  commission.activation = reader.digest("commission.activation");
  commission.commissioned_at = WallClock{reader.i64("commission.commissioned_at")};
  commission.actor =
      read_identity<ActorId>(reader, kMaxActorBytes, "commission.actor", ActorId::parse);
  commission.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes,
                                                 "commission.source", SourceReference::parse);
  commission.note =
      read_identity<Note>(reader, kMaxNoteBytes, "commission.note", parse_note_or_none);
  commission.commission_digest = reader.digest("commission.commission_digest");
  if (!reader.ok()) {
    return CommissionRecord{};
  }
  require_digest(reader, "commission.commission_digest", commission.commission_digest,
                 commission.compute_digest());
  return commission;
}

[[nodiscard]] IdempotencyRecord read_idempotency(StateReader& reader) {
  IdempotencyRecord receipt;
  receipt.request =
      read_identity<RequestId>(reader, kMaxRequestIdBytes, "idempotency.request", RequestId::parse);
  receipt.operation = read_enum<OperationKind>(reader, "idempotency.operation", 0, 12);
  receipt.request_digest = reader.digest("idempotency.request_digest");
  receipt.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "idempotency.rack", RackId::parse);
  receipt.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "idempotency.plan", PlanId::parse);
  receipt.revision_before = read_counter<PlanRevision>(reader, "idempotency.revision_before");
  receipt.revision_after = read_counter<PlanRevision>(reader, "idempotency.revision_after");
  receipt.sequence_after = read_counter<ObservationSequence>(reader, "idempotency.sequence_after");
  receipt.evidence = read_identity<EvidenceId>(reader, kMaxIdentityTextBytes,
                                               "idempotency.evidence", EvidenceId::parse);
  receipt.attempt = read_identity<AttemptId>(reader, kMaxIdentityTextBytes, "idempotency.attempt",
                                         AttemptId::parse);
  receipt.recorded_at = WallClock{reader.i64("idempotency.recorded_at")};
  receipt.actor =
      read_identity<ActorId>(reader, kMaxActorBytes, "idempotency.actor", ActorId::parse);
  receipt.response_digest = reader.digest("idempotency.response_digest");
  if (!reader.ok()) {
    return IdempotencyRecord{};
  }
  require_digest(reader, "idempotency.response_digest", receipt.response_digest,
                 receipt.compute_digest());
  return receipt;
}

[[nodiscard]] ProvenanceRecord read_provenance(StateReader& reader) {
  ProvenanceRecord record;
  record.operation = read_enum<OperationKind>(reader, "provenance.operation", 0, 12);
  record.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "provenance.rack", RackId::parse);
  record.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "provenance.plan", PlanId::parse);
  record.actor = read_identity<ActorId>(reader, kMaxActorBytes, "provenance.actor", ActorId::parse);
  record.source = read_identity<SourceReference>(reader, kMaxSourceReferenceBytes,
                                                 "provenance.source", SourceReference::parse);
  record.at = WallClock{reader.i64("provenance.at")};
  record.revision = read_counter<PlanRevision>(reader, "provenance.revision");
  record.sequence = read_counter<ObservationSequence>(reader, "provenance.sequence");
  record.request = reader.digest("provenance.request");
  record.note = read_identity<Note>(reader, kMaxNoteBytes, "provenance.note", parse_note_or_none);
  return record;
}

[[nodiscard]] RejectionRecord read_rejection(StateReader& reader) {
  RejectionRecord record;
  record.code = read_error_code(reader, "rejection.code");
  record.operation = read_enum<OperationKind>(reader, "rejection.operation", 0, 12);
  record.rack =
      read_identity<RackId>(reader, kMaxIdentityTextBytes, "rejection.rack", RackId::parse);
  record.plan =
      read_identity<PlanId>(reader, kMaxIdentityTextBytes, "rejection.plan", PlanId::parse);
  record.message = reader.text(kMaxFreeTextBytes, "rejection.message");
  record.at = WallClock{reader.i64("rejection.at")};
  record.actor = read_identity<ActorId>(reader, kMaxActorBytes, "rejection.actor", ActorId::parse);
  record.request = reader.digest("rejection.request");
  return record;
}

[[nodiscard]] RackState read_rack_state(StateReader& reader) {
  RackState rack;
  rack.rack = read_rack_record(reader);

  const std::uint32_t plans = reader.count(kMaxPlansPerRack, "rack.plans");
  if (reader.ok()) {
    rack.plans.reserve(plans);
  }
  for (std::uint32_t index = 0; index < plans && reader.ok(); ++index) {
    rack.plans.push_back(read_plan(reader));
  }

  const std::uint32_t evidence = reader.count(kMaxEvidenceRecordsPerRack, "rack.evidence");
  if (reader.ok()) {
    rack.evidence.reserve(evidence);
  }
  for (std::uint32_t index = 0; index < evidence && reader.ok(); ++index) {
    rack.evidence.push_back(read_evidence(reader));
  }

  const std::uint32_t authorizations = reader.count(kMaxFencesPerRack, "rack.authorizations");
  if (reader.ok()) {
    rack.authorizations.reserve(authorizations);
  }
  for (std::uint32_t index = 0; index < authorizations && reader.ok(); ++index) {
    rack.authorizations.push_back(read_authorization(reader));
  }

  const std::uint32_t fences = reader.count(kMaxFencesPerRack, "rack.fences");
  if (reader.ok()) {
    rack.fences.reserve(fences);
  }
  for (std::uint32_t index = 0; index < fences && reader.ok(); ++index) {
    rack.fences.push_back(read_fence(reader));
  }

  const std::uint32_t activations = reader.count(kMaxActivationRecordsPerRack, "rack.activations");
  if (reader.ok()) {
    rack.activations.reserve(activations);
  }
  for (std::uint32_t index = 0; index < activations && reader.ok(); ++index) {
    rack.activations.push_back(read_activation(reader));
  }

  // limits.hpp documents no bound of its own for commission records, so the
  // general bound on a decoded collection header applies.
  const std::uint32_t commissions = reader.count(kMaxDecodedCollectionCount, "rack.commissions");
  if (reader.ok()) {
    rack.commissions.reserve(commissions);
  }
  for (std::uint32_t index = 0; index < commissions && reader.ok(); ++index) {
    rack.commissions.push_back(read_commission(reader));
  }

  const std::uint32_t idempotency =
      reader.count(kMaxIdempotencyRecordsPerRack, "rack.idempotency");
  if (reader.ok()) {
    rack.idempotency.reserve(idempotency);
  }
  for (std::uint32_t index = 0; index < idempotency && reader.ok(); ++index) {
    rack.idempotency.push_back(read_idempotency(reader));
  }
  rack.idempotency_evictions = reader.u64("rack.idempotency_evictions");

  const std::uint32_t provenance = reader.count(kMaxProvenanceRecordsPerRack, "rack.provenance");
  if (reader.ok()) {
    rack.provenance.reserve(provenance);
  }
  for (std::uint32_t index = 0; index < provenance && reader.ok(); ++index) {
    rack.provenance.push_back(read_provenance(reader));
  }
  if (!reader.ok()) {
    return RackState{};
  }
  return rack;
}

[[nodiscard]] ServiceState read_service_state(StateReader& reader) {
  ServiceState state;
  state.store_epoch = read_counter<StoreEpoch>(reader, "state.store_epoch");
  state.store_sequence = read_counter<StoreSequence>(reader, "state.store_sequence");
  state.incarnation = read_counter<IncarnationId>(reader, "state.incarnation");
  state.control_epoch = read_counter<ControlEpoch>(reader, "state.control_epoch");
  state.observation_sequence =
      read_counter<ObservationSequence>(reader, "state.observation_sequence");
  state.created_at = WallClock{reader.i64("state.created_at")};
  state.updated_at = WallClock{reader.i64("state.updated_at")};
  state.last_actor =
      read_identity<ActorId>(reader, kMaxActorBytes, "state.last_actor", ActorId::parse);

  const std::uint32_t racks = reader.count(kMaxRacks, "state.racks");
  if (reader.ok()) {
    state.racks.reserve(racks);
  }
  for (std::uint32_t index = 0; index < racks && reader.ok(); ++index) {
    state.racks.push_back(read_rack_state(reader));
  }

  const std::uint32_t rejections = reader.count(kMaxRejectionJournalEntries, "state.rejections");
  if (reader.ok()) {
    state.rejections.reserve(rejections);
  }
  for (std::uint32_t index = 0; index < rejections && reader.ok(); ++index) {
    state.rejections.push_back(read_rejection(reader));
  }
  state.rejection_evictions = reader.u64("state.rejection_evictions");
  if (!reader.ok()) {
    return ServiceState{};
  }
  return state;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Appends the payload. The writer cannot fail on a state the in-memory API
// produced; the only failure is a count or a length that does not fit its u32
// field, which would otherwise be silently truncated.
class StateWriter final {
 public:
  explicit StateWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] const TurnupError& error() const noexcept { return error_; }

  void u8(std::uint8_t value) { out_.push_back(value); }
  void boolean(bool value) { out_.push_back(value ? std::uint8_t{1} : std::uint8_t{0}); }
  void u16(std::uint16_t value) { unsigned_value(value, 2u); }
  void u32(std::uint32_t value) { unsigned_value(value, 4u); }
  void u64(std::uint64_t value) { unsigned_value(value, 8u); }

  // A signed 64-bit field is written as its two's complement bit pattern.
  void i64(std::int64_t value) { unsigned_value(static_cast<std::uint64_t>(value), 8u); }

  void bytes(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
      return;
    }
    out_.insert(out_.end(), data, data + size);
  }

  void digest(const Digest& value) { bytes(value.data(), value.bytes().size()); }

  void text(std::string_view value) {
    if (value.size() > kMaxEncodedCount) {
      fail(ErrorCode::StateTooLarge, "a text field does not fit its encoded length");
      return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }

  void count(std::size_t value) {
    if (value > kMaxEncodedCount) {
      fail(ErrorCode::LimitExceeded, "a collection count does not fit its encoded field");
      return;
    }
    u32(static_cast<std::uint32_t>(value));
  }

 private:
  void unsigned_value(std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) {
      const unsigned shift = (width - index - 1u) * 8u;
      out_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void fail(ErrorCode code, std::string message) {
    if (!failed_) {
      failed_ = true;
      error_ = make_error(code, std::move(message), ErrorDetail{.operation = "encode_state"});
    }
  }

  std::vector<std::uint8_t>& out_;
  bool failed_ = false;
  TurnupError error_;
};

void write_generation_stamp(StateWriter& writer, const GenerationStamp& stamp) {
  writer.u64(stamp.rack.value());
  writer.u64(stamp.composition.value());
  writer.u64(stamp.topology.value());
  writer.u64(stamp.dependency.value());
  writer.u64(stamp.power.value());
  writer.u64(stamp.cooling.value());
  writer.u64(stamp.network.value());
  writer.u64(stamp.inventory.value());
  writer.u64(stamp.health.value());
  writer.u64(stamp.capacity.value());
  writer.u64(stamp.maintenance.value());
  writer.u64(stamp.policy.value());
  writer.u64(stamp.firmware.value());
}

void write_trait_set(StateWriter& writer, const TraitSet& traits) {
  writer.count(traits.size());
  for (const Trait& trait : traits.traits()) {
    writer.text(trait.text());
  }
}

void write_slot(StateWriter& writer, const SlotCoordinate& slot) {
  writer.u32(slot.unit);
  writer.u32(slot.slot);
}

void write_identity_observation(StateWriter& writer,
                                const IdentityCompositionObservation& observation) {
  writer.digest(observation.registry_composition);
  writer.u32(observation.declared_members);
  writer.u32(observation.unreadable_members);
}

void write_power_observation(StateWriter& writer, const PowerObservation& observation) {
  writer.text(observation.domain.text());
  writer.u64(observation.available_milliwatts);
  writer.u64(observation.applied_milliwatts);
  writer.u32(observation.energized_circuits);
  writer.boolean(observation.redundant_feeds_present);
  writer.boolean(observation.applied_verified);
}

void write_cooling_observation(StateWriter& writer, const CoolingObservation& observation) {
  writer.text(observation.domain.text());
  writer.u64(observation.capacity_milliwatts);
  writer.u64(observation.delivered_milliwatts);
  writer.u32(observation.inlet_millidegrees_c);
  writer.u32(observation.max_inlet_millidegrees_c);
  writer.boolean(observation.delivery_verified);
}

void write_network_observation(StateWriter& writer, const NetworkObservation& observation) {
  writer.text(observation.domain.text());
  writer.text(observation.topology.text());
  writer.u32(observation.attached_ports);
  writer.u32(observation.reachable_ports);
  writer.boolean(observation.authority_verified);
}

void write_inventory_entry(StateWriter& writer, const InventoryEntry& entry) {
  writer.text(entry.device.text());
  writer.text(entry.asset.text());
  writer.text(entry.firmware_baseline.text());
  write_trait_set(writer, entry.traits);
  write_slot(writer, entry.slot);
  writer.boolean(entry.readable);
}

void write_inventory_observation(StateWriter& writer, const InventoryObservation& observation) {
  writer.count(observation.entries.size());
  for (const InventoryEntry& entry : observation.entries) {
    write_inventory_entry(writer, entry);
  }
  writer.u32(observation.unreadable_positions);
}

void write_health_sample(StateWriter& writer, const DeviceHealthSample& sample) {
  writer.text(sample.device.text());
  writer.u8(static_cast<std::uint8_t>(sample.status));
  writer.text(sample.note.text());
}

void write_health_observation(StateWriter& writer, const HealthObservation& observation) {
  writer.count(observation.samples.size());
  for (const DeviceHealthSample& sample : observation.samples) {
    write_health_sample(writer, sample);
  }
  writer.u32(observation.unchecked_devices);
}

void write_compatibility_membership(StateWriter& writer,
                                     const CompatibilityMembership& membership) {
  writer.text(membership.device.text());
  writer.text(membership.firmware_baseline.text());
  write_trait_set(writer, membership.traits);
}

void write_compatibility_observation(StateWriter& writer,
                                     const CompatibilityObservation& observation) {
  writer.count(observation.members.size());
  for (const CompatibilityMembership& membership : observation.members) {
    write_compatibility_membership(writer, membership);
  }
}

void write_payload(StateWriter& writer, const EvidencePayload& payload) {
  writer.u8(static_cast<std::uint8_t>(payload.kind));
  write_identity_observation(writer, payload.identity);
  write_power_observation(writer, payload.power);
  write_cooling_observation(writer, payload.cooling);
  write_network_observation(writer, payload.network);
  write_inventory_observation(writer, payload.inventory);
  write_health_observation(writer, payload.health);
  write_compatibility_observation(writer, payload.compatibility);
}

void write_composition_member(StateWriter& writer, const CompositionMember& member) {
  writer.text(member.device.text());
  writer.text(member.asset.text());
  writer.text(member.firmware_baseline.text());
  write_trait_set(writer, member.traits);
  write_slot(writer, member.slot);
  writer.text(member.label.text());
}

void write_composition(StateWriter& writer, const RackComposition& composition) {
  writer.digest(composition.digest());
  writer.text(composition.rack().text());
  writer.text(composition.site().text());
  writer.u64(composition.generation().value());
  writer.count(composition.members().size());
  for (const CompositionMember& member : composition.members()) {
    write_composition_member(writer, member);
  }
}

void write_requirement(StateWriter& writer, const CompatibilityRequirement& requirement) {
  writer.u8(static_cast<std::uint8_t>(requirement.kind));
  writer.text(requirement.trait.text());
  writer.text(requirement.device.text());
  writer.text(requirement.firmware_baseline.text());
  writer.boolean(requirement.mandatory);
  writer.text(requirement.note.text());
}

void write_requirements(StateWriter& writer, const PlanRequirements& requirements) {
  writer.u64(requirements.required_power_milliwatts);
  writer.u64(requirements.required_cooling_milliwatts);
  writer.u32(requirements.required_network_ports);
  writer.boolean(requirements.require_redundant_feeds);
  writer.boolean(requirements.require_applied_power_verification);
  writer.boolean(requirements.require_cooling_delivery_verification);
  writer.boolean(requirements.require_network_authority_verification);
  writer.boolean(requirements.require_health_pass_for_every_member);
  writer.text(requirements.required_topology.text());
  writer.count(requirements.compatibility.size());
  for (const CompatibilityRequirement& requirement : requirements.compatibility) {
    write_requirement(writer, requirement);
  }
}

void write_policy(StateWriter& writer, const TurnupPolicy& policy) {
  writer.u64(policy.generation.value());
  writer.boolean(policy.allow_degraded_subsystems);
  writer.i64(policy.authorization_validity.milliseconds());
  writer.i64(policy.default_evidence_validity.milliseconds());
}

void write_binding(StateWriter& writer, const PlanBinding& binding) {
  writer.text(binding.rack.text());
  writer.digest(binding.composition);
  write_generation_stamp(writer, binding.stamp);
  writer.u64(binding.epoch.value());
}

void write_plan(StateWriter& writer, const RackTurnupPlan& plan) {
  writer.text(plan.id.text());
  writer.u64(plan.lifetime.value());
  writer.u64(plan.revision.value());
  write_binding(writer, plan.binding);
  write_requirements(writer, plan.requirements);
  write_policy(writer, plan.policy);
  writer.u8(static_cast<std::uint8_t>(plan.state));
  writer.digest(plan.requirements_digest);
  writer.u64(plan.evidence_high_water.value());
  writer.i64(plan.created_at.unix_milliseconds());
  writer.text(plan.created_by.text());
  writer.text(plan.source.text());
  writer.text(plan.note.text());
}

void write_rack_record(StateWriter& writer, const RackRecord& record) {
  writer.text(record.id.text());
  writer.text(record.label.text());
  writer.text(record.site.text());
  writer.u64(record.generation.value());
  write_composition(writer, record.composition);
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  writer.i64(record.registered_at.unix_milliseconds());
  writer.text(record.registered_by.text());
  writer.text(record.source.text());
  writer.text(record.note.text());
}

void write_evidence(StateWriter& writer, const EvidenceRecord& record) {
  writer.text(record.id.text());
  writer.text(record.rack.text());
  writer.u8(static_cast<std::uint8_t>(record.subsystem));
  write_generation_stamp(writer, record.stamp);
  writer.digest(record.composition);
  writer.i64(record.observed_at.unix_milliseconds());
  writer.i64(record.validity.milliseconds());
  writer.text(record.source.text());
  writer.text(record.producer.text());
  writer.text(record.note.text());
  writer.text(record.plan.text());
  writer.u64(record.plan_revision.value());
  writer.u64(record.sequence.value());
  write_payload(writer, record.payload);
  writer.digest(record.record_digest);
}

void write_authorization(StateWriter& writer, const TurnupAuthorization& authorization) {
  writer.text(authorization.attempt.text());
  writer.text(authorization.rack.text());
  writer.text(authorization.plan.text());
  writer.u64(authorization.lifetime.value());
  writer.u64(authorization.revision.value());
  writer.digest(authorization.composition);
  write_generation_stamp(writer, authorization.stamp);
  writer.u64(authorization.epoch.value());
  writer.u64(authorization.policy.value());
  writer.digest(authorization.verdict);
  writer.digest(authorization.evidence_set);
  writer.u64(authorization.evidence_high_water.value());
  writer.i64(authorization.issued_at.unix_milliseconds());
  writer.i64(authorization.validity.milliseconds());
  writer.text(authorization.actor.text());
  writer.text(authorization.source.text());
  writer.u8(static_cast<std::uint8_t>(authorization.state));
  writer.digest(authorization.authorization_digest);
}

void write_fence(StateWriter& writer, const AuthorizationFence& fence) {
  writer.text(fence.attempt.text());
  writer.u16(static_cast<std::uint16_t>(fence.reason));
  writer.text(fence.explanation);
  writer.u64(fence.sequence.value());
  writer.i64(fence.fenced_at.unix_milliseconds());
  writer.text(fence.actor.text());
  writer.digest(fence.request);
}

void write_activation(StateWriter& writer, const ActivationRecord& activation) {
  writer.text(activation.attempt.text());
  writer.text(activation.rack.text());
  writer.text(activation.plan.text());
  writer.u64(activation.revision.value());
  writer.digest(activation.authorization);
  writer.digest(activation.observed_composition);
  writer.digest(activation.observed_state);
  writer.u64(activation.sequence.value());
  writer.i64(activation.observed_at.unix_milliseconds());
  writer.i64(activation.validity.milliseconds());
  writer.u8(static_cast<std::uint8_t>(activation.outcome));
  writer.u32(activation.active_members);
  writer.text(activation.source.text());
  writer.text(activation.actor.text());
  writer.text(activation.note.text());
  writer.digest(activation.activation_digest);
}

void write_commission(StateWriter& writer, const CommissionRecord& commission) {
  writer.text(commission.attempt.text());
  writer.text(commission.plan.text());
  writer.u64(commission.revision.value());
  writer.digest(commission.activation);
  writer.i64(commission.commissioned_at.unix_milliseconds());
  writer.text(commission.actor.text());
  writer.text(commission.source.text());
  writer.text(commission.note.text());
  writer.digest(commission.commission_digest);
}

void write_idempotency(StateWriter& writer, const IdempotencyRecord& receipt) {
  writer.text(receipt.request.text());
  writer.u8(static_cast<std::uint8_t>(receipt.operation));
  writer.digest(receipt.request_digest);
  writer.text(receipt.rack.text());
  writer.text(receipt.plan.text());
  writer.u64(receipt.revision_before.value());
  writer.u64(receipt.revision_after.value());
  writer.u64(receipt.sequence_after.value());
  writer.text(receipt.evidence.text());
  writer.text(receipt.attempt.text());
  writer.i64(receipt.recorded_at.unix_milliseconds());
  writer.text(receipt.actor.text());
  writer.digest(receipt.response_digest);
}

void write_provenance(StateWriter& writer, const ProvenanceRecord& record) {
  writer.u8(static_cast<std::uint8_t>(record.operation));
  writer.text(record.rack.text());
  writer.text(record.plan.text());
  writer.text(record.actor.text());
  writer.text(record.source.text());
  writer.i64(record.at.unix_milliseconds());
  writer.u64(record.revision.value());
  writer.u64(record.sequence.value());
  writer.digest(record.request);
  writer.text(record.note.text());
}

void write_rejection(StateWriter& writer, const RejectionRecord& record) {
  writer.u16(static_cast<std::uint16_t>(record.code));
  writer.u8(static_cast<std::uint8_t>(record.operation));
  writer.text(record.rack.text());
  writer.text(record.plan.text());
  writer.text(record.message);
  writer.i64(record.at.unix_milliseconds());
  writer.text(record.actor.text());
  writer.digest(record.request);
}

void write_rack_state(StateWriter& writer, const RackState& rack) {
  write_rack_record(writer, rack.rack);

  writer.count(rack.plans.size());
  for (const RackTurnupPlan& plan : rack.plans) {
    write_plan(writer, plan);
  }

  writer.count(rack.evidence.size());
  for (const EvidenceRecord& record : rack.evidence) {
    write_evidence(writer, record);
  }

  writer.count(rack.authorizations.size());
  for (const TurnupAuthorization& authorization : rack.authorizations) {
    write_authorization(writer, authorization);
  }

  writer.count(rack.fences.size());
  for (const AuthorizationFence& fence : rack.fences) {
    write_fence(writer, fence);
  }

  writer.count(rack.activations.size());
  for (const ActivationRecord& activation : rack.activations) {
    write_activation(writer, activation);
  }

  writer.count(rack.commissions.size());
  for (const CommissionRecord& commission : rack.commissions) {
    write_commission(writer, commission);
  }

  writer.count(rack.idempotency.size());
  for (const IdempotencyRecord& receipt : rack.idempotency) {
    write_idempotency(writer, receipt);
  }
  writer.u64(rack.idempotency_evictions);

  writer.count(rack.provenance.size());
  for (const ProvenanceRecord& record : rack.provenance) {
    write_provenance(writer, record);
  }
}

void write_service_state(StateWriter& writer, const ServiceState& state) {
  writer.u32(kStateFormatVersion);
  writer.u32(kReservedWord);
  writer.u64(state.store_epoch.value());
  writer.u64(state.store_sequence.value());
  writer.u64(state.incarnation.value());
  writer.u64(state.control_epoch.value());
  writer.u64(state.observation_sequence.value());
  writer.i64(state.created_at.unix_milliseconds());
  writer.i64(state.updated_at.unix_milliseconds());
  writer.text(state.last_actor.text());

  writer.count(state.racks.size());
  for (const RackState& rack : state.racks) {
    write_rack_state(writer, rack);
  }

  writer.count(state.rejections.size());
  for (const RejectionRecord& rejection : state.rejections) {
    write_rejection(writer, rejection);
  }
  writer.u64(state.rejection_evictions);
}

}  // namespace

Result<std::vector<std::uint8_t>> encode_state(const ServiceState& state) {
  std::vector<std::uint8_t> payload;
  StateWriter writer(payload);
  write_service_state(writer, state);
  if (!writer.ok()) {
    return writer.error();
  }
  return std::move(payload);
}

Result<ServiceState> decode_state(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    if (size == 0) {
      return make_error(ErrorCode::TruncatedState, "the payload is empty",
                        ErrorDetail{.operation = "decode_state"});
    }
    return make_error(ErrorCode::InvalidArgument, "the payload pointer is null",
                      ErrorDetail{.operation = "decode_state", .actual = size});
  }

  StateReader reader(data, size);
  const std::uint32_t model = reader.u32("payload.model");
  if (!reader.ok()) {
    return reader.error();
  }
  if (model != kStateFormatVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the payload declares a format version this build cannot read",
                      ErrorDetail{.operation = "decode_state", .expected = kStateFormatVersion,
                                  .actual = model});
  }
  const std::uint32_t reserved = reader.u32("payload.reserved");
  if (!reader.ok()) {
    return reader.error();
  }
  if (reserved != kReservedWord) {
    return make_error(ErrorCode::ReservedBitsSet, "a reserved field is not zero",
                      ErrorDetail{.operation = "decode_state", .subject = "payload.reserved",
                                  .expected = kReservedWord, .actual = reserved});
  }

  ServiceState state = read_service_state(reader);
  if (!reader.ok()) {
    return reader.error();
  }
  if (!reader.at_end()) {
    return make_error(ErrorCode::TrailingBytes,
                      "bytes remain after the declared end of the payload",
                      ErrorDetail{.operation = "decode_state", .actual = reader.remaining()});
  }

  const Status valid = validate_state(state);
  if (!valid.has_value()) {
    return valid.error();
  }
  return std::move(state);
}

Result<ServiceState> decode_state(const std::vector<std::uint8_t>& data) {
  return decode_state(data.data(), data.size());
}

}  // namespace rackturnup
