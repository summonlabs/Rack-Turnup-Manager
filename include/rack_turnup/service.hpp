// Rack Turnup Manager - the turnup service.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rack_turnup/authorization.hpp"
#include "rack_turnup/composition.hpp"
#include "rack_turnup/evaluation.hpp"
#include "rack_turnup/evidence.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/observation.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/state.hpp"
#include "rack_turnup/store.hpp"
#include "rack_turnup/time.hpp"

namespace rackturnup {

// Fields every mutation carries. The authority time is supplied by the caller
// so that a decision is reproducible: nothing in this library reads the host
// clock on its own.
struct MutationContext {
  RequestId request;
  ActorId actor;
  SourceReference source;
  Note note;
  WallClock authority_time;
};

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

struct RegisterRackRequest {
  MutationContext context;
  RackId rack;
  DisplayLabel label;
  SiteId site;
  std::vector<CompositionMember> members;
};

struct UpdateCompositionRequest {
  MutationContext context;
  RackId rack;
  RackGeneration expected_rack_generation;
  CompositionGeneration composition_generation;
  std::vector<CompositionMember> members;
  // A rack that is authorized, active or commissioned is only re-composed when
  // the caller says so explicitly; the previous authority is then fenced.
  bool replace_commissioned = false;
};

struct CreatePlanRequest {
  MutationContext context;
  RackId rack;
  RackGeneration expected_rack_generation;
  GenerationStamp stamp;
  PlanRequirements requirements;
  TurnupPolicy policy;
  // A superseded plan with a live authorization is only replaced when the
  // caller says so explicitly.
  bool cancel_active_authorization = false;
  bool replace_commissioned = false;
};

struct ImportEvidenceRequest {
  MutationContext context;
  RackId rack;
  PlanRevision expected_revision;
  SubsystemKind subsystem = SubsystemKind::Power;
  EvidencePayload payload;
  WallClock observed_at;
  Millis validity;
  // When true, the plan policy default validity is used instead of `validity`.
  bool validity_from_policy = false;
  // The generation stamp the observer reports for the world it looked at. When
  // `stamp_from_plan` is true (the default, and what the command line tool
  // does) the plan binding is stamped onto the record. A caller that supplies
  // its own stamp gets an immediate EvidenceBindingMismatch instead of a record
  // that can never contribute to a verdict.
  GenerationStamp stamp;
  bool stamp_from_plan = true;
};

struct AuthorizeRequest {
  MutationContext context;
  RackId rack;
  PlanRevision expected_revision;
  Millis validity;
  bool validity_from_policy = false;
};

struct RecordActivationRequest {
  MutationContext context;
  RackId rack;
  AttemptId attempt;
  PlanRevision expected_revision;
  ActivationOutcome outcome = ActivationOutcome::Unknown;
  // A zero digest means "the composition the plan is bound to".
  Digest observed_composition;
  std::uint32_t active_members = 0;
  WallClock observed_at;
  Millis validity;
  bool validity_from_policy = false;
};

struct CommissionRequest {
  MutationContext context;
  RackId rack;
  AttemptId attempt;
  PlanRevision expected_revision;
};

struct RollbackRequest {
  MutationContext context;
  RackId rack;
  PlanRevision expected_revision;
  Note reason;
  // Rolling back a rack whose activation was observed is allowed only when the
  // caller acknowledges that the observation happened.
  bool acknowledge_active = false;
};

struct LifecycleRequest {
  MutationContext context;
  RackId rack;
  PlanRevision expected_revision;
  Note note;
};

struct TakeControlRequest {
  MutationContext context;
};

// ---------------------------------------------------------------------------
// Receipts
// ---------------------------------------------------------------------------

struct RackSummary {
  RackId id;
  DisplayLabel label;
  SiteId site;
  RackGeneration generation;
  CompositionGeneration composition_generation;
  Digest composition;
  RackLifecycleState lifecycle = RackLifecycleState::Registered;
  std::size_t member_count = 0;
  bool has_plan = false;
  PlanId plan;
  PlanLifetime lifetime;
  PlanRevision revision;
  PlanState plan_state = PlanState::Active;
  TurnupVerdict verdict = TurnupVerdict::NotReady;
  std::size_t blocker_count = 0;
  std::size_t eligible_evidence = 0;
  WallClock registered_at;
  WallClock authority_time;
};

struct EvidenceReceipt {
  EvidenceId id;
  ObservationSequence sequence;
  PlanRevision revision;
  bool replayed = false;
};

struct AuthorizationReceipt {
  AttemptId attempt;
  PlanId plan;
  PlanRevision revision;
  Digest verdict;
  Digest evidence_set;
  ObservationSequence evidence_high_water;
  WallClock issued_at;
  WallClock expires_at;
  bool replayed = false;
};

struct ActivationReceipt {
  AttemptId attempt;
  PlanId plan;
  PlanRevision revision;
  ObservationSequence sequence;
  ActivationOutcome outcome = ActivationOutcome::Unknown;
  bool replayed = false;
};

struct CommissionReceipt {
  AttemptId attempt;
  PlanId plan;
  PlanRevision revision;
  WallClock commissioned_at;
  bool replayed = false;
};

struct RollbackReceipt {
  PlanRevision revision;
  TurnupVerdict verdict = TurnupVerdict::NotReady;
  std::size_t authorizations_fenced = 0;
  bool replayed = false;
};

struct LifecycleReceipt {
  RackLifecycleState from = RackLifecycleState::Registered;
  RackLifecycleState to = RackLifecycleState::Registered;
  PlanRevision revision;
  bool replayed = false;
};

struct RecoveryReport {
  std::size_t racks = 0;
  std::size_t fences_added = 0;
  std::size_t plans_superseded = 0;
  StoreEpoch store_epoch;
  StoreSequence store_sequence;
  IncarnationId incarnation;
  ControlEpoch control_epoch;
  ObservationSequence observation_sequence;
  bool published = false;
  std::vector<std::string> notes;
};

struct TakeoverReport {
  ControlEpoch previous;
  ControlEpoch current;
  std::size_t authorizations_fenced = 0;
};

struct ServiceSummary {
  std::size_t rack_count = 0;
  std::size_t registered = 0;
  std::size_t planned = 0;
  std::size_t authorized = 0;
  std::size_t active = 0;
  std::size_t commissioned = 0;
  std::size_t draining = 0;
  std::size_t drained = 0;
  std::size_t decommissioned = 0;
  std::size_t blocked = 0;
  StoreEpoch store_epoch;
  StoreSequence store_sequence;
  IncarnationId incarnation;
  ControlEpoch control_epoch;
  ObservationSequence observation_sequence;
  WallClock created_at;
  WallClock updated_at;
  WallClock authority_time;
  std::size_t rejection_journal = 0;
};

// ---------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------

// One service owns one facility scope and one durable state file.
//
// Concurrency model: a service instance is single-threaded and holds no locks
// other than the store's writer lock. It never calls back into user code while
// holding state, spawns no threads and has no internal queues, so there is no
// lock ordering to invert and no completion that can arrive late. Cross-process
// exclusion is the operating-system lock on the store's lock file, taken in
// fail-fast mode; a second writer is refused with WriterLockHeld.
class TurnupService final {
 public:
  TurnupService() = default;
  ~TurnupService();
  TurnupService(const TurnupService&) = delete;
  TurnupService& operator=(const TurnupService&) = delete;
  TurnupService(TurnupService&& other) noexcept;
  TurnupService& operator=(TurnupService&& other) noexcept;

