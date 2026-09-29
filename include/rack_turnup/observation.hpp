// Rack Turnup Manager - per-subsystem observations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "rack_turnup/composition.hpp"
#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/result.hpp"

namespace rackturnup {

// Every structure below is an observation, not an authority. It records what
// one owner reported about the world at one time, and this library decides what
// that observation proves. A producer claim such as "applied" or "verified"
// is only ever one input to that decision.

// ---------------------------------------------------------------------------
// Identity and composition
// ---------------------------------------------------------------------------

struct IdentityCompositionObservation {
  // The composition digest the rack registry currently reports.
  Digest registry_composition;
  std::uint32_t declared_members = 0;
  std::uint32_t unreadable_members = 0;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return registry_composition.is_zero() && declared_members == 0 && unreadable_members == 0;
  }
};

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

// Power that exists is not power that reaches the rack, so available and
// applied power are recorded separately, and `applied_verified` distinguishes a
// measurement from a command acknowledgement.
struct PowerObservation {
  PowerDomainReference domain;
  std::uint64_t available_milliwatts = 0;
  std::uint64_t applied_milliwatts = 0;
  std::uint32_t energized_circuits = 0;
  bool redundant_feeds_present = false;
  bool applied_verified = false;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return domain.empty() && available_milliwatts == 0 && applied_milliwatts == 0 &&
           energized_circuits == 0 && !redundant_feeds_present && !applied_verified;
  }
};

// ---------------------------------------------------------------------------
// Cooling
// ---------------------------------------------------------------------------

// Cooling capacity that exists is not cooling delivery that is proven, so
// capacity and delivered cooling are recorded separately.
struct CoolingObservation {
  CoolingDomainReference domain;
  std::uint64_t capacity_milliwatts = 0;
  std::uint64_t delivered_milliwatts = 0;
  std::uint32_t inlet_millidegrees_c = 0;
  std::uint32_t max_inlet_millidegrees_c = 0;
  bool delivery_verified = false;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return domain.empty() && capacity_milliwatts == 0 && delivered_milliwatts == 0 &&
           inlet_millidegrees_c == 0 && max_inlet_millidegrees_c == 0 && !delivery_verified;
  }
};

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

// Attachment is not reachability and reachability is not authority, so the
// three are separate fields: ports attached, ports proven reachable, and
// whether an authority outside this library verified the attachment.
struct NetworkObservation {
  NetworkDomainReference domain;
  FabricTopologyReference topology;
  std::uint32_t attached_ports = 0;
  std::uint32_t reachable_ports = 0;
  bool authority_verified = false;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return domain.empty() && topology.empty() && attached_ports == 0 && reachable_ports == 0 &&
           !authority_verified;
  }
};

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

enum class HealthStatus : std::uint8_t {
  Unknown = 0,
  Passing = 1,
  Warning = 2,
  Failing = 3,
};

[[nodiscard]] RACK_TURNUP_API std::string_view health_status_name(HealthStatus status) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<HealthStatus> health_status_from_name(std::string_view name);

struct DeviceHealthSample {
  DeviceId device;
  HealthStatus status = HealthStatus::Unknown;
  Note note;

  void update_digest(Sha256& hasher) const noexcept;
};

struct HealthObservation {
  // Sorted by device identity and free of duplicates.
  std::vector<DeviceHealthSample> samples;
  // Positions that were inspected but produced no health answer.
  std::uint32_t unchecked_devices = 0;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return samples.empty() && unchecked_devices == 0;
  }
};

[[nodiscard]] RACK_TURNUP_API Result<HealthObservation> create_health_observation(
    std::vector<DeviceHealthSample> samples, std::uint32_t unchecked_devices);

// ---------------------------------------------------------------------------
// Compatibility
// ---------------------------------------------------------------------------

// What each member actually is: its firmware baseline and the traits it
// declares. Compatibility requirements are evaluated against this, never
// against what the composition intended.
struct CompatibilityMembership {
  DeviceId device;
  FirmwareBaselineId firmware_baseline;
  TraitSet traits;

  void update_digest(Sha256& hasher) const noexcept;
};

struct CompatibilityObservation {
  std::vector<CompatibilityMembership> members;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept { return members.empty(); }
};

[[nodiscard]] RACK_TURNUP_API Result<CompatibilityObservation> create_compatibility_observation(
    std::vector<CompatibilityMembership> members);

// ---------------------------------------------------------------------------
// Tagged payload
// ---------------------------------------------------------------------------

enum class EvidencePayloadKind : std::uint8_t {
  IdentityComposition = 1,
  Power = 2,
  Cooling = 3,
  Network = 4,
  Inventory = 5,
  Health = 6,
  Compatibility = 7,
};

[[nodiscard]] RACK_TURNUP_API std::string_view evidence_payload_kind_name(
    EvidencePayloadKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<EvidencePayloadKind> evidence_payload_kind_from_name(
    std::string_view name);
[[nodiscard]] RACK_TURNUP_API SubsystemKind subsystem_for_payload_kind(
    EvidencePayloadKind kind) noexcept;
[[nodiscard]] RACK_TURNUP_API EvidencePayloadKind payload_kind_for_subsystem(
    SubsystemKind kind) noexcept;

// Exactly one payload field is meaningful, selected by `kind`. The decoder
// rejects a record whose other payload fields are not empty, so an impossible
// combination can never reach an evaluation.
struct EvidencePayload {
  EvidencePayloadKind kind = EvidencePayloadKind::IdentityComposition;
  IdentityCompositionObservation identity;
  PowerObservation power;
  CoolingObservation cooling;
  NetworkObservation network;
  InventoryObservation inventory;
  HealthObservation health;
  CompatibilityObservation compatibility;

  void update_digest(Sha256& hasher) const noexcept;

  // True when every field that does not match `kind` is empty.
  [[nodiscard]] bool is_consistent() const noexcept;
};

}  // namespace rackturnup
