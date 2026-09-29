// Rack Turnup Manager - composition digests and inventory closure.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/composition.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "rack_turnup/text.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

template <typename Counter>
[[nodiscard]] Counter counter_from(std::size_t value) {
  return Counter::create(static_cast<std::uint64_t>(value)).value();
}

}  // namespace

std::string SlotCoordinate::to_text() const {
  std::string out = "u";
  out.append(std::to_string(unit));
  out.append(".s");
  out.append(std::to_string(slot));
  return out;
}

Result<SlotCoordinate> make_slot_coordinate(std::uint64_t unit, std::uint64_t slot) {
  if (unit < 1 || unit > kMaxRackUnit) {
    return make_error(ErrorCode::InvalidCoordinate, "rack unit is outside the supported range",
                      ErrorDetail{.operation = "make_slot_coordinate",
                                  .expected = kMaxRackUnit,
                                  .actual = unit});
  }
  if (slot > kMaxSlotIndex) {
    return make_error(ErrorCode::InvalidCoordinate, "slot index is outside the supported range",
                      ErrorDetail{.operation = "make_slot_coordinate",
                                  .expected = kMaxSlotIndex,
                                  .actual = slot});
  }
  SlotCoordinate coordinate;
  coordinate.unit = static_cast<std::uint32_t>(unit);
  coordinate.slot = static_cast<std::uint32_t>(slot);
  return coordinate;
}

Result<SlotCoordinate> parse_slot_coordinate(std::string_view text) {
  const auto reject = [&](std::string message) {
    return make_error(ErrorCode::InvalidCoordinate, std::move(message),
                      ErrorDetail{.operation = "parse_slot_coordinate",
                                  .subject = std::string(text)});
  };
  if (text.size() < 4 || text[0] != 'u') {
    return reject("coordinate must be written as u<unit>.s<slot>");
  }
  const std::size_t separator = text.find(".s");
  if (separator == std::string_view::npos || separator < 2) {
    return reject("coordinate must contain exactly one .s separator");
  }
  std::uint64_t unit = 0;
  std::uint64_t slot = 0;
  if (!parse_u64(text.substr(1, separator - 1), unit) ||
      !parse_u64(text.substr(separator + 2), slot)) {
    return reject("coordinate parts must be decimal integers");
  }
  return make_slot_coordinate(unit, slot);
}

Result<TraitSet> TraitSet::create(std::vector<Trait> traits) {
  if (traits.size() > kMaxTraitsPerSet) {
    return make_error(ErrorCode::LimitExceeded, "trait set exceeds the documented bound",
                      ErrorDetail{.operation = "TraitSet::create",
                                  .expected = kMaxTraitsPerSet,
                                  .actual = traits.size()});
  }
  std::sort(traits.begin(), traits.end(), [](const Trait& left, const Trait& right) {
    return byte_less(left.text(), right.text());
  });
  for (std::size_t i = 1; i < traits.size(); ++i) {
    if (traits[i - 1].text() == traits[i].text()) {
      return make_error(ErrorCode::InvalidArgument, "trait set contains a repeated trait",
                        ErrorDetail{.operation = "TraitSet::create",
                                    .subject = traits[i].text()});
    }
  }
  TraitSet set;
  set.traits_ = std::move(traits);
  return set;
}

bool TraitSet::contains(const Trait& trait) const noexcept {
  std::size_t low = 0;
  std::size_t high = traits_.size();
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (traits_[middle].text() == trait.text()) {
      return true;
    }
    if (byte_less(traits_[middle].text(), trait.text())) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return false;
}

void TraitSet::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(static_cast<std::uint32_t>(traits_.size()));
  for (const Trait& trait : traits_) {
    hasher.update_len_text(trait.text());
  }
}

std::string TraitSet::to_text(char delimiter) const {
  std::vector<std::string> parts;
  parts.reserve(traits_.size());
  for (const Trait& trait : traits_) {
    parts.push_back(trait.text());
  }
  return join(parts, delimiter);
}

void CompositionMember::update_digest(Sha256& hasher) const noexcept {
  hasher.update_len_text(device.text());
  hasher.update_len_text(asset.text());
  hasher.update_len_text(firmware_baseline.text());
  traits.update_digest(hasher);
  hasher.update_u32(slot.unit);
  hasher.update_u32(slot.slot);
  hasher.update_len_text(label.text());
}

