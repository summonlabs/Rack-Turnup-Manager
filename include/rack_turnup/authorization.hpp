// Rack Turnup Manager - turnup authority, activation and fencing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// A bounded, self-describing grant to turn one rack up. It binds to the exact
// rack composition, every generation, the control epoch, the plan revision and
// the digest of the verdict it was issued against. Any movement in one of those
// values makes the grant unusable, and the fence below records that fact.
struct TurnupAuthorization {
  AttemptId attempt;
  RackId rack;
  PlanId plan;
  PlanLifetime lifetime;
  PlanRevision revision;
  Digest composition;
  GenerationStamp stamp;
  ControlEpoch epoch;
  PolicyGeneration policy;
  // Digest of the evaluation this grant was issued against.
  Digest verdict;
  // Digest of the evidence set that the evaluation used.
  Digest evidence_set;
  ObservationSequence evidence_high_water;
  WallClock issued_at;
  Millis validity;
  ActorId actor;
  SourceReference source;
  AuthorizationState state = AuthorizationState::Active;
  Digest authorization_digest;

  [[nodiscard]] bool is_active() const noexcept { return state == AuthorizationState::Active; }
  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest compute_digest() const;
};

// Why an authorization stopped being usable. A fence is appended, never
// removed, so the history of authority is monotone.
struct AuthorizationFence {
  AttemptId attempt;
  ErrorCode reason = ErrorCode::Ok;
  std::string explanation;
  ObservationSequence sequence;
  WallClock fenced_at;
  ActorId actor;
  Digest request;

  void update_digest(Sha256& hasher) const noexcept;
};

// A post-action observation of the rack's active state. It is the only thing
// that can move a rack past the authorization stage, and it is accepted only
// when it was observed after the authorization was issued and at a strictly
// newer observation sequence.
struct ActivationRecord {
  AttemptId attempt;
  RackId rack;
  PlanId plan;
  PlanRevision revision;
  Digest authorization;
  Digest observed_composition;
  Digest observed_state;
  ObservationSequence sequence;
  WallClock observed_at;
  Millis validity;
  ActivationOutcome outcome = ActivationOutcome::Unknown;
  std::uint32_t active_members = 0;
  SourceReference source;
  ActorId actor;
  Note note;
  Digest activation_digest;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest compute_digest() const;
};

// The record that a rack is commissioned. It names the activation observation
// it accepted, so commissioning can never be traced to an acknowledgement.
struct CommissionRecord {
  AttemptId attempt;
  PlanId plan;
  PlanRevision revision;
  Digest activation;
  WallClock commissioned_at;
  ActorId actor;
  SourceReference source;
  Note note;
  Digest commission_digest;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest compute_digest() const;
};

// Receipt of one mutation, kept so that a lost response can be replayed
// without applying the mutation twice. The request digest is what makes a
// replay distinguishable from a conflicting reuse of the same request id.
struct IdempotencyRecord {
  RequestId request;
  OperationKind operation = OperationKind::Unknown;
  Digest request_digest;
  RackId rack;
  PlanId plan;
  PlanRevision revision_before;
  PlanRevision revision_after;
  ObservationSequence sequence_after;
  EvidenceId evidence;
  AttemptId attempt;
  WallClock recorded_at;
  ActorId actor;
  Digest response_digest;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] Digest compute_digest() const;
};

// Append-only audit trail of accepted mutations for one rack.
struct ProvenanceRecord {
  OperationKind operation = OperationKind::Unknown;
  RackId rack;
  PlanId plan;
  ActorId actor;
  SourceReference source;
  WallClock at;
  PlanRevision revision;
  ObservationSequence sequence;
  Digest request;
  Note note;

  void update_digest(Sha256& hasher) const noexcept;
};

// One rejected mutation, retained so an operator can see what the service
// refused and why without reading a log.
struct RejectionRecord {
  ErrorCode code = ErrorCode::Ok;
  OperationKind operation = OperationKind::Unknown;
  RackId rack;
  PlanId plan;
  std::string message;
  WallClock at;
  ActorId actor;
  Digest request;

  void update_digest(Sha256& hasher) const noexcept;
};

}  // namespace rackturnup
