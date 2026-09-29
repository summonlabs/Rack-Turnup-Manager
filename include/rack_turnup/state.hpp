// Rack Turnup Manager - the durable projection of the service.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rack_turnup/authorization.hpp"
#include "rack_turnup/composition.hpp"
#include "rack_turnup/digest.hpp"
#include "rack_turnup/evidence.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// The rack record: identity, composition and lifecycle position. Everything
// else about a rack hangs off this record.
struct RackRecord {
  RackId id;
  DisplayLabel label;
  SiteId site;
  // Advances on every accepted mutation of the rack record, so a request that
  // was planned against an older record is detected rather than applied.
  RackGeneration generation;
  RackComposition composition;
  RackLifecycleState lifecycle = RackLifecycleState::Registered;
  WallClock registered_at;
  ActorId registered_by;
  SourceReference source;
  Note note;

  [[nodiscard]] CompositionGeneration composition_generation() const noexcept {
    return composition.generation();
  }
  [[nodiscard]] const Digest& composition_digest() const noexcept { return composition.digest(); }
  void update_digest(Sha256& hasher) const noexcept;
};

// Everything the service knows about one rack.
struct RackState {
  RackRecord rack;
  // Ordered by plan lifetime; the newest plan is last.
  std::vector<RackTurnupPlan> plans;
  // Ordered by observation sequence.
  std::vector<EvidenceRecord> evidence;
  std::vector<TurnupAuthorization> authorizations;
  std::vector<AuthorizationFence> fences;
  std::vector<ActivationRecord> activations;
  std::vector<CommissionRecord> commissions;
  std::vector<IdempotencyRecord> idempotency;
  std::uint64_t idempotency_evictions = 0;
  std::vector<ProvenanceRecord> provenance;

  [[nodiscard]] const RackTurnupPlan* active_plan() const noexcept;
  [[nodiscard]] RackTurnupPlan* active_plan() noexcept;
  // The plan that currently describes the rack: the active plan when there is
  // one, otherwise the newest plan it ever had. A commissioned rack keeps its
  // completed plan as its current description, so its summary and its lifecycle
  // operations still have a plan to work with.
  [[nodiscard]] const RackTurnupPlan* current_plan() const noexcept;
  [[nodiscard]] RackTurnupPlan* current_plan() noexcept;
  [[nodiscard]] const RackTurnupPlan* find_plan(const PlanId& id) const noexcept;
  [[nodiscard]] RackTurnupPlan* find_plan(const PlanId& id) noexcept;
  [[nodiscard]] const TurnupAuthorization* find_authorization(const AttemptId& attempt) const noexcept;
  [[nodiscard]] TurnupAuthorization* find_authorization(const AttemptId& attempt) noexcept;
  [[nodiscard]] const EvidenceRecord* find_evidence(const EvidenceId& id) const noexcept;
  [[nodiscard]] bool has_evidence(const EvidenceId& id) const noexcept;
  [[nodiscard]] const IdempotencyRecord* find_receipt(const RequestId& id) const noexcept;
};

// The whole durable state of one service.
struct ServiceState {
  StoreEpoch store_epoch;
  StoreSequence store_sequence;
  IncarnationId incarnation;
  ControlEpoch control_epoch;
  ObservationSequence observation_sequence;
  WallClock created_at;
  WallClock updated_at;
  ActorId last_actor;
  std::vector<RackState> racks;
  std::vector<RejectionRecord> rejections;
  std::uint64_t rejection_evictions = 0;

  [[nodiscard]] const RackState* find_rack(const RackId& id) const noexcept;
  [[nodiscard]] RackState* find_rack(const RackId& id) noexcept;
};

// Encodes the state into its canonical durable payload. The encoding is
// deterministic: two equal states always produce identical bytes.
[[nodiscard]] RACK_TURNUP_API Result<std::vector<std::uint8_t>> encode_state(
    const ServiceState& state);

// Decodes a payload that was already checked for length, checksum and digest.
// Every field is validated again here: bounds, enumerations, identities and
// impossible combinations are rejected rather than repaired.
[[nodiscard]] RACK_TURNUP_API Result<ServiceState> decode_state(const std::uint8_t* data,
                                                                 std::size_t size);
[[nodiscard]] RACK_TURNUP_API Result<ServiceState> decode_state(
    const std::vector<std::uint8_t>& data);

// Structural validation of a state, applied to freshly built and decoded states
// alike. It reports a broken invariant as an error instead of trusting it.
[[nodiscard]] RACK_TURNUP_API Status validate_state(const ServiceState& state);

// Digest over the durable content of a state, used to compare a commit with
// the bytes that were read back.
[[nodiscard]] RACK_TURNUP_API Digest compute_state_digest(const ServiceState& state);

}  // namespace rackturnup