Result<RackComposition> RackComposition::create(RackId rack, SiteId site,
                                                CompositionGeneration generation,
                                                std::vector<CompositionMember> members) {
  if (rack.empty()) {
    return make_error(ErrorCode::EmptyValue, "a composition must name its rack",
                      ErrorDetail{.operation = "RackComposition::create"});
  }
  if (members.empty()) {
    return make_error(ErrorCode::CompositionEmpty,
                      "a composition must contain at least one member",
                      ErrorDetail{.operation = "RackComposition::create",
                                  .subject = rack.text()});
  }
  if (members.size() > kMaxCompositionMembers) {
    return make_error(ErrorCode::LimitExceeded, "composition exceeds the documented bound",
                      ErrorDetail{.operation = "RackComposition::create",
                                  .expected = kMaxCompositionMembers,
                                  .actual = members.size()});
  }

  // Members are ordered by device identity before anything else is checked, so
  // the reported defect does not depend on the order the caller supplied.
  std::sort(members.begin(), members.end(),
            [](const CompositionMember& left, const CompositionMember& right) {
              return byte_less(left.device.text(), right.device.text());
            });

  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].device.empty()) {
      return make_error(ErrorCode::EmptyValue, "a composition member must name its device",
                        ErrorDetail{.operation = "RackComposition::create",
                                    .subject = rack.text()});
    }
    if (i > 0 && members[i - 1].device == members[i].device) {
      return make_error(ErrorCode::DuplicateDeviceId,
                        "a composition lists the same device more than once",
                        ErrorDetail{.operation = "RackComposition::create",
                                    .subject = members[i].device.text()});
    }
  }

  for (const CompositionMember& member : members) {
    if (!member.slot.is_valid()) {
      return make_error(ErrorCode::InvalidCoordinate,
                        "a composition member has an invalid slot coordinate",
                        ErrorDetail{.operation = "RackComposition::create",
                                    .subject = member.device.text(),
                                    .related = member.slot.to_text()});
    }
  }

  std::vector<SlotCoordinate> slots;
  slots.reserve(members.size());
  for (const CompositionMember& member : members) {
    slots.push_back(member.slot);
  }
  std::sort(slots.begin(), slots.end());
  for (std::size_t i = 1; i < slots.size(); ++i) {
    if (slots[i - 1] == slots[i]) {
      return make_error(ErrorCode::InvalidCoordinate,
                        "two composition members occupy the same slot",
                        ErrorDetail{.operation = "RackComposition::create",
                                    .subject = slots[i].to_text()});
    }
  }

  RackComposition composition;
  composition.rack_ = std::move(rack);
  composition.site_ = std::move(site);
  composition.generation_ = generation;
  composition.members_ = std::move(members);
  composition.digest_ = composition.compute_digest();
  return composition;
}

Digest RackComposition::compute_digest() const {
  Sha256 hasher;
  hasher.update_u32(kCompositionDigestModel);
  hasher.update_len_text(rack_.text());
  hasher.update_len_text(site_.text());
  hasher.update_u64(generation_.value());
  hasher.update_u32(static_cast<std::uint32_t>(members_.size()));
  for (const CompositionMember& member : members_) {
    member.update_digest(hasher);
  }
  return hasher.finish();
}

const CompositionMember* RackComposition::find(const DeviceId& device) const noexcept {
  std::size_t low = 0;
  std::size_t high = members_.size();
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (members_[middle].device == device) {
      return &members_[middle];
    }
    if (byte_less(members_[middle].device.text(), device.text())) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return nullptr;
}

std::vector<std::string> RackComposition::device_list() const {
  std::vector<std::string> devices;
  devices.reserve(members_.size());
  for (const CompositionMember& member : members_) {
    devices.push_back(member.device.text());
  }
  return devices;
}

void InventoryEntry::update_digest(Sha256& hasher) const noexcept {
  hasher.update_len_text(device.text());
  hasher.update_len_text(asset.text());
  hasher.update_len_text(firmware_baseline.text());
  traits.update_digest(hasher);
  hasher.update_u32(slot.unit);
  hasher.update_u32(slot.slot);
  hasher.update_byte(readable ? 1u : 0u);
}

void InventoryObservation::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(static_cast<std::uint32_t>(entries.size()));
  for (const InventoryEntry& entry : entries) {
    entry.update_digest(hasher);
  }
  hasher.update_u32(unreadable_positions);
}

Result<InventoryObservation> create_inventory_observation(std::vector<InventoryEntry> entries,
                                                          std::uint32_t unreadable_positions) {
  if (entries.size() > kMaxInventoryEntries) {
    return make_error(ErrorCode::LimitExceeded, "inventory exceeds the documented bound",
                      ErrorDetail{.operation = "create_inventory_observation",
                                  .expected = kMaxInventoryEntries,
                                  .actual = entries.size()});
  }
  std::sort(entries.begin(), entries.end(),
            [](const InventoryEntry& left, const InventoryEntry& right) {
              return byte_less(left.device.text(), right.device.text());
            });
  for (std::size_t i = 1; i < entries.size(); ++i) {
    if (entries[i - 1].device == entries[i].device) {
      return make_error(ErrorCode::DuplicateInventoryEntry,
                        "inventory reports the same device more than once",
                        ErrorDetail{.operation = "create_inventory_observation",
                                    .subject = entries[i].device.text()});
    }
  }
  for (const InventoryEntry& entry : entries) {
    if (entry.device.empty()) {
      return make_error(ErrorCode::EmptyValue, "an inventory entry must name its device",
                        ErrorDetail{.operation = "create_inventory_observation"});
    }
    if (!entry.slot.is_valid()) {
      return make_error(ErrorCode::InvalidCoordinate,
                        "an inventory entry has an invalid slot coordinate",
                        ErrorDetail{.operation = "create_inventory_observation",
                                    .subject = entry.device.text(),
                                    .related = entry.slot.to_text()});
    }
  }
  InventoryObservation observation;
  observation.entries = std::move(entries);
  observation.unreadable_positions = unreadable_positions;
  return observation;
}

