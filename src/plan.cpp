// Rack Turnup Manager - plan requirements, policy and binding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/plan.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/text.hpp"

namespace rackturnup {
namespace {

constexpr std::uint32_t kRequirementsModel = 1;
constexpr std::uint32_t kPolicyModel = 1;
constexpr std::uint32_t kRequirementsDigestModel = 1;
constexpr std::uint32_t kBindingModel = 1;
constexpr std::uint32_t kPlanModel = 1;

[[nodiscard]] Result<bool> parse_bool_flag(std::string_view text, std::string_view key) {
  if (text == "true") {
    return true;
  }
  if (text == "false") {
    return false;
  }
  return make_error(ErrorCode::InvalidArgument, "boolean field must be true or false",
                    ErrorDetail{.operation = "parse_compatibility_requirement",
                                .subject = std::string(key) + "=" + std::string(text)});
}

}  // namespace

std::string_view requirement_kind_name(RequirementKind kind) noexcept {
  switch (kind) {
    case RequirementKind::TraitOnEveryMember:
      return "trait_on_every_member";
    case RequirementKind::TraitOnAnyMember:
      return "trait_on_any_member";
    case RequirementKind::TraitOnDevice:
      return "trait_on_device";
    case RequirementKind::BaselineOnEveryMember:
      return "baseline_on_every_member";
    case RequirementKind::BaselineOnDevice:
      return "baseline_on_device";
  }
  return "unknown";
}

Result<RequirementKind> requirement_kind_from_name(std::string_view name) {
  for (std::uint8_t value = 1; value <= 5; ++value) {
    const auto kind = static_cast<RequirementKind>(value);
    if (requirement_kind_name(kind) == name) {
      return kind;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown requirement kind name",
                    ErrorDetail{.operation = "requirement_kind_from_name",
                                .subject = std::string(name)});
}

bool requirement_is_trait_scoped(RequirementKind kind) noexcept {
  return kind == RequirementKind::TraitOnEveryMember || kind == RequirementKind::TraitOnAnyMember ||
         kind == RequirementKind::TraitOnDevice;
}

bool requirement_is_baseline_scoped(RequirementKind kind) noexcept {
  return kind == RequirementKind::BaselineOnEveryMember || kind == RequirementKind::BaselineOnDevice;
}

bool requirement_is_device_scoped(RequirementKind kind) noexcept {
  return kind == RequirementKind::TraitOnDevice || kind == RequirementKind::BaselineOnDevice;
}

bool CompatibilityRequirement::is_consistent() const noexcept {
  if (requirement_is_trait_scoped(kind) != !trait.empty()) {
    return false;
  }
  if (requirement_is_baseline_scoped(kind) != !firmware_baseline.empty()) {
    return false;
  }
  if (requirement_is_device_scoped(kind) != !device.empty()) {
    return false;
  }
  return true;
}

std::string CompatibilityRequirement::to_text() const {
  std::string out = "kind=";
  out.append(requirement_kind_name(kind));
  if (!trait.empty()) {
    out.append(";trait=");
    out.append(trait.text());
  }
  if (!device.empty()) {
    out.append(";device=");
    out.append(device.text());
  }
  if (!firmware_baseline.empty()) {
    out.append(";baseline=");
    out.append(firmware_baseline.text());
  }
  out.append(";mandatory=");
  out.append(mandatory ? "true" : "false");
  if (!note.empty()) {
    out.append(";note=");
    out.append(note.text());
  }
  return out;
}

void CompatibilityRequirement::update_digest(Sha256& hasher) const noexcept {
  hasher.update_len_text(requirement_kind_name(kind));
  hasher.update_len_text(trait.text());
  hasher.update_len_text(device.text());
  hasher.update_len_text(firmware_baseline.text());
  hasher.update_byte(mandatory ? 1u : 0u);
  hasher.update_len_text(note.text());
}

Result<CompatibilityRequirement> make_compatibility_requirement(
    RequirementKind kind, Trait trait, DeviceId device, FirmwareBaselineId firmware_baseline,
    bool mandatory, Note note) {
  CompatibilityRequirement requirement;
  requirement.kind = kind;
  requirement.trait = std::move(trait);
  requirement.device = std::move(device);
  requirement.firmware_baseline = std::move(firmware_baseline);
  requirement.mandatory = mandatory;
  requirement.note = std::move(note);
  if (!requirement.is_consistent()) {
    return make_error(ErrorCode::MalformedCompatibilityProfile,
                      "compatibility requirement carries fields its kind does not use",
                      ErrorDetail{.operation = "make_compatibility_requirement",
                                  .subject = requirement.to_text()});
  }
  return requirement;
}

Result<CompatibilityRequirement> parse_compatibility_requirement(std::string_view text) {
  CompatibilityRequirement requirement;
  requirement.kind = RequirementKind::TraitOnEveryMember;
  bool have_kind = false;
  bool have_mandatory = false;
  for (const std::string& part : split(text, ';')) {
    const std::size_t equals = part.find('=');
    if (equals == std::string::npos || equals == 0) {
      return make_error(ErrorCode::InvalidArgument,
                        "requirement fields must be written as key=value",
                        ErrorDetail{.operation = "parse_compatibility_requirement",
                                    .subject = part});
    }
    const std::string_view key(part.data(), equals);
    const std::string_view value(part.data() + equals + 1, part.size() - equals - 1);
    if (key == "kind") {
      const Result<RequirementKind> kind = requirement_kind_from_name(value);
      if (!kind.has_value()) {
        return kind.error();
      }
      requirement.kind = kind.value();
      have_kind = true;
    } else if (key == "mandatory") {
      const Result<bool> flag = parse_bool_flag(value, key);
      if (!flag.has_value()) {
        return flag.error();
      }
      requirement.mandatory = flag.value();
      have_mandatory = true;
    } else if (key == "trait") {
      const Result<Trait> parsed = Trait::parse(value);
      if (!parsed.has_value()) {
        return parsed.error();
      }
      requirement.trait = parsed.value();
    } else if (key == "device") {
      const Result<DeviceId> parsed = DeviceId::parse(value);
      if (!parsed.has_value()) {
        return parsed.error();
      }
      requirement.device = parsed.value();
    } else if (key == "baseline") {
      const Result<FirmwareBaselineId> parsed = FirmwareBaselineId::parse(value);
      if (!parsed.has_value()) {
        return parsed.error();
      }
      requirement.firmware_baseline = parsed.value();
    } else if (key == "note") {
      const Result<Note> parsed = parse_note_or_none(value);
      if (!parsed.has_value()) {
        return parsed.error();
      }
      requirement.note = parsed.value();
    } else {
      return make_error(ErrorCode::InvalidArgument, "unknown requirement field",
                        ErrorDetail{.operation = "parse_compatibility_requirement",
                                    .subject = std::string(key)});
    }
  }
  if (!have_kind) {
    return make_error(ErrorCode::InvalidArgument, "requirement must declare its kind",
                      ErrorDetail{.operation = "parse_compatibility_requirement",
                                  .subject = std::string(text)});
  }
  if (!have_mandatory) {
    requirement.mandatory = true;
  }
  if (!requirement.is_consistent()) {
    return make_error(ErrorCode::MalformedCompatibilityProfile,
                      "requirement fields do not match its kind",
                      ErrorDetail{.operation = "parse_compatibility_requirement",
                                  .subject = requirement.to_text()});
  }
  return requirement;
}

void PlanRequirements::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kRequirementsModel);
  hasher.update_u64(required_power_milliwatts);
  hasher.update_u64(required_cooling_milliwatts);
  hasher.update_u32(required_network_ports);
  std::uint8_t flags = 0;
  if (require_redundant_feeds) {
    flags = static_cast<std::uint8_t>(flags | 1u);
  }
  if (require_applied_power_verification) {
    flags = static_cast<std::uint8_t>(flags | 2u);
  }
  if (require_cooling_delivery_verification) {
    flags = static_cast<std::uint8_t>(flags | 4u);
  }
  if (require_network_authority_verification) {
    flags = static_cast<std::uint8_t>(flags | 8u);
  }
  if (require_health_pass_for_every_member) {
    flags = static_cast<std::uint8_t>(flags | 16u);
  }
  hasher.update_byte(flags);
  hasher.update_len_text(required_topology.text());
  hasher.update_u32(static_cast<std::uint32_t>(compatibility.size()));
  for (const CompatibilityRequirement& requirement : compatibility) {
    requirement.update_digest(hasher);
  }
}

