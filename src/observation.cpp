// Rack Turnup Manager - per-subsystem observation payloads.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/observation.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "rack_turnup/limits.hpp"
#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

// Version of the payload preimage layout, kept inside the payload so that a
// payload can be hashed on its own.
constexpr std::uint32_t kPayloadModel = 1;

}  // namespace

std::string_view health_status_name(HealthStatus status) noexcept {
  switch (status) {
    case HealthStatus::Unknown:
      return "unknown";
    case HealthStatus::Passing:
      return "passing";
    case HealthStatus::Warning:
      return "warning";
    case HealthStatus::Failing:
      return "failing";
  }
  return "unknown";
}

Result<HealthStatus> health_status_from_name(std::string_view name) {
  const HealthStatus statuses[] = {HealthStatus::Unknown, HealthStatus::Passing,
                                   HealthStatus::Warning, HealthStatus::Failing};
  for (const HealthStatus status : statuses) {
    if (health_status_name(status) == name) {
      return status;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown health status name",
                    ErrorDetail{.operation = "health_status_from_name",
                                .subject = std::string(name)});
}

void IdentityCompositionObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  const std::vector<std::uint8_t>& bytes = registry_composition.bytes();
  hasher.update(bytes.data(), bytes.size());
  hasher.update_u32(declared_members);
  hasher.update_u32(unreadable_members);
}

void PowerObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_len_text(domain.text());
  hasher.update_u64(available_milliwatts);
  hasher.update_u64(applied_milliwatts);
  hasher.update_u32(energized_circuits);
  hasher.update_byte(redundant_feeds_present ? 1u : 0u);
  hasher.update_byte(applied_verified ? 1u : 0u);
}

void CoolingObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_len_text(domain.text());
  hasher.update_u64(capacity_milliwatts);
  hasher.update_u64(delivered_milliwatts);
  hasher.update_u32(inlet_millidegrees_c);
  hasher.update_u32(max_inlet_millidegrees_c);
  hasher.update_byte(delivery_verified ? 1u : 0u);
}

void NetworkObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_len_text(domain.text());
  hasher.update_len_text(topology.text());
  hasher.update_u32(attached_ports);
  hasher.update_u32(reachable_ports);
  hasher.update_byte(authority_verified ? 1u : 0u);
}

void DeviceHealthSample::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_len_text(device.text());
  hasher.update_byte(static_cast<std::uint8_t>(status));
  hasher.update_len_text(note.text());
}

void HealthObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_u32(static_cast<std::uint32_t>(samples.size()));
  for (const DeviceHealthSample& sample : samples) {
    sample.update_digest(hasher);
  }
  hasher.update_u32(unchecked_devices);
}

void CompatibilityMembership::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_len_text(device.text());
  hasher.update_len_text(firmware_baseline.text());
  traits.update_digest(hasher);
}

void CompatibilityObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPayloadModel);
  hasher.update_u32(static_cast<std::uint32_t>(members.size()));
  for (const CompatibilityMembership& membership : members) {
    membership.update_digest(hasher);
  }
}

Result<HealthObservation> create_health_observation(std::vector<DeviceHealthSample> samples,
                                                    std::uint32_t unchecked_devices) {
  if (samples.size() > kMaxObservationEntries) {
    return make_error(ErrorCode::LimitExceeded, "health observation exceeds the documented bound",
                      ErrorDetail{.operation = "create_health_observation",
                                  .expected = kMaxObservationEntries,
                                  .actual = samples.size()});
  }
  std::sort(samples.begin(), samples.end(),
            [](const DeviceHealthSample& left, const DeviceHealthSample& right) {
              return byte_less(left.device.text(), right.device.text());
            });
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (samples[i].device.empty()) {
      return make_error(ErrorCode::EmptyValue, "a health sample must name its device",
                        ErrorDetail{.operation = "create_health_observation"});
    }
    if (i > 0 && samples[i - 1].device == samples[i].device) {
      return make_error(ErrorCode::DuplicateDeviceId,
                        "a health observation reports the same device twice",
                        ErrorDetail{.operation = "create_health_observation",
                                    .subject = samples[i].device.text()});
    }
  }
  HealthObservation observation;
  observation.samples = std::move(samples);
  observation.unchecked_devices = unchecked_devices;
  return observation;
}