std::string_view inventory_closure_state_name(InventoryClosureState state) noexcept {
  switch (state) {
    case InventoryClosureState::Closed:
      return "closed";
    case InventoryClosureState::Gap:
      return "gap";
    case InventoryClosureState::Ambiguous:
      return "ambiguous";
  }
  return "gap";
}

std::string InventoryClosure::explain() const {
  std::string out = "inventory ";
  out.append(inventory_closure_state_name(state));
  out.append(": ");
  out.append(std::to_string(observed_members.value()));
  out.append(" of ");
  out.append(std::to_string(expected_members.value()));
  out.append(" members observed");
  const auto append_items = [&out](std::string_view label,
                                   const std::vector<std::string>& items) {
    if (items.empty()) {
      return;
    }
    out.append("; ");
    out.append(label);
    out.push_back('=');
    out.append(std::to_string(items.size()));
    out.append(" [");
    bool first = true;
    for (const std::string& item : items) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      out.append(item);
    }
    out.push_back(']');
  };
  append_items("missing", missing_devices);
  append_items("unknown", unknown_devices);
  append_items("unreadable", unreadable_devices);
  append_items("displaced", displaced_devices);
  append_items("duplicate_slots", duplicate_slots);
  if (unreadable_positions != 0) {
    out.append("; unreadable_positions=");
    out.append(std::to_string(unreadable_positions));
  }
  return out;
}

Result<InventoryClosure> close_inventory(const RackComposition& composition,
                                         const InventoryObservation& observation) {
  InventoryClosure closure;
  closure.expected_members = counter_from<ExpectedDeviceCount>(composition.size());

  // Both collections are ordered by device identity, so closure is a single
  // merge pass: an entry that sorts before the member it is compared against is
  // a device the composition does not contain.
  std::size_t observed = 0;
  std::size_t index = 0;
  for (const CompositionMember& member : composition.members()) {
    while (index < observation.entries.size() &&
           byte_less(observation.entries[index].device.text(), member.device.text())) {
      closure.unknown_devices.push_back(observation.entries[index].device.text());
      ++index;
    }
    if (index >= observation.entries.size() ||
        !(observation.entries[index].device == member.device)) {
      closure.missing_devices.push_back(member.device.text());
      continue;
    }
    const InventoryEntry& match = observation.entries[index];
    ++index;
    if (!match.readable) {
      closure.unreadable_devices.push_back(member.device.text());
      continue;
    }
    ++observed;
    if (!(match.slot == member.slot)) {
      closure.displaced_devices.push_back(member.device.text());
    }
  }
  while (index < observation.entries.size()) {
    closure.unknown_devices.push_back(observation.entries[index].device.text());
    ++index;
  }
  closure.observed_members = counter_from<ObservedDeviceCount>(observed);
  closure.unreadable_positions = observation.unreadable_positions;

  std::vector<SlotCoordinate> occupied;
  occupied.reserve(observation.entries.size());
  for (const InventoryEntry& entry : observation.entries) {
    if (entry.readable) {
      occupied.push_back(entry.slot);
    }
  }
  std::sort(occupied.begin(), occupied.end());
  for (std::size_t i = 1; i < occupied.size(); ++i) {
    if (occupied[i - 1] == occupied[i]) {
      const std::string text = occupied[i].to_text();
      if (closure.duplicate_slots.empty() || closure.duplicate_slots.back() != text) {
        closure.duplicate_slots.push_back(text);
      }
    }
  }

  // Precedence of the primary code is fixed so that the same inventory always
  // produces the same primary reason: an unknown member contradicts the
  // composition itself, a duplicated slot contradicts the inventory, a
  // displaced member contradicts the planned placement, an unreadable position
  // leaves closure unproven, and a missing member is the plainest gap.
  if (!closure.unknown_devices.empty()) {
    closure.state = InventoryClosureState::Ambiguous;
    closure.code = ErrorCode::UnknownCompositionMember;
  } else if (!closure.duplicate_slots.empty()) {
    closure.state = InventoryClosureState::Ambiguous;
    closure.code = ErrorCode::DuplicateInventorySlot;
  } else if (!closure.displaced_devices.empty()) {
    closure.state = InventoryClosureState::Ambiguous;
    closure.code = ErrorCode::InventoryAmbiguous;
  } else if (!closure.unreadable_devices.empty() || observation.unreadable_positions != 0) {
    closure.state = InventoryClosureState::Gap;
    closure.code = ErrorCode::InventoryUnreadable;
  } else if (!closure.missing_devices.empty()) {
    closure.state = InventoryClosureState::Gap;
    closure.code = ErrorCode::InventoryGap;
  } else {
    closure.state = InventoryClosureState::Closed;
    closure.code = ErrorCode::Ok;
  }
  return closure;
}

}  // namespace rackturnup