Result<PlanRequirements> make_plan_requirements(PlanRequirements requirements) {
  if (requirements.compatibility.size() > kMaxCompatibilityRequirements) {
    return make_error(ErrorCode::LimitExceeded,
                      "plan declares more compatibility requirements than are supported",
                      ErrorDetail{.operation = "make_plan_requirements",
                                  .expected = kMaxCompatibilityRequirements,
                                  .actual = requirements.compatibility.size()});
  }
  for (const CompatibilityRequirement& requirement : requirements.compatibility) {
    if (!requirement.is_consistent()) {
      return make_error(ErrorCode::MalformedCompatibilityProfile,
                        "compatibility requirement carries fields its kind does not use",
                        ErrorDetail{.operation = "make_plan_requirements",
                                    .subject = requirement.to_text()});
    }
  }
  std::sort(requirements.compatibility.begin(), requirements.compatibility.end(),
            [](const CompatibilityRequirement& left, const CompatibilityRequirement& right) {
              return byte_less(left.to_text(), right.to_text());
            });
  for (std::size_t i = 1; i < requirements.compatibility.size(); ++i) {
    if (requirements.compatibility[i - 1].to_text() == requirements.compatibility[i].to_text()) {
      return make_error(ErrorCode::DuplicateCompatibilityRequirement,
                        "the plan declares the same compatibility requirement twice",
                        ErrorDetail{.operation = "make_plan_requirements",
                                    .subject = requirements.compatibility[i].to_text()});
    }
  }
  return requirements;
}