Result<CompatibilityObservation> create_compatibility_observation(
    std::vector<CompatibilityMembership> members) {
  if (members.size() > kMaxObservationEntries) {
    return make_error(ErrorCode::LimitExceeded,
                      "compatibility observation exceeds the documented bound",
                      ErrorDetail{.operation = "create_compatibility_observation",
                                  .expected = kMaxObservationEntries,
                                  .actual = members.size()});
  }
  std::sort(members.begin(), members.end(),
            [](const CompatibilityMembership& left, const CompatibilityMembership& right) {
              return byte_less(left.device.text(), right.device.text());
            });
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].device.empty()) {
      return make_error(ErrorCode::EmptyValue,
                        "a compatibility membership must name its device",
                        ErrorDetail{.operation = "create_compatibility_observation"});
    }
    if (i > 0 && members[i - 1].device == members[i].device) {
      return make_error(ErrorCode::DuplicateDeviceId,
                        "a compatibility observation reports the same device twice",
                        ErrorDetail{.operation = "create_compatibility_observation",
                                    .subject = members[i].device.text()});
    }
  }
  CompatibilityObservation observation;
  observation.members = std::move(members);
  return observation;
}

std::string_view evidence_payload_kind_name(EvidencePayloadKind kind) noexcept {
  switch (kind) {
    case EvidencePayloadKind::IdentityComposition:
      return "identity_composition";
    case EvidencePayloadKind::Power:
      return "power";
    case EvidencePayloadKind::Cooling:
      return "cooling";
    case EvidencePayloadKind::Network:
      return "network";
    case EvidencePayloadKind::Inventory:
      return "inventory";
    case EvidencePayloadKind::Health:
      return "health";
    case EvidencePayloadKind::Compatibility:
      return "compatibility";
  }
  return "unknown";
}

Result<EvidencePayloadKind> evidence_payload_kind_from_name(std::string_view name) {
  for (std::uint8_t value = 1; value <= static_cast<std::uint8_t>(kSubsystemKindCount); ++value) {
    const auto kind = static_cast<EvidencePayloadKind>(value);
    if (evidence_payload_kind_name(kind) == name) {
      return kind;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown evidence payload kind name",
                    ErrorDetail{.operation = "evidence_payload_kind_from_name",
                                .subject = std::string(name)});
}

SubsystemKind subsystem_for_payload_kind(EvidencePayloadKind kind) noexcept {
  const auto value = static_cast<std::uint8_t>(kind);
  if (value >= 1 && value <= static_cast<std::uint8_t>(kSubsystemKindCount)) {
    return static_cast<SubsystemKind>(value);
  }
  return SubsystemKind::IdentityComposition;
}

EvidencePayloadKind payload_kind_for_subsystem(SubsystemKind kind) noexcept {
  return static_cast<EvidencePayloadKind>(static_cast<std::uint8_t>(kind));
}

void EvidencePayload::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(static_cast<std::uint32_t>(kind));
  switch (kind) {
    case EvidencePayloadKind::IdentityComposition:
      identity.update_digest(hasher);
      break;
    case EvidencePayloadKind::Power:
      power.update_digest(hasher);
      break;
    case EvidencePayloadKind::Cooling:
      cooling.update_digest(hasher);
      break;
    case EvidencePayloadKind::Network:
      network.update_digest(hasher);
      break;
    case EvidencePayloadKind::Inventory:
      inventory.update_digest(hasher);
      break;
    case EvidencePayloadKind::Health:
      health.update_digest(hasher);
      break;
    case EvidencePayloadKind::Compatibility:
      compatibility.update_digest(hasher);
      break;
  }
}

bool EvidencePayload::is_consistent() const noexcept {
  std::uint32_t present = 0;
  if (!identity.is_empty()) {
    present |= 1u << 0;
  }
  if (!power.is_empty()) {
    present |= 1u << 1;
  }
  if (!cooling.is_empty()) {
    present |= 1u << 2;
  }
  if (!network.is_empty()) {
    present |= 1u << 3;
  }
  if (!inventory.is_empty()) {
    present |= 1u << 4;
  }
  if (!health.is_empty()) {
    present |= 1u << 5;
  }
  if (!compatibility.is_empty()) {
    present |= 1u << 6;
  }
  const auto value = static_cast<std::uint32_t>(kind);
  if (value < 1 || value > static_cast<std::uint32_t>(kSubsystemKindCount)) {
    return false;
  }
  // Exactly one payload must be present, and it must be the one the tag names.
  return present == (1u << (value - 1u));
}

}  // namespace rackturnup
