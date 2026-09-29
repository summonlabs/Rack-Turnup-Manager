// Rack Turnup Manager - turnup service implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/service.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/limits.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

[[nodiscard]] TurnupError missing_actor(OperationKind operation) {
  return make_error(ErrorCode::InvalidArgument, "a mutation must name the actor that requested it",
                    ErrorDetail{.operation = std::string(operation_kind_name(operation))});
}

void update_digest_bytes(Sha256& hasher, const Digest& digest) noexcept {
  const std::vector<std::uint8_t>& bytes = digest.bytes();
  hasher.update(bytes.data(), bytes.size());
}

void update_time(Sha256& hasher, WallClock time) noexcept {
  hasher.update_u64(static_cast<std::uint64_t>(time.unix_milliseconds()));
}

void update_members(Sha256& hasher, const std::vector<CompositionMember>& members) {
  hasher.update_u32(static_cast<std::uint32_t>(members.size()));
  for (const CompositionMember& member : members) {
    member.update_digest(hasher);
  }
}

// Request digests cover the intent of a request, never its envelope: a retry
// that names a different actor or a later authority time is still a replay of
// the same intent, while a changed payload is a conflict.
[[nodiscard]] Digest digest_register_request(const RegisterRackRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::RegisterRack));
  hasher.update_len_text(request.rack.text());
  hasher.update_len_text(request.label.text());
  hasher.update_len_text(request.site.text());
  update_members(hasher, request.members);
  return hasher.finish();
}

[[nodiscard]] Digest digest_update_composition_request(const UpdateCompositionRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::UpdateComposition));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_rack_generation.value());
  hasher.update_u64(request.composition_generation.value());
  hasher.update_byte(request.replace_commissioned ? 1u : 0u);
  update_members(hasher, request.members);
  return hasher.finish();
}

[[nodiscard]] Digest digest_create_plan_request(const CreatePlanRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::CreatePlan));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_rack_generation.value());
  request.stamp.update_digest(hasher);
  request.requirements.update_digest(hasher);
  request.policy.update_digest(hasher);
  hasher.update_byte(request.cancel_active_authorization ? 1u : 0u);
  hasher.update_byte(request.replace_commissioned ? 1u : 0u);
  return hasher.finish();
}

[[nodiscard]] Digest digest_import_evidence_request(const ImportEvidenceRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::ImportEvidence));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_revision.value());
  hasher.update_byte(static_cast<std::uint8_t>(request.subsystem));
  request.payload.update_digest(hasher);
  update_time(hasher, request.observed_at);
  hasher.update_u64(static_cast<std::uint64_t>(request.validity.milliseconds()));
  hasher.update_byte(request.validity_from_policy ? 1u : 0u);
  hasher.update_byte(request.stamp_from_plan ? 1u : 0u);
  if (!request.stamp_from_plan) {
    request.stamp.update_digest(hasher);
  }
  return hasher.finish();
}

[[nodiscard]] Digest digest_authorize_request(const AuthorizeRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::AuthorizeTurnup));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_revision.value());
  hasher.update_u64(static_cast<std::uint64_t>(request.validity.milliseconds()));
  hasher.update_byte(request.validity_from_policy ? 1u : 0u);
  return hasher.finish();
}

[[nodiscard]] Digest digest_record_activation_request(const RecordActivationRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::RecordActivation));
  hasher.update_len_text(request.rack.text());
  hasher.update_len_text(request.attempt.text());
  hasher.update_u64(request.expected_revision.value());
  hasher.update_byte(static_cast<std::uint8_t>(request.outcome));
  update_digest_bytes(hasher, request.observed_composition);
  hasher.update_u32(request.active_members);
  update_time(hasher, request.observed_at);
  hasher.update_u64(static_cast<std::uint64_t>(request.validity.milliseconds()));
  hasher.update_byte(request.validity_from_policy ? 1u : 0u);
  return hasher.finish();
}

[[nodiscard]] Digest digest_commission_request(const CommissionRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::Commission));
  hasher.update_len_text(request.rack.text());
  hasher.update_len_text(request.attempt.text());
  hasher.update_u64(request.expected_revision.value());
  return hasher.finish();
}

[[nodiscard]] Digest digest_rollback_request(const RollbackRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::Rollback));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_revision.value());
  hasher.update_len_text(request.reason.text());
  hasher.update_byte(request.acknowledge_active ? 1u : 0u);
  return hasher.finish();
}

[[nodiscard]] Digest digest_lifecycle_request(OperationKind operation,
                                              const LifecycleRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(operation));
  hasher.update_len_text(request.rack.text());
  hasher.update_u64(request.expected_revision.value());
  hasher.update_len_text(request.note.text());
  return hasher.finish();
}

[[nodiscard]] Digest digest_take_control_request(const TakeControlRequest& request) {
  Sha256 hasher;
  hasher.update_u32(static_cast<std::uint32_t>(OperationKind::TakeControl));
  hasher.update_len_text(request.context.actor.text());
  return hasher.finish();
}

// The digest of the state an activation observation claims to have seen.
[[nodiscard]] Digest digest_observed_state(ActivationOutcome outcome, std::uint32_t active_members,
                                           WallClock observed_at, const Digest& composition) {
  Sha256 hasher;
  hasher.update_u32(1u);
  hasher.update_byte(static_cast<std::uint8_t>(outcome));
  hasher.update_u32(active_members);
  update_time(hasher, observed_at);
  update_digest_bytes(hasher, composition);
  return hasher.finish();
}

[[nodiscard]] bool same_activation_claim(const ActivationRecord& record, ActivationOutcome outcome,
                                         std::uint32_t active_members, WallClock observed_at,
                                         const Digest& composition) {
  return record.outcome == outcome && record.active_members == active_members &&
         record.observed_at == observed_at &&
         digests_equal(record.observed_composition, composition);
}

[[nodiscard]] std::string blocker_summary(const Evaluation& evaluation) {
  const Blocker* blocker = evaluation.primary_blocker();
  if (blocker == nullptr) {
    return std::string("the rack is ") + std::string(turnup_verdict_name(evaluation.verdict));
  }
  return std::string(code_name(blocker->code)) + ": " + blocker->explanation;
}

// Rebuilds the observation payload so that the collections a policy depends on
// are canonical (sorted, duplicate free) before they are stored. A payload the
// caller assembled by hand is either canonicalized or refused.
[[nodiscard]] Result<EvidencePayload> normalize_payload(const EvidencePayload& payload) {
  if (!payload.is_consistent()) {
    return make_error(ErrorCode::InvalidArgument,
                      "the evidence payload does not match the subsystem it is tagged with",
                      ErrorDetail{.operation = "import_evidence",
                                  .related = std::string(evidence_payload_kind_name(payload.kind))});
  }
  EvidencePayload normalized = payload;
  switch (payload.kind) {
    case EvidencePayloadKind::Inventory: {
      const Result<InventoryObservation> rebuilt =
          create_inventory_observation(payload.inventory.entries, payload.inventory.unreadable_positions);
      if (!rebuilt.has_value()) {
        return rebuilt.error();
      }
      normalized.inventory = rebuilt.value();
      break;
    }
    case EvidencePayloadKind::Health: {
      const Result<HealthObservation> rebuilt =
          create_health_observation(payload.health.samples, payload.health.unchecked_devices);
      if (!rebuilt.has_value()) {
        return rebuilt.error();
      }
      normalized.health = rebuilt.value();
      break;
    }
    case EvidencePayloadKind::Compatibility: {
      const Result<CompatibilityObservation> rebuilt =
          create_compatibility_observation(payload.compatibility.members);
      if (!rebuilt.has_value()) {
        return rebuilt.error();
      }
      normalized.compatibility = rebuilt.value();
      break;
    }
    case EvidencePayloadKind::IdentityComposition:
    case EvidencePayloadKind::Power:
    case EvidencePayloadKind::Cooling:
    case EvidencePayloadKind::Network:
      break;
  }
  return normalized;
}

}  // namespace

TurnupService::~TurnupService() { close(); }

TurnupService::TurnupService(TurnupService&& other) noexcept
    : store_(std::move(other.store_)),
      state_(std::move(other.state_)),
      open_report_(std::move(other.open_report_)),
      open_(other.open_) {
  other.open_ = false;
}