void TurnupPolicy::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPolicyModel);
  hasher.update_u64(generation.value());
  hasher.update_byte(allow_degraded_subsystems ? 1u : 0u);
  hasher.update_u64(static_cast<std::uint64_t>(authorization_validity.milliseconds()));
  hasher.update_u64(static_cast<std::uint64_t>(default_evidence_validity.milliseconds()));
}

Result<TurnupPolicy> make_turnup_policy(TurnupPolicy policy) {
  if (policy.authorization_validity.is_zero()) {
    return make_error(ErrorCode::InvalidDuration,
                      "authorization validity must be greater than zero",
                      ErrorDetail{.operation = "make_turnup_policy",
                                  .subject = "authorization_validity"});
  }
  return policy;
}

void PlanBinding::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kBindingModel);
  hasher.update_len_text(rack.text());
  const std::vector<std::uint8_t>& bytes = composition.bytes();
  hasher.update(bytes.data(), bytes.size());
  stamp.update_digest(hasher);
  hasher.update_u64(epoch.value());
}

bool PlanBinding::operator==(const PlanBinding& other) const noexcept {
  return rack == other.rack && composition == other.composition && stamp == other.stamp &&
         epoch == other.epoch;
}

std::string_view plan_state_name(PlanState state) noexcept {
  switch (state) {
    case PlanState::Active:
      return "active";
    case PlanState::Superseded:
      return "superseded";
    case PlanState::Cancelled:
      return "cancelled";
    case PlanState::Completed:
      return "completed";
  }
  return "unknown";
}

Digest RackTurnupPlan::compute_requirements_digest() const {
  Sha256 hasher;
  hasher.update_u32(kRequirementsDigestModel);
  requirements.update_digest(hasher);
  policy.update_digest(hasher);
  return hasher.finish();
}

void RackTurnupPlan::update_digest(Sha256& hasher) const noexcept {
  hasher.update_u32(kPlanModel);
  hasher.update_len_text(id.text());
  hasher.update_u64(lifetime.value());
  hasher.update_u64(revision.value());
  binding.update_digest(hasher);
  const std::vector<std::uint8_t>& bytes = requirements_digest.bytes();
  hasher.update(bytes.data(), bytes.size());
  hasher.update_byte(static_cast<std::uint8_t>(state));
  hasher.update_u64(evidence_high_water.value());
}

}  // namespace rackturnup
