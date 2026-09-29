// Rack Turnup Manager - documented resource bounds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>

#include "rack_turnup/export.hpp"

namespace rackturnup {

// Every bound below is enforced before allocation or mutation, on both the
// in-memory path and the decoding path, so that neither a hostile caller nor a
// hostile state file can drive unbounded growth. The values are part of the
// documented contract and are asserted at and one past their limit by the test
// suite.

// Maximum racks managed by one TurnupService (one service owns one facility
// scope). A rack is a plan container, not a device count.
inline constexpr std::size_t kMaxRacks = 2048;

// Maximum plans retained for one rack. Plans are revisions of an intent to turn
// a rack up; retaining history is bounded and oldest-first eviction is explicit.
inline constexpr std::size_t kMaxPlansPerRack = 256;

// Maximum composition members in one rack. This is the count of installed
// devices the rack declares as its own composition.
inline constexpr std::size_t kMaxCompositionMembers = 2048;

// Maximum inventory entries (expected or observed) in one hardware inventory.
inline constexpr std::size_t kMaxInventoryEntries = 4096;

// Maximum entries in one observation collection that is not an inventory, such
// as health samples or compatibility memberships.
inline constexpr std::size_t kMaxObservationEntries = 4096;

// Maximum compatibility requirements held by one plan.
inline constexpr std::size_t kMaxCompatibilityRequirements = 512;

// Maximum traits in one trait set (a baseline or a device's declared traits).
inline constexpr std::size_t kMaxTraitsPerSet = 256;

// Maximum evidence records retained per rack. Evidence is generation stamped
// and freshness bounded; the newest record for a (subsystem, scope) pair is the
// only one that may contribute to readiness, but superseded records are kept
// until the bound is reached so that operators can see what was replaced.
inline constexpr std::size_t kMaxEvidenceRecordsPerRack = 1024;

// Maximum stages in one turnup plan. The stage ladder is fixed by the domain,
// so this bound is a consistency check on decoded plans rather than a tuning
// knob.
inline constexpr std::size_t kMaxPlanStages = 16;

// Maximum blockers retained in one blocker report before aggregation stops
// appending and starts counting suppressed entries.
inline constexpr std::size_t kMaxBlockersPerReport = 512;

// Maximum recorded idempotency receipts kept per rack. The oldest receipt is
// evicted first and the eviction count is preserved and reported, so replay
// coverage is visibly bounded rather than silently incomplete.
inline constexpr std::size_t kMaxIdempotencyRecordsPerRack = 128;

// Maximum activation records retained per rack.
inline constexpr std::size_t kMaxActivationRecordsPerRack = 64;

// Maximum provenance records retained per rack.
inline constexpr std::size_t kMaxProvenanceRecordsPerRack = 512;

// Maximum rejection explanations retained by one service for inspection.
inline constexpr std::size_t kMaxRejectionJournalEntries = 512;

// Maximum authorization fence entries retained per rack. Every fence that was
// ever created is remembered until this bound is reached; a fenced
// authorization can never be un-fenced.
inline constexpr std::size_t kMaxFencesPerRack = 256;

// Text bounds, in bytes, of externally supplied strings.
inline constexpr std::size_t kMaxIdentityTextBytes = 192;
inline constexpr std::size_t kMaxOpaqueReferenceBytes = 160;
inline constexpr std::size_t kMaxTraitBytes = 48;
inline constexpr std::size_t kMaxActorBytes = 64;
inline constexpr std::size_t kMaxRequestIdBytes = 64;
inline constexpr std::size_t kMaxSourceReferenceBytes = 160;
inline constexpr std::size_t kMaxLabelBytes = 128;
inline constexpr std::size_t kMaxNoteBytes = 240;

// Maximum duration bounds. Freshness windows and observation ages are bounded
// so that a decoded or supplied duration cannot overflow time arithmetic.
inline constexpr std::int64_t kMaxDurationMilliseconds = 365ll * 24ll * 60ll * 60ll * 1000ll;

// Maximum byte length of one encoded state file a reader will accept. The
// declared payload length is checked against this bound before any buffer is
// reserved.
inline constexpr std::uint64_t kMaxStateFileBytes = 512ull * 1024ull * 1024ull;

// Maximum accepted element count in one decoded collection header. Decoding
// rejects a declared count above the matching bound before growing a container.
inline constexpr std::uint32_t kMaxDecodedCollectionCount = 1u << 20;

// Maximum length of one path supplied to the durable store, in bytes.
inline constexpr std::size_t kMaxPathBytes = 4096;

}  // namespace rackturnup