TurnupService& TurnupService::operator=(TurnupService&& other) noexcept {
  if (this != &other) {
    close();
    store_ = std::move(other.store_);
    state_ = std::move(other.state_);
    open_report_ = std::move(other.open_report_);
    open_ = other.open_;
    other.open_ = false;
  }
  return *this;
}

void TurnupService::close() noexcept {
  store_.close();
  open_ = false;
}

Result<TurnupService> TurnupService::open(const StoreOptions& options) {
  Result<DurableStore> store = DurableStore::open(options);
  if (!store.has_value()) {
    return store.error();
  }
  Result<ServiceState> state = store.value().load();
  if (!state.has_value()) {
    return state.error();
  }

  TurnupService service;
  service.store_ = std::move(store.value());
  service.state_ = std::move(state.value());
  service.open_ = true;

  std::size_t fences_added = 0;
  std::vector<std::string> notes;
  const Status reconciled = service.reconcile(false, fences_added, notes);
  if (!reconciled.has_value()) {
    return reconciled.error();
  }
  service.open_report_ = service.make_report(fences_added, notes);
  return Result<TurnupService>(std::move(service));
}

RecoveryReport TurnupService::make_report(std::size_t fences_added,
                                          const std::vector<std::string>& notes) const {
  RecoveryReport report;
  report.racks = state_.racks.size();
  report.fences_added = fences_added;
  report.plans_superseded = 0;
  report.store_epoch = state_.store_epoch;
  report.store_sequence = state_.store_sequence;
  report.incarnation = state_.incarnation;
  report.control_epoch = state_.control_epoch;
  report.observation_sequence = state_.observation_sequence;
  report.notes = notes;
  return report;
}

Status TurnupService::reconcile(bool reload, std::size_t& fences_added,
                                std::vector<std::string>& notes) {
  if (reload) {
    if (!store_.is_writable()) {
      return make_error(ErrorCode::ReadOnlyStore, "recovery requires a writable store",
                        ErrorDetail{.operation = "recover", .subject = store_.path()});
    }
    const Result<ServiceState> reloaded = store_.load();
    if (!reloaded.has_value()) {
      return reloaded.error();
    }
    state_ = reloaded.value();
  }

  ServiceState working = state_;
  bool changed = false;
  fences_added = 0;
  notes.clear();

  for (RackState& rack : working.racks) {
    bool plan_superseded = false;
    for (RackTurnupPlan& plan : rack.plans) {
      if (plan.state != PlanState::Active) {
        continue;
      }
      const bool composition_changed =
          !digests_equal(plan.binding.composition, rack.rack.composition_digest());
      const bool generation_changed =
          !(plan.binding.stamp.rack == rack.rack.generation) ||
          !(plan.binding.stamp.composition == rack.rack.composition_generation());
      if (composition_changed || generation_changed) {
        plan.state = PlanState::Superseded;
        plan_superseded = true;
        changed = true;
        notes.push_back(rack.rack.id.text() + ": plan " + plan.id.text() +
                        " no longer matches the rack record and was superseded");
      }
    }

    for (TurnupAuthorization& authorization : rack.authorizations) {
      if (!authorization.is_active()) {
        continue;
      }
      ErrorCode reason = ErrorCode::Ok;
      std::string explanation;
      const RackTurnupPlan* plan = rack.find_plan(authorization.plan);
      if (rack.fences.size() >= kMaxFencesPerRack) {
        return make_error(ErrorCode::CapacityExhausted,
                          "the rack has no room left for another authorization fence",
                          ErrorDetail{.operation = "recover", .subject = rack.rack.id.text()});
      }
      if (plan == nullptr || plan->state != PlanState::Active || plan_superseded) {
        reason = ErrorCode::PlanNotActive;
        explanation = "the plan the authorization belongs to is no longer active";
      } else if (!(plan->revision == authorization.revision)) {
        reason = ErrorCode::StalePlanRevision;
        explanation = "the plan advanced after the authorization was issued";
      } else if (!(authorization.epoch == working.control_epoch)) {
        reason = ErrorCode::StaleAuthorityEpoch;
        explanation = "control authority was taken over after the authorization was issued";
      } else if (!digests_equal(authorization.composition, rack.rack.composition_digest())) {
        reason = ErrorCode::CompositionDigestMismatch;
        explanation = "the rack composition changed after the authorization was issued";
      }
      if (reason == ErrorCode::Ok) {
        continue;
      }
      authorization.state = AuthorizationState::Fenced;
      AuthorizationFence fence;
      fence.attempt = authorization.attempt;
      fence.reason = reason;
      fence.explanation = explanation;
      fence.sequence = working.observation_sequence;
      fence.fenced_at = working.updated_at;
      fence.actor = working.last_actor;
      rack.fences.push_back(fence);
      ++fences_added;
      changed = true;
      notes.push_back(rack.rack.id.text() + ": authorization " + authorization.attempt.text() +
                      " was fenced during recovery");
    }

    if (rack.rack.lifecycle == RackLifecycleState::Authorized) {
      bool active_authorization = false;
      for (const TurnupAuthorization& authorization : rack.authorizations) {
        if (authorization.is_active()) {
          active_authorization = true;
          break;
        }
      }
      if (!active_authorization) {
        rack.rack.lifecycle = RackLifecycleState::Planned;
        changed = true;
        notes.push_back(rack.rack.id.text() +
                        ": the rack was authorized but holds no active authorization, so it "
                        "returned to planned");
      }
    }
  }

  if (!changed) {
    return Status{};
  }
  if (!store_.is_writable()) {
    return Status{};
  }
  const Status committed = commit_working(working, working.updated_at);
  if (!committed.has_value()) {
    return committed;
  }
  return Status{};
}

Result<RecoveryReport> TurnupService::recover() {
  if (!open_) {
    return make_error(ErrorCode::InvalidArgument, "the service is not open",
                      ErrorDetail{.operation = "recover"});
  }
  std::size_t fences_added = 0;
  std::vector<std::string> notes;
  const Status reconciled = reconcile(true, fences_added, notes);
  if (!reconciled.has_value()) {
    return reconciled.error();
  }
  RecoveryReport report = make_report(fences_added, notes);
  report.published = fences_added != 0;
  return report;
}

Status TurnupService::commit_working(ServiceState& working, WallClock authority_time) {
  if (!store_.is_writable()) {
    return make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                      ErrorDetail{.operation = "commit", .subject = store_.path()});
  }
  if (store_.sequence().is_max()) {
    return make_error(ErrorCode::LimitExceeded, "the publication sequence is exhausted",
                      ErrorDetail{.operation = "commit", .subject = store_.path()});
  }
  working.store_sequence = store_.sequence().next();
  working.store_epoch = store_.epoch();
  working.updated_at = authority_time;
  const Status committed = store_.commit(working);
  if (!committed.has_value()) {
    return committed;
  }
  state_ = std::move(working);
  return Status{};
}

void TurnupService::journal_rejection(const MutationContext& context, OperationKind operation,
                                      const RackId& rack, const PlanId& plan, const Digest& request,
                                      const TurnupError& error) {
  if (!open_ || !store_.is_writable()) {
    return;
  }
  ServiceState working = state_;
  RejectionRecord record;
  record.code = error.code;
  record.operation = operation;
  record.rack = rack;
  record.plan = plan;
  record.message = error.message;
  record.at = context.authority_time;
  record.actor = context.actor;
  record.request = request;
  if (working.rejections.size() >= kMaxRejectionJournalEntries) {
    working.rejections.erase(working.rejections.begin());
    ++working.rejection_evictions;
  }
  working.rejections.push_back(record);
  // The rejection itself is what the caller sees; a journal that cannot be
  // published is not allowed to replace or hide it.
  const Status committed = commit_working(working, context.authority_time);
  static_cast<void>(committed);
}

Result<const RackState*> TurnupService::require_rack(const RackId& rack) const {
  if (rack.empty()) {
    return make_error(ErrorCode::EmptyValue, "a rack identity is required",
                      ErrorDetail{.operation = "require_rack"});
  }
  const RackState* state = state_.find_rack(rack);
  if (state == nullptr) {
    return make_error(ErrorCode::UnknownRackId, "no rack with that identity is registered",
                      ErrorDetail{.operation = "require_rack", .subject = rack.text()});
  }
  return state;
}

