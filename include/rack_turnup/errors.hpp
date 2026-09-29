// Rack Turnup Manager - stable machine-readable error taxonomy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/export.hpp"

namespace rackturnup {

// Error codes are a stable, machine-readable contract. Numeric values are
// grouped by fault domain and must never be reused for a different meaning.
// New codes are appended inside the matching group.
//
// Validation precedence is fixed and independent of incidental ordering: a
// request is resolved against the plan identity and revision first, then the
// request's own content, then the plan's current state, then the evidence
// binding, and only then the turnup gates. The same invalid request therefore
// resolves to the same primary error code regardless of map ordering, thread
// scheduling or unrelated state.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // 1xx - input rejected at a construction or validation boundary.
  InvalidArgument = 100,
  MalformedIdentity = 101,
  IdentityTooLong = 102,
  InvalidCharacter = 103,
  InvalidEnumValue = 104,
  InvalidRange = 105,
  EmptyValue = 106,
  InvalidUtf8 = 107,
  InvalidTimestamp = 108,
  InvalidDuration = 109,
  InvalidDigest = 110,
  InvalidFlags = 111,
  InvalidCoordinate = 112,

  // 2xx - identity uniqueness and existence.
  DuplicateRackId = 200,
  UnknownRackId = 201,
  DuplicatePlanId = 202,
  UnknownPlanId = 203,
  DuplicateEvidenceId = 204,
  UnknownEvidenceId = 205,
  DuplicateDeviceId = 206,
  DuplicateAttemptId = 207,
  RequestIdConflict = 208,
  DuplicateCompatibilityRequirement = 209,
  DuplicateInventoryEntry = 210,

  // 3xx - stale authority, generations and epochs.
  StaleRackGeneration = 300,
  StaleCompositionGeneration = 301,
  StalePlanRevision = 302,
  StaleAuthorityEpoch = 303,
  StaleTopologyGeneration = 304,
  StalePowerGeneration = 305,
  StaleCoolingGeneration = 306,
  StaleNetworkGeneration = 307,
  StalePolicyGeneration = 308,
  StaleDependencyGeneration = 309,
  StaleCapacityGeneration = 310,
  StaleMaintenanceGeneration = 311,
  StaleHealthGeneration = 312,
  StaleFirmwareGeneration = 313,
  StaleObservedGeneration = 314,
  CompositionDigestMismatch = 315,
  EvidenceBindingMismatch = 316,
  TurnupAuthorityFenced = 317,
  SequenceRegression = 318,
  SnapshotIdentityMismatch = 319,
  StaleWriterFenced = 320,
  StoreEpochRegression = 321,
  UnknownCompositionMember = 322,
  AuthoritySuperseded = 323,

  // 4xx - lifecycle gates and stage ladder.
  StageOutOfOrder = 400,
  LifecycleTransitionNotAllowed = 401,
  TurnupAlreadyAuthorized = 402,
  TurnupNotAuthorized = 403,
  TurnupNotReady = 404,
  ActivationNotObserved = 405,
  AlreadyCommissioned = 406,
  NotCommissioned = 407,
  PlanNotActive = 408,
  RollbackNotAllowed = 409,
  ActivationAttemptConflict = 410,
  ActivationNotConfirmed = 412,

  // 5xx - composition, inventory and readiness.
  CompositionEmpty = 500,
  CompositionGenerationRegression = 501,
  InventoryGap = 502,
  DuplicateInventorySlot = 503,
  ReadinessUnknown = 504,
  ReadinessBlocked = 505,
  EvidenceMissing = 506,
  EvidenceStale = 507,
  EvidenceSuperseded = 508,
  HeadroomInsufficient = 509,
  HealthFailed = 510,
  UnhealthyDevice = 511,
  InventoryAmbiguous = 512,
  HealthDegraded = 513,
  InventoryUnreadable = 514,

  // 6xx - compatibility.
  CompatibilityUnsatisfied = 600,
  UnknownCompatibilityProfile = 601,
  MalformedCompatibilityProfile = 602,
  FirmwareBaselineMismatch = 603,
  TraitUnknown = 604,
  CompatibilityAdvisory = 605,

  // 7xx - persistence, encoding and integrity.
  IoFailure = 700,
  IntegrityCheckFailed = 701,
  UnsupportedFormatVersion = 702,
  TruncatedState = 703,
  CorruptState = 704,
  StateTooLarge = 705,
  NoAuthoritativeState = 706,
  InvalidStateEncoding = 707,
  ReservedBitsSet = 708,
  TrailingBytes = 709,
  MalformedFieldOrder = 710,
  StaleDurableState = 711,

  // 8xx - bounds and resource closure.
  LimitExceeded = 800,
  SnapshotInvalid = 801,
  CapacityExhausted = 802,

  // 9xx - writer ownership and fencing.
  WriterLockHeld = 900,
  WriterLockInvalid = 901,
  ReadOnlyStore = 902,
  LockIoFailure = 903,
  WriterIdentityMismatch = 904,
  PublicationFailed = 905,
};

// Broad category of a code, so callers can branch on fault class without
// enumerating every code.
enum class ErrorCategory : std::uint8_t {
  None = 0,
  Input = 1,
  Identity = 2,
  Authority = 3,
  Lifecycle = 4,
  Readiness = 5,
  Compatibility = 6,
  Persistence = 7,
  Limits = 8,
  Writer = 9,
};

// Machine-readable structured context for a rejection. Fields that do not apply
// are empty. `items` carries the ordered, deduplicated list relevant to the
// rejection, for example the missing compatibility requirements or the stages
// that are not ready.
struct ErrorDetail {
  std::string operation;
  std::string subject;
  std::string related;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
  std::vector<std::string> items;
};

struct TurnupError {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  ErrorDetail detail;
};

[[nodiscard]] RACK_TURNUP_API ErrorCategory category_of(ErrorCode code) noexcept;

// Stable identifier for a code, for example "turnup_not_ready".
[[nodiscard]] RACK_TURNUP_API std::string_view code_name(ErrorCode code) noexcept;

// One sentence explaining what the code means, independent of any particular
// occurrence.
[[nodiscard]] RACK_TURNUP_API std::string_view explain(ErrorCode code) noexcept;

[[nodiscard]] RACK_TURNUP_API std::uint16_t code_value(ErrorCode code) noexcept;

// Renders "code_name(code_value): message" plus any structured context.
[[nodiscard]] RACK_TURNUP_API std::string describe(const TurnupError& error);

[[nodiscard]] RACK_TURNUP_API TurnupError make_error(ErrorCode code,
                                                     std::string message,
                                                     ErrorDetail detail = {});

// True when the code reports a stale-authority condition. Callers that
// implement lost-response replay need this to distinguish "my plan was
// superseded" from "my request was malformed".
[[nodiscard]] RACK_TURNUP_API bool is_staleness_code(ErrorCode code) noexcept;

// True when the code reports that a rack is not (yet) proven ready.
[[nodiscard]] RACK_TURNUP_API bool is_readiness_code(ErrorCode code) noexcept;

}  // namespace rackturnup