  // Opens the durable store, loads the authoritative generation, advances the
  // incarnation, and reconciles authority: any authorization whose binding no
  // longer holds is fenced before the service answers a single question.
  [[nodiscard]] static Result<TurnupService> open(const StoreOptions& options);

  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] bool is_writable() const noexcept { return store_.is_writable(); }
  [[nodiscard]] const ServiceState& state() const noexcept { return state_; }
  [[nodiscard]] const std::string& store_path() const noexcept { return store_.path(); }
  [[nodiscard]] const RecoveryReport& open_report() const noexcept { return open_report_; }

  [[nodiscard]] Result<RecoveryReport> recover();
  [[nodiscard]] Result<RackSummary> register_rack(const RegisterRackRequest& request);
  [[nodiscard]] Result<RackSummary> update_composition(const UpdateCompositionRequest& request);
  [[nodiscard]] Result<RackSummary> create_plan(const CreatePlanRequest& request);
  [[nodiscard]] Result<EvidenceReceipt> import_evidence(const ImportEvidenceRequest& request);
  [[nodiscard]] Result<AuthorizationReceipt> authorize_turnup(const AuthorizeRequest& request);
  [[nodiscard]] Result<ActivationReceipt> record_activation(const RecordActivationRequest& request);
  [[nodiscard]] Result<CommissionReceipt> commission(const CommissionRequest& request);
  [[nodiscard]] Result<RollbackReceipt> rollback(const RollbackRequest& request);
  [[nodiscard]] Result<LifecycleReceipt> begin_drain(const LifecycleRequest& request);
  [[nodiscard]] Result<LifecycleReceipt> complete_drain(const LifecycleRequest& request);
  [[nodiscard]] Result<LifecycleReceipt> decommission(const LifecycleRequest& request);
  [[nodiscard]] Result<TakeoverReport> take_control(const TakeControlRequest& request);