Result<RackState*> TurnupService::require_mutable_rack(ServiceState& working,
                                                      const RackId& rack) const {
  if (rack.empty()) {
    return make_error(ErrorCode::EmptyValue, "a rack identity is required",
                      ErrorDetail{.operation = "require_mutable_rack"});
  }
  RackState* state = working.find_rack(rack);
  if (state == nullptr) {
    return make_error(ErrorCode::UnknownRackId, "no rack with that identity is registered",
                      ErrorDetail{.operation = "require_mutable_rack", .subject = rack.text()});
  }
  return state;
}

Result<bool> TurnupService::replay_or_conflict(const ServiceState& working, const RackId& rack,
                                               OperationKind operation, const RequestId& request,
                                               const Digest& request_digest,
                                               const IdempotencyRecord*& receipt) const {
  receipt = nullptr;
  if (request.empty()) {
    return false;
  }
  for (const RackState& candidate : working.racks) {
    const IdempotencyRecord* record = candidate.find_receipt(request);
    if (record == nullptr) {
      continue;
    }
    if (record->operation != operation || !digests_equal(record->request_digest, request_digest)) {
      return make_error(ErrorCode::RequestIdConflict,
                        "the request identity was already used for a different request",
                        ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                    .subject = request.text(),
                                    .related = rack.text()});
    }
    if (!digests_equal(record->response_digest, record->compute_digest())) {
      return make_error(ErrorCode::IntegrityCheckFailed,
                        "the stored receipt does not match its own digest",
                        ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                    .subject = request.text()});
    }
    receipt = record;
    return true;
  }
  return false;
}


Result<Evaluation> TurnupService::evaluate_rack_state(const RackState& rack,
                                                     WallClock authority_time) const {
  const RackTurnupPlan* plan = rack.current_plan();
  if (plan == nullptr) {
    return make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                      ErrorDetail{.operation = "evaluate", .subject = rack.rack.id.text()});
  }
  const EvaluationContext context{rack.rack.composition, *plan,       rack.evidence,
                                  rack.authorizations,   rack.activations, rack.commissions,
                                  rack.rack.generation,  rack.rack.lifecycle, authority_time};
  return evaluate_rack(context);
}

void TurnupService::append_provenance(RackState& rack, const MutationContext& context,
                                      OperationKind operation, const PlanId& plan,
                                      PlanRevision revision, ObservationSequence sequence,
                                      const Digest& request) const {
  ProvenanceRecord record;
  record.operation = operation;
  record.rack = rack.rack.id;
  record.plan = plan;
  record.actor = context.actor;
  record.source = context.source;
  record.at = context.authority_time;
  record.revision = revision;
  record.sequence = sequence;
  record.request = request;
  record.note = context.note;
  if (rack.provenance.size() >= kMaxProvenanceRecordsPerRack) {
    rack.provenance.erase(rack.provenance.begin());
  }
  rack.provenance.push_back(std::move(record));
}

void TurnupService::append_receipt(RackState& rack, const MutationContext& context,
                                   OperationKind operation, const Digest& request_digest,
                                   const IdempotencyRecord& receipt) const {
  static_cast<void>(context);
  static_cast<void>(operation);
  if (receipt.request.empty()) {
    return;
  }
  if (rack.idempotency.size() >= kMaxIdempotencyRecordsPerRack) {
    rack.idempotency.erase(rack.idempotency.begin());
    ++rack.idempotency_evictions;
  }
  static_cast<void>(request_digest);
  rack.idempotency.push_back(receipt);
}

Result<RackSummary> TurnupService::register_rack(const RegisterRackRequest& request) {
  constexpr OperationKind kOperation = OperationKind::RegisterRack;
  const Digest request_digest = digest_register_request(request);
  const auto reject = [&](TurnupError error) -> Result<RackSummary> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  // A retry of a request that was already applied is answered from its receipt,
  // even though the rack it created now exists: the caller lost the response,
  // not the mutation. Only a genuinely new request for an existing rack is a
  // duplicate.
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    const Result<RackSummary> summary = TurnupService::summary(request.rack,
                                                              request.context.authority_time);
    if (!summary.has_value()) {
      return reject(summary.error());
    }
    return summary;
  }
  if (working.find_rack(request.rack) != nullptr) {
    return reject(make_error(ErrorCode::DuplicateRackId,
                             "the rack identity is already registered",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  const Result<RackComposition> composition = RackComposition::create(
      request.rack, request.site, CompositionGeneration::initial(), request.members);
  if (!composition.has_value()) {
    return reject(composition.error());
  }
  if (working.racks.size() >= kMaxRacks) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "the service already manages as many racks as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .expected = kMaxRacks,
                                         .actual = working.racks.size()}));
  }

  RackState rack;
  rack.rack.id = request.rack;
  rack.rack.label = request.label;
  rack.rack.site = request.site;
  rack.rack.generation = RackGeneration::initial();
  rack.rack.composition = composition.value();
  rack.rack.lifecycle = RackLifecycleState::Registered;
  rack.rack.registered_at = request.context.authority_time;
  rack.rack.registered_by = request.context.actor;
  rack.rack.source = request.context.source;
  rack.rack.note = request.context.note;

  append_provenance(rack, request.context, kOperation, PlanId{}, PlanRevision{},
                    working.observation_sequence, request_digest);
  IdempotencyRecord receipt;
  receipt.request = request.context.request;
  receipt.operation = kOperation;
  receipt.request_digest = request_digest;
  receipt.rack = request.rack;
  receipt.recorded_at = request.context.authority_time;
  receipt.actor = request.context.actor;
  receipt.response_digest = receipt.compute_digest();
  append_receipt(rack, request.context, kOperation, request_digest, receipt);

  const auto position = std::lower_bound(
      working.racks.begin(), working.racks.end(), request.rack,
      [](const RackState& candidate, const RackId& id) {
        return byte_less(candidate.rack.id.text(), id.text());
      });
  working.racks.insert(position, std::move(rack));

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  return summary(request.rack, request.context.authority_time);
}

