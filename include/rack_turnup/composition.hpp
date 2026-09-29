// Rack Turnup Manager - rack identity, composition and inventory closure.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/errors.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/limits.hpp"
#include "rack_turnup/result.hpp"

namespace rackturnup {

// Physical position of a device in a rack frame. A rack unit is one-based and
// a slot is zero-based; both are bounded so that a decoded coordinate can never
// overflow an offset computation.
inline constexpr std::uint32_t kMaxRackUnit = 512;
inline constexpr std::uint32_t kMaxSlotIndex = 255;

struct SlotCoordinate {
  std::uint32_t unit = 0;
  std::uint32_t slot = 0;

  [[nodiscard]] bool is_valid() const noexcept {
    return unit >= 1 && unit <= kMaxRackUnit && slot <= kMaxSlotIndex;
  }
  [[nodiscard]] bool operator==(const SlotCoordinate& other) const noexcept {
    return unit == other.unit && slot == other.slot;
  }
  [[nodiscard]] bool operator!=(const SlotCoordinate& other) const noexcept {
    return !(*this == other);
  }
  // Ordering is by rack unit and then by slot.
  [[nodiscard]] bool operator<(const SlotCoordinate& other) const noexcept {
    return (unit != other.unit) ? (unit < other.unit) : (slot < other.slot);
  }
  // Canonical text form, "u<unit>.s<slot>".
  [[nodiscard]] std::string to_text() const;
};

[[nodiscard]] RACK_TURNUP_API Result<SlotCoordinate> make_slot_coordinate(std::uint64_t unit,
                                                                          std::uint64_t slot);
[[nodiscard]] RACK_TURNUP_API Result<SlotCoordinate> parse_slot_coordinate(std::string_view text);

// A canonical set of compatibility traits. The set is kept sorted by the
// canonical text of its traits, so two sets with the same members compare equal
// regardless of the order they were supplied in. A repeated trait is rejected
// rather than silently collapsed: the caller stated the same fact twice, which
// means the caller and this library disagree about the input.
class TraitSet final {
 public:
  TraitSet() = default;

  [[nodiscard]] static Result<TraitSet> create(std::vector<Trait> traits);

  [[nodiscard]] const std::vector<Trait>& traits() const noexcept { return traits_; }
  [[nodiscard]] std::size_t size() const noexcept { return traits_.size(); }
  [[nodiscard]] bool empty() const noexcept { return traits_.empty(); }
  [[nodiscard]] bool contains(const Trait& trait) const noexcept;

  [[nodiscard]] bool operator==(const TraitSet& other) const noexcept {
    return traits_ == other.traits_;
  }
  [[nodiscard]] bool operator!=(const TraitSet& other) const noexcept {
    return !(*this == other);
  }

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] std::string to_text(char delimiter) const;

 private:
  std::vector<Trait> traits_;
};

// One device the rack declares as part of its own composition.
struct CompositionMember {
  DeviceId device;
  AssetId asset;
  FirmwareBaselineId firmware_baseline;
  TraitSet traits;
  SlotCoordinate slot;
  DisplayLabel label;

  void update_digest(Sha256& hasher) const noexcept;
};

// The exact membership a plan is bound to. The composition digest is computed
// over a canonical preimage whose layout is versioned by
// kCompositionDigestModel, so a membership change always changes the digest and
// therefore fences any authority issued against the previous composition.
class RackComposition final {
 public:
  RackComposition() = default;

  [[nodiscard]] static Result<RackComposition> create(RackId rack, SiteId site,
                                                     CompositionGeneration generation,
                                                     std::vector<CompositionMember> members);

  [[nodiscard]] const RackId& rack() const noexcept { return rack_; }
  [[nodiscard]] const SiteId& site() const noexcept { return site_; }
  [[nodiscard]] CompositionGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] const std::vector<CompositionMember>& members() const noexcept { return members_; }
  [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
  [[nodiscard]] bool empty() const noexcept { return members_.empty(); }
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }

  // Members are sorted by device identity, so this is a binary search.
  [[nodiscard]] const CompositionMember* find(const DeviceId& device) const noexcept;

  // Recomputes the digest over the canonical preimage. Used by the decoder to
  // prove that a decoded composition is the one its digest claims.
  [[nodiscard]] Digest compute_digest() const;

  [[nodiscard]] std::vector<std::string> device_list() const;

 private:
  RackId rack_;
  SiteId site_;
  CompositionGeneration generation_;
  std::vector<CompositionMember> members_;
  Digest digest_;
};

// One observed inventory position. `readable` is false when the position
// exists but its contents could not be read, which is not the same as an empty
// position and never counts as closure.
struct InventoryEntry {
  DeviceId device;
  AssetId asset;
  FirmwareBaselineId firmware_baseline;
  TraitSet traits;
  SlotCoordinate slot;
  bool readable = true;

  void update_digest(Sha256& hasher) const noexcept;
};

struct InventoryObservation {
  // Sorted by device identity and free of duplicates when the observation is
  // created through `create_inventory_observation`.
  std::vector<InventoryEntry> entries;
  std::uint32_t unreadable_positions = 0;

  void update_digest(Sha256& hasher) const noexcept;
  [[nodiscard]] bool is_empty() const noexcept {
    return entries.empty() && unreadable_positions == 0;
  }
};

[[nodiscard]] RACK_TURNUP_API Result<InventoryObservation> create_inventory_observation(
    std::vector<InventoryEntry> entries, std::uint32_t unreadable_positions);

// Outcome of closing an inventory against a composition.
enum class InventoryClosureState : std::uint8_t {
  Closed = 1,
  Gap = 2,
  Ambiguous = 3,
};

[[nodiscard]] RACK_TURNUP_API std::string_view inventory_closure_state_name(
    InventoryClosureState state) noexcept;

struct InventoryClosure {
  InventoryClosureState state = InventoryClosureState::Gap;
  ErrorCode code = ErrorCode::Ok;
  ExpectedDeviceCount expected_members;
  ObservedDeviceCount observed_members;
  std::vector<std::string> missing_devices;
  std::vector<std::string> unknown_devices;
  std::vector<std::string> unreadable_devices;
  std::vector<std::string> displaced_devices;
  std::vector<std::string> duplicate_slots;
  // Positions that were inspected but produced no readable contents.
  std::uint32_t unreadable_positions = 0;

  [[nodiscard]] bool is_closed() const noexcept { return state == InventoryClosureState::Closed; }
  // Deterministic one-sentence explanation of the closure outcome.
  [[nodiscard]] std::string explain() const;
};

// Closes an inventory observation against a composition. Structural defects in
// the observation itself (a repeated device entry) are rejected as errors;
// every finding about the rack is reported in the closure so that the caller
// sees the same answer the evaluation will report.
[[nodiscard]] RACK_TURNUP_API Result<InventoryClosure> close_inventory(
    const RackComposition& composition, const InventoryObservation& observation);

}  // namespace rackturnup