  [[nodiscard]] Result<Evaluation> evaluate(const RackId& rack, WallClock authority_time) const;
  [[nodiscard]] Result<BlockerReport> blockers(const RackId& rack, WallClock authority_time) const;
  [[nodiscard]] Result<RackSummary> summary(const RackId& rack, WallClock authority_time) const;
  [[nodiscard]] Result<ServiceSummary> summary(WallClock authority_time) const;
  [[nodiscard]] Result<std::vector<EvidenceRecord>> evidence(const RackId& rack) const;
  [[nodiscard]] Result<std::vector<TurnupAuthorization>> authorizations(const RackId& rack) const;
  [[nodiscard]] Result<std::vector<AuthorizationFence>> fences(const RackId& rack) const;
  [[nodiscard]] Result<std::vector<ProvenanceRecord>> provenance(const RackId& rack) const;
  [[nodiscard]] Result<std::vector<RejectionRecord>> rejections() const;
  [[nodiscard]] Result<std::vector<RackId>> rack_ids() const;

 private:
  [[nodiscard]] Result<const RackState*> require_rack(const RackId& rack) const;
  [[nodiscard]] Result<RackState*> require_mutable_rack(ServiceState& working, const RackId& rack) const;
  [[nodiscard]] Status commit_working(ServiceState& working, WallClock authority_time);
  [[nodiscard]] Result<bool> replay_or_conflict(const ServiceState& working, const RackId& rack,
                                                OperationKind operation, const RequestId& request,
                                                const Digest& request_digest,
                                                const IdempotencyRecord*& receipt) const;
  void record_rejection(const MutationContext& context, OperationKind operation, const RackId& rack,
                        const PlanId& plan, const Digest& request, const TurnupError& error);
  [[nodiscard]] Result<Evaluation> evaluate_rack_state(const RackState& rack,
                                                      WallClock authority_time) const;
  void append_provenance(RackState& rack, const MutationContext& context, OperationKind operation,
                         const PlanId& plan, PlanRevision revision,
                         ObservationSequence sequence, const Digest& request) const;
  void append_receipt(RackState& rack, const MutationContext& context, OperationKind operation,
                      const Digest& request_digest, const IdempotencyRecord& receipt) const;
  void journal_rejection(const MutationContext& context, OperationKind operation, const RackId& rack,
                         const PlanId& plan, const Digest& request, const TurnupError& error);
  [[nodiscard]] Result<LifecycleReceipt> lifecycle_transition(OperationKind operation,
                                                            const LifecycleRequest& request,
                                                            RackLifecycleState target,
                                                            ErrorCode wrong_state_code,
                                                            const char* wrong_state_message);
  [[nodiscard]] Status reconcile(bool reload, std::size_t& fences_added,
                                 std::vector<std::string>& notes);
  [[nodiscard]] RecoveryReport make_report(std::size_t fences_added,
                                           const std::vector<std::string>& notes) const;
  [[nodiscard]] Status fence_active_authorizations(RackState& rack, ErrorCode reason,
                                                  const std::string& explanation,
                                                  const MutationContext& context,
                                                  const Digest& request) const;

  DurableStore store_;
  ServiceState state_;
  RecoveryReport open_report_;
  bool open_ = false;
};

}  // namespace rackturnup