Result<RackSummary> TurnupService::update_composition(const UpdateCompositionRequest& request) {
  constexpr OperationKind kOperation = OperationKind::UpdateComposition;
  const Digest request_digest = digest_update_composition_request(request);
  const auto reject = [&](TurnupError error) -> Result<RackSummary> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    return summary(request.rack, request.context.authority_time);
  }
  if (!(rack.rack.generation == request.expected_rack_generation)) {
    return reject(make_error(ErrorCode::StaleRackGeneration,
                             "the rack record advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = rack.rack.generation.value(),
                                         .actual = request.expected_rack_generation.value()}));
  }
  if (!(request.composition_generation > rack.rack.composition_generation())) {
    return reject(make_error(ErrorCode::CompositionGenerationRegression,
                             "a new composition generation must be strictly greater",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = rack.rack.composition_generation().value() + 1,
                                         .actual = request.composition_generation.value()}));
  }
  const bool in_service = rack.rack.lifecycle == RackLifecycleState::Authorized ||
                          rack.rack.lifecycle == RackLifecycleState::Active ||
                          rack.rack.lifecycle == RackLifecycleState::Commissioned ||
                          rack.rack.lifecycle == RackLifecycleState::Draining;
  if (in_service && !request.replace_commissioned) {
    return reject(make_error(
        (rack.rack.lifecycle == RackLifecycleState::Authorized) ? ErrorCode::TurnupAlreadyAuthorized
                                                                : ErrorCode::AlreadyCommissioned,
        "the rack is in service under an existing plan; re-composing it must be requested "
        "explicitly",
        ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                    .subject = request.rack.text(),
                    .related = std::string(rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  const Result<RackComposition> composition = RackComposition::create(
      request.rack, rack.rack.site, request.composition_generation, request.members);
  if (!composition.has_value()) {
    return reject(composition.error());
  }
  if (rack.fences.size() + rack.authorizations.size() > kMaxFencesPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack has no room left for another authorization record",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }

  rack.rack.composition = composition.value();
  std::size_t fenced = 0;
  for (TurnupAuthorization& authorization : rack.authorizations) {
    if (!authorization.is_active()) {
      continue;
    }
    authorization.state = AuthorizationState::Fenced;
    AuthorizationFence fence;
    fence.attempt = authorization.attempt;
    fence.reason = ErrorCode::CompositionDigestMismatch;
    fence.explanation = "the rack composition changed after the authorization was issued";
    fence.sequence = working.observation_sequence;
    fence.fenced_at = request.context.authority_time;
    fence.actor = request.context.actor;
    fence.request = request_digest;
    rack.fences.push_back(fence);
    ++fenced;
  }
  for (RackTurnupPlan& plan : rack.plans) {
    if (plan.state == PlanState::Active) {
      plan.state = PlanState::Superseded;
    }
  }
  if (in_service) {
    rack.rack.lifecycle = RackLifecycleState::Planned;
  }
  rack.rack.generation = rack.rack.generation.next();
  if (rack.rack.generation.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the rack generation is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }

  append_provenance(rack, request.context, kOperation, PlanId{}, PlanRevision{},
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }
  static_cast<void>(fenced);

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  return summary(request.rack, request.context.authority_time);
}


// ---------------------------------------------------------------------------
// Identity allocation
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Result<PlanId> make_plan_id(PlanLifetime lifetime) {
  return PlanId::parse("tp:" + std::to_string(lifetime.value()));
}

[[nodiscard]] Result<AttemptId> make_attempt_id(std::size_t ordinal) {
  return AttemptId::parse("at:" + std::to_string(ordinal));
}

[[nodiscard]] Result<EvidenceId> make_evidence_id(ObservationSequence sequence) {
  return EvidenceId::parse("te:" + std::to_string(sequence.value()));
}

}  // namespace

Status TurnupService::fence_active_authorizations(RackState& rack, ErrorCode reason,
                                                  const std::string& explanation,
                                                  const MutationContext& context,
                                                  const Digest& request) const {
  for (TurnupAuthorization& authorization : rack.authorizations) {
    if (!authorization.is_active()) {
      continue;
    }
    if (rack.fences.size() >= kMaxFencesPerRack) {
      return make_error(ErrorCode::CapacityExhausted,
                        "the rack has no room left for another authorization fence",
                        ErrorDetail{.operation = "fence_active_authorizations",
                                    .subject = rack.rack.id.text()});
    }
    authorization.state = AuthorizationState::Fenced;
    AuthorizationFence fence;
    fence.attempt = authorization.attempt;
    fence.reason = reason;
    fence.explanation = explanation;
    fence.sequence = state_.observation_sequence;
    fence.fenced_at = context.authority_time;
    fence.actor = context.actor;
    fence.request = request;
    rack.fences.push_back(std::move(fence));
  }
  return Status{};
}

Result<RackSummary> TurnupService::create_plan(const CreatePlanRequest& request) {
  constexpr OperationKind kOperation = OperationKind::CreatePlan;
  const Digest request_digest = digest_create_plan_request(request);
  const auto reject = [&](TurnupError error) -> Result<RackSummary> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    return summary(request.rack, request.context.authority_time);
  }
  if (!(rack.rack.generation == request.expected_rack_generation)) {
    return reject(make_error(ErrorCode::StaleRackGeneration,
                             "the rack record advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = rack.rack.generation.value(),
                                         .actual = request.expected_rack_generation.value()}));
  }
  // The plan carries exactly the policy this request declares, so a caller that
  // leaves the policy generation out of its stamp has it filled in from the
  // policy. A contradictory non-zero value is refused below.
  GenerationStamp stamp = request.stamp;
  if (stamp.policy.value() == 0) {
    stamp.policy = request.policy.generation;
  }
  if (!(stamp.rack == rack.rack.generation)) {
    return reject(make_error(ErrorCode::StaleRackGeneration,
                             "the plan stamp does not carry the current rack generation",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = rack.rack.generation.value(),
                                         .actual = stamp.rack.value()}));
  }
  if (!(stamp.composition == rack.rack.composition_generation())) {
    return reject(make_error(ErrorCode::StaleCompositionGeneration,
                             "the plan stamp does not carry the current composition generation",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = rack.rack.composition_generation().value(),
                                         .actual = stamp.composition.value()}));
  }
  if (!(stamp.policy == request.policy.generation)) {
    return reject(make_error(ErrorCode::StalePolicyGeneration,
                             "the plan stamp disagrees with the policy it carries",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = request.policy.generation.value(),
                                         .actual = stamp.policy.value()}));
  }
  const Result<PlanRequirements> requirements = make_plan_requirements(request.requirements);
  if (!requirements.has_value()) {
    return reject(requirements.error());
  }
  const Result<TurnupPolicy> policy = make_turnup_policy(request.policy);
  if (!policy.has_value()) {
    return reject(policy.error());
  }
  if (!lifecycle_allows(rack.rack.lifecycle, RackLifecycleState::Planned)) {
    return reject(make_error(ErrorCode::LifecycleTransitionNotAllowed,
                             "a rack in this lifecycle position cannot take a new plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  if (rack.rack.lifecycle == RackLifecycleState::Authorized &&
      !request.cancel_active_authorization) {
    return reject(make_error(ErrorCode::TurnupAlreadyAuthorized,
                             "an active authorization exists; cancel it or supersede it explicitly",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  const bool in_service = rack.rack.lifecycle == RackLifecycleState::Active ||
                          rack.rack.lifecycle == RackLifecycleState::Commissioned;
  if (in_service && !request.replace_commissioned) {
    return reject(make_error(ErrorCode::AlreadyCommissioned,
                             "the rack is in service; superseding its plan must be requested "
                             "explicitly",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  if (rack.plans.size() >= kMaxPlansPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack holds as many plans as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = kMaxPlansPerRack,
                                         .actual = rack.plans.size()}));
  }

  PlanLifetime lifetime = PlanLifetime::initial();
  if (!rack.plans.empty()) {
    if (rack.plans.back().lifetime.is_max()) {
      return reject(make_error(ErrorCode::LimitExceeded, "the plan lineage is exhausted",
                               ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                           .subject = request.rack.text()}));
    }
    lifetime = rack.plans.back().lifetime.next();
  }
  const Result<PlanId> plan_id = make_plan_id(lifetime);
  if (!plan_id.has_value()) {
    return reject(plan_id.error());
  }

  const Status fenced = fence_active_authorizations(
      rack, ErrorCode::AuthoritySuperseded,
      "a new plan superseded the plan this authorization belonged to", request.context,
      request_digest);
  if (!fenced.has_value()) {
    return reject(fenced.error());
  }
  for (RackTurnupPlan& plan : rack.plans) {
    if (plan.state == PlanState::Active) {
      plan.state = PlanState::Superseded;
    }
  }

  RackTurnupPlan plan;
  plan.id = plan_id.value();
  plan.lifetime = lifetime;
  plan.revision = PlanRevision::initial();
  plan.binding.rack = rack.rack.id;
  plan.binding.composition = rack.rack.composition_digest();
  plan.binding.stamp = stamp;
  plan.binding.epoch = working.control_epoch;
  plan.requirements = requirements.value();
  plan.policy = policy.value();
  plan.state = PlanState::Active;
  plan.requirements_digest = plan.compute_requirements_digest();
  plan.evidence_high_water = working.observation_sequence;
  plan.created_at = request.context.authority_time;
  plan.created_by = request.context.actor;
  plan.source = request.context.source;
  plan.note = request.context.note;
  if (!digests_equal(plan.requirements_digest, plan.compute_requirements_digest())) {
    return reject(make_error(ErrorCode::IntegrityCheckFailed,
                             "the plan requirements digest could not be computed",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  rack.rack.lifecycle = RackLifecycleState::Planned;
  rack.plans.push_back(std::move(plan));

  append_provenance(rack, request.context, kOperation, plan_id.value(), PlanRevision::initial(),
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan_id.value();
    receipt.revision_before = PlanRevision{};
    receipt.revision_after = PlanRevision::initial();
    receipt.sequence_after = working.observation_sequence;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  return summary(request.rack, request.context.authority_time);
}

Result<EvidenceReceipt> TurnupService::import_evidence(const ImportEvidenceRequest& request) {
  constexpr OperationKind kOperation = OperationKind::ImportEvidence;
  const Digest request_digest = digest_import_evidence_request(request);
  const auto reject = [&](TurnupError error) -> Result<EvidenceReceipt> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    EvidenceReceipt receipt;
    receipt.id = replay->evidence;
    receipt.sequence = replay->sequence_after;
    receipt.revision = replay->revision_after;
    receipt.replayed = true;
    return receipt;
  }
  RackTurnupPlan* plan = rack.active_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  if (!(plan->binding.epoch == working.control_epoch)) {
    return reject(make_error(ErrorCode::StaleAuthorityEpoch,
                             "the plan belongs to an older control epoch",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = working.control_epoch.value(),
                                         .actual = plan->binding.epoch.value()}));
  }
  if (!(request.subsystem == subsystem_for_payload_kind(request.payload.kind))) {
    return reject(make_error(ErrorCode::InvalidArgument,
                             "the payload kind does not match the subsystem it is imported as",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             subsystem_kind_name(request.subsystem))}));
  }
  const Result<EvidencePayload> payload = normalize_payload(request.payload);
  if (!payload.has_value()) {
    return reject(payload.error());
  }
  if (request.observed_at > request.context.authority_time) {
    return reject(make_error(ErrorCode::InvalidTimestamp,
                             "evidence cannot be observed after the authority time it is "
                             "imported at",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = request.observed_at.to_text()}));
  }
  const GenerationStamp stamp =
      request.stamp_from_plan ? plan->binding.stamp : request.stamp;
  if (!(stamp == plan->binding.stamp)) {
    return reject(make_error(ErrorCode::EvidenceBindingMismatch,
                             "evidence does not describe the generation set the plan binds to",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = describe_generation_mismatches(
                                             stamp.compare(plan->binding.stamp))}));
  }
  if (rack.evidence.size() >= kMaxEvidenceRecordsPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack holds as much evidence as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = kMaxEvidenceRecordsPerRack,
                                         .actual = rack.evidence.size()}));
  }
  if (working.observation_sequence.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "the observation sequence is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  const ObservationSequence sequence = working.observation_sequence.next();
  const Result<EvidenceId> evidence_id = make_evidence_id(sequence);
  if (!evidence_id.has_value()) {
    return reject(evidence_id.error());
  }
  if (plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }

  EvidenceRecord record;
  record.id = evidence_id.value();
  record.rack = rack.rack.id;
  record.subsystem = request.subsystem;
  record.stamp = stamp;
  record.composition = plan->binding.composition;
  record.observed_at = request.observed_at;
  record.validity = request.validity_from_policy ? plan->policy.default_evidence_validity
                                                 : request.validity;
  record.source = request.context.source;
  record.producer = request.context.actor;
  record.note = request.context.note;
  record.plan = plan->id;
  record.plan_revision = plan->revision.next();
  record.sequence = sequence;
  record.payload = payload.value();
  record.record_digest = record.compute_digest();

  plan->revision = plan->revision.next();
  plan->evidence_high_water = sequence;
  const PlanRevision revision_after = plan->revision;
  const PlanId plan_id_after = plan->id;
  working.observation_sequence = sequence;
  const Status fenced = fence_active_authorizations(
      rack, ErrorCode::AuthoritySuperseded,
      "evidence was imported after the authorization was issued", request.context, request_digest);
  if (!fenced.has_value()) {
    return reject(fenced.error());
  }
  if (rack.rack.lifecycle == RackLifecycleState::Authorized) {
    rack.rack.lifecycle = RackLifecycleState::Planned;
  }
  rack.evidence.push_back(record);

  append_provenance(rack, request.context, kOperation, plan->id, plan->revision, sequence,
                    request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan->id;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = plan->revision;
    receipt.sequence_after = sequence;
    receipt.evidence = record.id;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  EvidenceReceipt receipt;
  receipt.id = record.id;
  receipt.sequence = sequence;
  receipt.revision = revision_after;
  static_cast<void>(plan_id_after);
  return receipt;
}


Result<AuthorizationReceipt> TurnupService::authorize_turnup(const AuthorizeRequest& request) {
  constexpr OperationKind kOperation = OperationKind::AuthorizeTurnup;
  const Digest request_digest = digest_authorize_request(request);
  const auto reject = [&](TurnupError error) -> Result<AuthorizationReceipt> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    const TurnupAuthorization* authorization = rack.find_authorization(replay->attempt);
    if (authorization == nullptr) {
      return reject(make_error(ErrorCode::IntegrityCheckFailed,
                               "the recorded receipt names an authorization that is not present",
                               ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                           .subject = request.rack.text()}));
    }
    AuthorizationReceipt receipt;
    receipt.attempt = authorization->attempt;
    receipt.plan = authorization->plan;
    receipt.revision = authorization->revision;
    receipt.verdict = authorization->verdict;
    receipt.evidence_set = authorization->evidence_set;
    receipt.evidence_high_water = authorization->evidence_high_water;
    receipt.issued_at = authorization->issued_at;
    receipt.expires_at = WallClock(authorization->issued_at.unix_milliseconds() +
                                   authorization->validity.milliseconds());
    receipt.replayed = true;
    return receipt;
  }
  RackTurnupPlan* plan = rack.active_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  if (!(plan->binding.epoch == working.control_epoch)) {
    return reject(make_error(ErrorCode::StaleAuthorityEpoch,
                             "the plan belongs to an older control epoch",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = working.control_epoch.value(),
                                         .actual = plan->binding.epoch.value()}));
  }
  if (!request.validity_from_policy && request.validity.is_zero()) {
    return reject(make_error(ErrorCode::InvalidDuration,
                             "an authorization validity must be greater than zero",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (rack.authorizations.size() >= kMaxFencesPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack holds as many authorizations as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = kMaxFencesPerRack,
                                         .actual = rack.authorizations.size()}));
  }
  for (TurnupAuthorization& existing : rack.authorizations) {
    if (!existing.is_active()) {
      continue;
    }
    if (existing.plan == plan->id && existing.revision == plan->revision) {
      return reject(make_error(ErrorCode::TurnupAlreadyAuthorized,
                               "an active authorization already covers this plan revision",
                               ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                           .subject = request.rack.text(),
                                           .related = existing.attempt.text()}));
    }
  }
  const Status fenced = fence_active_authorizations(
      rack, ErrorCode::AuthoritySuperseded,
      "a newer authorization request superseded this one", request.context, request_digest);
  if (!fenced.has_value()) {
    return reject(fenced.error());
  }

  const Result<Evaluation> evaluation = evaluate_rack_state(rack, request.context.authority_time);
  if (!evaluation.has_value()) {
    return reject(evaluation.error());
  }
  if (!evaluation.value().ready_to_authorize()) {
    std::vector<std::string> items;
    for (const Blocker& blocker : evaluation.value().blockers) {
      items.push_back(std::string(code_name(blocker.code)) + " " + blocker.explanation);
    }
    return reject(make_error(ErrorCode::TurnupNotReady,
                             "the rack is not proven ready for turnup",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = blocker_summary(evaluation.value()),
                                         .items = std::move(items)}));
  }
  if (rack.rack.lifecycle == RackLifecycleState::Planned &&
      !lifecycle_allows(rack.rack.lifecycle, RackLifecycleState::Authorized)) {
    return reject(make_error(ErrorCode::LifecycleTransitionNotAllowed,
                             "the rack is not in a lifecycle position that can be authorized",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  if (plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  const Result<AttemptId> attempt = make_attempt_id(rack.authorizations.size() + 1);
  if (!attempt.has_value()) {
    return reject(attempt.error());
  }

  plan->revision = plan->revision.next();
  TurnupAuthorization authorization;
  authorization.attempt = attempt.value();
  authorization.rack = rack.rack.id;
  authorization.plan = plan->id;
  authorization.lifetime = plan->lifetime;
  authorization.revision = plan->revision;
  authorization.composition = evaluation.value().composition;
  authorization.stamp = plan->binding.stamp;
  authorization.epoch = working.control_epoch;
  authorization.policy = plan->policy.generation;
  authorization.verdict = evaluation.value().verdict_digest;
  authorization.evidence_set = evaluation.value().evidence_set;
  authorization.evidence_high_water = plan->evidence_high_water;
  authorization.issued_at = request.context.authority_time;
  authorization.validity = request.validity_from_policy ? plan->policy.authorization_validity
                                                        : request.validity;
  authorization.actor = request.context.actor;
  authorization.source = request.context.source;
  authorization.state = AuthorizationState::Active;
  authorization.authorization_digest = authorization.compute_digest();
  const AttemptId attempt_id = authorization.attempt;
  const PlanId authorization_plan = authorization.plan;
  const PlanRevision authorization_revision = authorization.revision;
  const ObservationSequence authorization_high_water = authorization.evidence_high_water;
  const Millis authorization_validity = authorization.validity;
  rack.authorizations.push_back(std::move(authorization));
  rack.rack.lifecycle = RackLifecycleState::Authorized;

  append_provenance(rack, request.context, kOperation, plan->id, plan->revision,
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan->id;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = plan->revision;
    receipt.sequence_after = working.observation_sequence;
    receipt.attempt = attempt_id;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }

  // The receipt is captured before publication: publishing moves the working
  // state into the service, which would leave every reference into it dangling.
  AuthorizationReceipt receipt;
  receipt.attempt = attempt_id;
  receipt.plan = authorization_plan;
  receipt.revision = authorization_revision;
  receipt.verdict = evaluation.value().verdict_digest;
  receipt.evidence_set = evaluation.value().evidence_set;
  receipt.evidence_high_water = authorization_high_water;
  receipt.issued_at = request.context.authority_time;
  receipt.expires_at = WallClock(request.context.authority_time.unix_milliseconds() +
                                 authorization_validity.milliseconds());
  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  return receipt;
}

Result<ActivationReceipt> TurnupService::record_activation(const RecordActivationRequest& request) {
  constexpr OperationKind kOperation = OperationKind::RecordActivation;
  const Digest request_digest = digest_record_activation_request(request);
  const auto reject = [&](TurnupError error) -> Result<ActivationReceipt> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    const ActivationRecord* stored = nullptr;
    for (const ActivationRecord& candidate : rack.activations) {
      if (candidate.attempt == replay->attempt) {
        stored = &candidate;
      }
    }
    if (stored == nullptr) {
      return reject(make_error(ErrorCode::IntegrityCheckFailed,
                               "the recorded receipt names an activation that is not present",
                               ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                           .subject = request.rack.text()}));
    }
    ActivationReceipt receipt;
    receipt.attempt = stored->attempt;
    receipt.plan = stored->plan;
    receipt.revision = stored->revision;
    receipt.sequence = stored->sequence;
    receipt.outcome = stored->outcome;
    receipt.replayed = true;
    return receipt;
  }
  RackTurnupPlan* plan = rack.active_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  if (!(plan->binding.epoch == working.control_epoch)) {
    return reject(make_error(ErrorCode::StaleAuthorityEpoch,
                             "the plan belongs to an older control epoch",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = working.control_epoch.value(),
                                         .actual = plan->binding.epoch.value()}));
  }
  TurnupAuthorization* authorization = rack.find_authorization(request.attempt);
  if (authorization == nullptr) {
    return reject(make_error(ErrorCode::TurnupNotAuthorized,
                             "no authorization with that attempt identity exists",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  if (authorization->state == AuthorizationState::Fenced) {
    return reject(make_error(ErrorCode::TurnupAuthorityFenced,
                             "the authorization was fenced and can never be used again",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  if (authorization->state != AuthorizationState::Active) {
    return reject(make_error(ErrorCode::TurnupNotAuthorized,
                             "the authorization is no longer active",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text(),
                                         .related = std::string(
                                             authorization_state_name(authorization->state))}));
  }
  if (request.observed_at <= authorization->issued_at) {
    return reject(make_error(ErrorCode::InvalidTimestamp,
                             "the activation observation must be newer than the authorization",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text(),
                                         .related = request.observed_at.to_text()}));
  }
  const Digest composition = request.observed_composition.is_zero()
                                 ? plan->binding.composition
                                 : request.observed_composition;
  if (!digests_equal(composition, plan->binding.composition)) {
    return reject(make_error(ErrorCode::CompositionDigestMismatch,
                             "the activation observation describes a different composition",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = composition.to_hex()}));
  }
  for (const ActivationRecord& existing : rack.activations) {
    if (!(existing.attempt == request.attempt)) {
      continue;
    }
    if (same_activation_claim(existing, request.outcome, request.active_members,
                              request.observed_at, composition)) {
      ActivationReceipt receipt;
      receipt.attempt = existing.attempt;
      receipt.plan = existing.plan;
      receipt.revision = existing.revision;
      receipt.sequence = existing.sequence;
      receipt.outcome = existing.outcome;
      return receipt;
    }
    return reject(make_error(ErrorCode::ActivationAttemptConflict,
                             "a different activation observation was already recorded",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  if (rack.activations.size() >= kMaxActivationRecordsPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack holds as many activation records as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = kMaxActivationRecordsPerRack,
                                         .actual = rack.activations.size()}));
  }
  if (working.observation_sequence.is_max() || plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded,
                             "the observation sequence or plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  const ObservationSequence sequence = working.observation_sequence.next();

  plan->revision = plan->revision.next();
  ActivationRecord record;
  record.attempt = request.attempt;
  record.rack = rack.rack.id;
  record.plan = plan->id;
  record.revision = plan->revision;
  record.authorization = authorization->authorization_digest;
  record.observed_composition = composition;
  record.sequence = sequence;
  record.observed_at = request.observed_at;
  record.validity = request.validity_from_policy ? plan->policy.default_evidence_validity
                                                 : request.validity;
  record.outcome = request.outcome;
  record.active_members = request.active_members;
  record.source = request.context.source;
  record.actor = request.context.actor;
  record.note = request.context.note;
  record.observed_state = digest_observed_state(request.outcome, request.active_members,
                                                request.observed_at, composition);
  record.activation_digest = record.compute_digest();
  working.observation_sequence = sequence;

  if (request.outcome == ActivationOutcome::Active) {
    if (rack.rack.lifecycle == RackLifecycleState::Authorized) {
      if (!lifecycle_allows(rack.rack.lifecycle, RackLifecycleState::Active)) {
        return reject(make_error(ErrorCode::LifecycleTransitionNotAllowed,
                                 "the rack cannot move to an active position",
                                 ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                             .subject = request.rack.text()}));
      }
      rack.rack.lifecycle = RackLifecycleState::Active;
    }
    authorization->state = AuthorizationState::Consumed;
  }
  const Status fenced = fence_active_authorizations(
      rack, ErrorCode::AuthoritySuperseded,
      "an activation observation advanced the plan after this authorization", request.context,
      request_digest);
  if (!fenced.has_value()) {
    return reject(fenced.error());
  }
  const ActivationOutcome recorded_outcome = record.outcome;
  const PlanId activation_plan = record.plan;
  const PlanRevision activation_revision = record.revision;
  rack.activations.push_back(record);

  append_provenance(rack, request.context, kOperation, plan->id, plan->revision, sequence,
                    request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan->id;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = plan->revision;
    receipt.sequence_after = sequence;
    receipt.attempt = request.attempt;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  ActivationReceipt receipt;
  receipt.attempt = request.attempt;
  receipt.plan = activation_plan;
  receipt.revision = activation_revision;
  receipt.sequence = sequence;
  receipt.outcome = recorded_outcome;
  return receipt;
}


Result<CommissionReceipt> TurnupService::commission(const CommissionRequest& request) {
  constexpr OperationKind kOperation = OperationKind::Commission;
  const Digest request_digest = digest_commission_request(request);
  const auto reject = [&](TurnupError error) -> Result<CommissionReceipt> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  for (const CommissionRecord& existing : rack.commissions) {
    if (existing.attempt == request.attempt) {
      CommissionReceipt receipt;
      receipt.attempt = existing.attempt;
      receipt.plan = existing.plan;
      receipt.revision = existing.revision;
      receipt.commissioned_at = existing.commissioned_at;
      receipt.replayed = true;
      return receipt;
    }
  }
  RackTurnupPlan* plan = rack.active_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  const TurnupAuthorization* authorization = rack.find_authorization(request.attempt);
  if (authorization == nullptr) {
    return reject(make_error(ErrorCode::TurnupNotAuthorized,
                             "no authorization with that attempt identity exists",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  if (authorization->state != AuthorizationState::Consumed) {
    return reject(make_error(ErrorCode::ActivationNotObserved,
                             "the attempt has no accepted post-action activation observation",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  const ActivationRecord* activation = nullptr;
  for (const ActivationRecord& candidate : rack.activations) {
    if (candidate.attempt == request.attempt) {
      activation = &candidate;
    }
  }
  if (activation == nullptr) {
    return reject(make_error(ErrorCode::ActivationNotObserved,
                             "the attempt has no activation observation",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text()}));
  }
  if (activation->outcome != ActivationOutcome::Active) {
    return reject(make_error(ErrorCode::ActivationNotConfirmed,
                             "the activation observation does not confirm an active rack",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.attempt.text(),
                                         .related = std::string(
                                             activation_outcome_name(activation->outcome))}));
  }
  if (rack.commissions.size() >= kMaxActivationRecordsPerRack) {
    return reject(make_error(ErrorCode::CapacityExhausted,
                             "the rack holds as many commission records as it supports",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!lifecycle_allows(rack.rack.lifecycle, RackLifecycleState::Commissioned)) {
    return reject(make_error(ErrorCode::LifecycleTransitionNotAllowed,
                             "the rack is not in a lifecycle position that can be commissioned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  if (plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }

  plan->revision = plan->revision.next();
  CommissionRecord record;
  record.attempt = request.attempt;
  record.plan = plan->id;
  record.revision = plan->revision;
  record.activation = activation->activation_digest;
  record.commissioned_at = request.context.authority_time;
  record.actor = request.context.actor;
  record.source = request.context.source;
  record.note = request.context.note;
  record.commission_digest = record.compute_digest();
  const PlanId commission_plan = record.plan;
  const PlanRevision commission_revision = record.revision;
  const WallClock commissioned_at = record.commissioned_at;
  rack.commissions.push_back(record);
  plan->state = PlanState::Completed;
  rack.rack.lifecycle = RackLifecycleState::Commissioned;

  append_provenance(rack, request.context, kOperation, plan->id, plan->revision,
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = commission_plan;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = commission_revision;
    receipt.sequence_after = working.observation_sequence;
    receipt.attempt = request.attempt;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  CommissionReceipt receipt;
  receipt.attempt = request.attempt;
  receipt.plan = commission_plan;
  receipt.revision = commission_revision;
  receipt.commissioned_at = commissioned_at;
  return receipt;
}

Result<RollbackReceipt> TurnupService::rollback(const RollbackRequest& request) {
  constexpr OperationKind kOperation = OperationKind::Rollback;
  const Digest request_digest = digest_rollback_request(request);
  const auto reject = [&](TurnupError error) -> Result<RollbackReceipt> {
    journal_rejection(request.context, kOperation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, kOperation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    RollbackReceipt receipt;
    receipt.revision = replay->revision_after;
    receipt.replayed = true;
    return receipt;
  }
  RackTurnupPlan* plan = rack.active_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  if (rack.rack.lifecycle != RackLifecycleState::Authorized &&
      rack.rack.lifecycle != RackLifecycleState::Active) {
    return reject(make_error(ErrorCode::RollbackNotAllowed,
                             "only an authorized or newly active rack can be rolled back",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(rack.rack.lifecycle))}));
  }
  if (rack.rack.lifecycle == RackLifecycleState::Active && !request.acknowledge_active) {
    return reject(make_error(ErrorCode::RollbackNotAllowed,
                             "the rack was observed active; acknowledge that to roll it back",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }
  if (plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                         .subject = request.rack.text()}));
  }

  std::size_t cancelled = 0;
  for (TurnupAuthorization& authorization : rack.authorizations) {
    if (authorization.is_active()) {
      authorization.state = AuthorizationState::Cancelled;
      ++cancelled;
    }
  }
  plan->revision = plan->revision.next();
  const PlanRevision rollback_revision = plan->revision;
  rack.rack.lifecycle = RackLifecycleState::Planned;
  append_provenance(rack, request.context, kOperation, plan->id, plan->revision,
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = kOperation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan->id;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = rollback_revision;
    receipt.sequence_after = working.observation_sequence;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, kOperation, request_digest, receipt);
  }
  const Result<Evaluation> evaluation = evaluate_rack_state(rack, request.context.authority_time);
  const TurnupVerdict verdict =
      evaluation.has_value() ? evaluation.value().verdict : TurnupVerdict::NotReady;

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  RollbackReceipt receipt;
  receipt.revision = rollback_revision;
  receipt.verdict = verdict;
  receipt.authorizations_fenced = cancelled;
  return receipt;
}

Result<LifecycleReceipt> TurnupService::lifecycle_transition(OperationKind operation,
                                                            const LifecycleRequest& request,
                                                            RackLifecycleState target,
                                                            ErrorCode wrong_state_code,
                                                            const char* wrong_state_message) {
  const Digest request_digest = digest_lifecycle_request(operation, request);
  const auto reject = [&](TurnupError error) -> Result<LifecycleReceipt> {
    journal_rejection(request.context, operation, request.rack, PlanId{}, request_digest, error);
    return error;
  };
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(operation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(operation));
  }
  ServiceState working = state_;
  const Result<RackState*> found = require_mutable_rack(working, request.rack);
  if (!found.has_value()) {
    return reject(found.error());
  }
  RackState& rack = *found.value();
  const RackLifecycleState previous = rack.rack.lifecycle;
  const IdempotencyRecord* replay = nullptr;
  const Result<bool> replayed =
      replay_or_conflict(working, request.rack, operation, request.context.request, request_digest,
                         replay);
  if (!replayed.has_value()) {
    return reject(replayed.error());
  }
  if (replayed.value()) {
    LifecycleReceipt receipt;
    receipt.to = target;
    receipt.from = target;
    receipt.revision = replay->revision_after;
    receipt.replayed = true;
    return receipt;
  }
  if (previous == target) {
    LifecycleReceipt receipt;
    receipt.from = previous;
    receipt.to = target;
    return receipt;
  }
  RackTurnupPlan* plan = rack.current_plan();
  if (plan == nullptr) {
    return reject(make_error(ErrorCode::PlanNotActive, "the rack has no active turnup plan",
                             ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                         .subject = request.rack.text()}));
  }
  if (!(plan->revision == request.expected_revision)) {
    return reject(make_error(ErrorCode::StalePlanRevision,
                             "the plan advanced since the request was planned",
                             ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                         .subject = request.rack.text(),
                                         .expected = plan->revision.value(),
                                         .actual = request.expected_revision.value()}));
  }
  if (!lifecycle_allows(previous, target)) {
    return reject(make_error(wrong_state_code, wrong_state_message,
                             ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                         .subject = request.rack.text(),
                                         .related = std::string(
                                             rack_lifecycle_state_name(previous))}));
  }
  if (plan->revision.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the plan revision is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(operation)),
                                         .subject = request.rack.text()}));
  }

  plan->revision = plan->revision.next();
  const PlanRevision revision_after = plan->revision;
  const Status fenced = fence_active_authorizations(
      rack, ErrorCode::AuthoritySuperseded,
      "the rack lifecycle advanced past the authorization", request.context, request_digest);
  if (!fenced.has_value()) {
    return reject(fenced.error());
  }
  rack.rack.lifecycle = target;
  append_provenance(rack, request.context, operation, plan->id, plan->revision,
                    working.observation_sequence, request_digest);
  if (!request.context.request.empty()) {
    IdempotencyRecord receipt;
    receipt.request = request.context.request;
    receipt.operation = operation;
    receipt.request_digest = request_digest;
    receipt.rack = request.rack;
    receipt.plan = plan->id;
    receipt.revision_before = request.expected_revision;
    receipt.revision_after = revision_after;
    receipt.sequence_after = working.observation_sequence;
    receipt.recorded_at = request.context.authority_time;
    receipt.actor = request.context.actor;
    receipt.response_digest = receipt.compute_digest();
    append_receipt(rack, request.context, operation, request_digest, receipt);
  }

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  LifecycleReceipt receipt;
  receipt.from = previous;
  receipt.to = target;
  receipt.revision = revision_after;
  return receipt;
}

Result<LifecycleReceipt> TurnupService::begin_drain(const LifecycleRequest& request) {
  constexpr OperationKind kOperation = OperationKind::BeginDrain;
  return lifecycle_transition(kOperation, request, RackLifecycleState::Draining,
                              ErrorCode::NotCommissioned,
                              "only a commissioned rack can begin draining");
}

Result<LifecycleReceipt> TurnupService::complete_drain(const LifecycleRequest& request) {
  constexpr OperationKind kOperation = OperationKind::CompleteDrain;
  return lifecycle_transition(kOperation, request, RackLifecycleState::Drained,
                              ErrorCode::LifecycleTransitionNotAllowed,
                              "only a draining rack can finish draining");
}

Result<LifecycleReceipt> TurnupService::decommission(const LifecycleRequest& request) {
  constexpr OperationKind kOperation = OperationKind::Decommission;
  return lifecycle_transition(kOperation, request, RackLifecycleState::Decommissioned,
                              ErrorCode::LifecycleTransitionNotAllowed,
                              "a rack must be drained before it can be decommissioned");
}

Result<TakeoverReport> TurnupService::take_control(const TakeControlRequest& request) {
  constexpr OperationKind kOperation = OperationKind::TakeControl;
  const Digest request_digest = digest_take_control_request(request);
  const auto reject = [&](TurnupError error) -> Result<TakeoverReport> {
    journal_rejection(request.context, kOperation, RackId{}, PlanId{}, request_digest, error);
    return error;
  };
  if (!open_) {
    return reject(make_error(ErrorCode::InvalidArgument, "the service is not open",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (!store_.is_writable()) {
    return reject(make_error(ErrorCode::ReadOnlyStore, "the service was opened read-only",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  if (request.context.actor.empty()) {
    return reject(missing_actor(kOperation));
  }
  ServiceState working = state_;
  if (working.control_epoch.is_max()) {
    return reject(make_error(ErrorCode::LimitExceeded, "the control epoch is exhausted",
                             ErrorDetail{.operation = std::string(operation_kind_name(kOperation))}));
  }
  const ControlEpoch previous = working.control_epoch;
  const ControlEpoch next = previous.next();
  std::size_t fenced = 0;
  std::size_t superseded = 0;

  for (RackState& rack : working.racks) {
    bool touched = false;
    for (TurnupAuthorization& authorization : rack.authorizations) {
      if (!authorization.is_active()) {
        continue;
      }
      if (rack.fences.size() >= kMaxFencesPerRack) {
        return reject(make_error(ErrorCode::CapacityExhausted,
                                 "the rack has no room left for another authorization fence",
                                 ErrorDetail{.operation = std::string(operation_kind_name(kOperation)),
                                             .subject = rack.rack.id.text()}));
      }
      authorization.state = AuthorizationState::Fenced;
      AuthorizationFence fence;
      fence.attempt = authorization.attempt;
      fence.reason = ErrorCode::AuthoritySuperseded;
      fence.explanation = "control authority was taken over";
      fence.sequence = working.observation_sequence;
      fence.fenced_at = request.context.authority_time;
      fence.actor = request.context.actor;
      fence.request = request_digest;
      rack.fences.push_back(std::move(fence));
      ++fenced;
      touched = true;
    }
    for (RackTurnupPlan& plan : rack.plans) {
      if (plan.state == PlanState::Active) {
        plan.state = PlanState::Superseded;
        ++superseded;
        touched = true;
      }
    }
    if (rack.rack.lifecycle == RackLifecycleState::Authorized) {
      rack.rack.lifecycle = RackLifecycleState::Planned;
      touched = true;
    }
    if (touched) {
      append_provenance(rack, request.context, kOperation, PlanId{}, PlanRevision{},
                        working.observation_sequence, request_digest);
    }
  }
  working.control_epoch = next;

  const Status committed = commit_working(working, request.context.authority_time);
  if (!committed.has_value()) {
    return reject(committed.error());
  }
  TakeoverReport report;
  report.previous = previous;
  report.current = next;
  report.authorizations_fenced = fenced;
  static_cast<void>(superseded);
  return report;
}

Result<Evaluation> TurnupService::evaluate(const RackId& rack, WallClock authority_time) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  return evaluate_rack_state(*found.value(), authority_time);
}

Result<BlockerReport> TurnupService::blockers(const RackId& rack, WallClock authority_time) const {
  const Result<Evaluation> evaluation = evaluate(rack, authority_time);
  if (!evaluation.has_value()) {
    return evaluation.error();
  }
  return evaluation.value().to_report();
}

Result<RackSummary> TurnupService::summary(const RackId& rack, WallClock authority_time) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  const RackState& state = *found.value();
  RackSummary summary;
  summary.id = state.rack.id;
  summary.label = state.rack.label;
  summary.site = state.rack.site;
  summary.generation = state.rack.generation;
  summary.composition_generation = state.rack.composition_generation();
  summary.composition = state.rack.composition_digest();
  summary.lifecycle = state.rack.lifecycle;
  summary.member_count = state.rack.composition.size();
  summary.registered_at = state.rack.registered_at;
  summary.authority_time = authority_time;
  const RackTurnupPlan* plan = state.current_plan();
  if (plan != nullptr) {
    summary.has_plan = true;
    summary.plan = plan->id;
    summary.lifetime = plan->lifetime;
    summary.revision = plan->revision;
    summary.plan_state = plan->state;
  }
  const Result<Evaluation> evaluation = evaluate_rack_state(state, authority_time);
  if (evaluation.has_value()) {
    summary.verdict = evaluation.value().verdict;
    summary.blocker_count = evaluation.value().blockers.size();
    summary.eligible_evidence = evaluation.value().eligible_evidence;
  } else {
    summary.verdict = TurnupVerdict::NotReady;
    summary.blocker_count = 0;
  }
  return summary;
}

Result<ServiceSummary> TurnupService::summary(WallClock authority_time) const {
  ServiceSummary summary;
  summary.rack_count = state_.racks.size();
  for (const RackState& rack : state_.racks) {
    switch (rack.rack.lifecycle) {
      case RackLifecycleState::Registered:
        ++summary.registered;
        break;
      case RackLifecycleState::Planned:
        ++summary.planned;
        break;
      case RackLifecycleState::Authorized:
        ++summary.authorized;
        break;
      case RackLifecycleState::Active:
        ++summary.active;
        break;
      case RackLifecycleState::Commissioned:
        ++summary.commissioned;
        break;
      case RackLifecycleState::Draining:
        ++summary.draining;
        break;
      case RackLifecycleState::Drained:
        ++summary.drained;
        break;
      case RackLifecycleState::Decommissioned:
        ++summary.decommissioned;
        break;
    }
    const Result<Evaluation> evaluation = evaluate_rack_state(rack, authority_time);
    if (evaluation.has_value() && evaluation.value().verdict == TurnupVerdict::Blocked) {
      ++summary.blocked;
    }
  }
  summary.store_epoch = state_.store_epoch;
  summary.store_sequence = state_.store_sequence;
  summary.incarnation = state_.incarnation;
  summary.control_epoch = state_.control_epoch;
  summary.observation_sequence = state_.observation_sequence;
  summary.created_at = state_.created_at;
  summary.updated_at = state_.updated_at;
  summary.authority_time = authority_time;
  summary.rejection_journal = state_.rejections.size();
  return summary;
}

Result<std::vector<EvidenceRecord>> TurnupService::evidence(const RackId& rack) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  return found.value()->evidence;
}

Result<std::vector<TurnupAuthorization>> TurnupService::authorizations(const RackId& rack) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  return found.value()->authorizations;
}

Result<std::vector<AuthorizationFence>> TurnupService::fences(const RackId& rack) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  return found.value()->fences;
}

Result<std::vector<ProvenanceRecord>> TurnupService::provenance(const RackId& rack) const {
  const Result<const RackState*> found = require_rack(rack);
  if (!found.has_value()) {
    return found.error();
  }
  return found.value()->provenance;
}

Result<std::vector<RejectionRecord>> TurnupService::rejections() const { return state_.rejections; }

Result<std::vector<RackId>> TurnupService::rack_ids() const {
  std::vector<RackId> ids;
  ids.reserve(state_.racks.size());
  for (const RackState& rack : state_.racks) {
    ids.push_back(rack.rack.id);
  }
  return ids;
}

}  // namespace rackturnup




