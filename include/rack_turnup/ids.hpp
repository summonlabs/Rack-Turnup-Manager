// Rack Turnup Manager - strongly typed identities, generations and epochs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "rack_turnup/export.hpp"
#include "rack_turnup/limits.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/text.hpp"

namespace rackturnup {

namespace detail {

[[nodiscard]] constexpr bool ident_first(char c) noexcept { return ascii_alphanumeric(c); }
[[nodiscard]] constexpr bool ident_rest(char c) noexcept {
  return ascii_alphanumeric(c) || c == '.' || c == '_' || c == '-' || c == ':';
}
[[nodiscard]] constexpr bool opaque_rest(char c) noexcept {
  return ascii_alphanumeric(c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '/' ||
         c == '@' || c == '+';
}
[[nodiscard]] constexpr bool trait_first(char c) noexcept { return ascii_lower_alphanumeric(c); }
[[nodiscard]] constexpr bool trait_rest(char c) noexcept {
  return ascii_lower_alphanumeric(c) || c == '.' || c == '_' || c == '-';
}

// Shared implementation of "a canonical text value with a policy". The derived
// type is final, so no further type can inherit a constructor from it.
template <typename Derived, typename Policy>
class CanonicalText {
 public:
  static Result<Derived> parse(std::string_view text) {
    if (text.empty()) {
      return make_error(ErrorCode::EmptyValue,
                        std::string(Policy::kind) + " must not be empty",
                        ErrorDetail{.operation = std::string(Policy::kind)});
    }
    if (text.size() > Policy::max_bytes) {
      return make_error(ErrorCode::IdentityTooLong,
                        std::string(Policy::kind) + " exceeds " +
                            std::to_string(Policy::max_bytes) + " bytes",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .expected = Policy::max_bytes,
                                    .actual = text.size()});
    }

    std::string_view body = text;
    if constexpr (!Policy::prefix.empty()) {
      if (body.size() < Policy::prefix.size() ||
          body.substr(0, Policy::prefix.size()) != Policy::prefix) {
        return make_error(ErrorCode::MalformedIdentity,
                          std::string(Policy::kind) + " must begin with '" +
                              std::string(Policy::prefix) + "'",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      body.remove_prefix(Policy::prefix.size());
    }
    if (body.empty()) {
      return make_error(ErrorCode::EmptyValue,
                        std::string(Policy::kind) + " has an empty body",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .subject = std::string(text)});
    }
    if (body.size() > Policy::max_body_bytes) {
      return make_error(ErrorCode::IdentityTooLong,
                        std::string(Policy::kind) + " body exceeds " +
                            std::to_string(Policy::max_body_bytes) + " bytes",
                        ErrorDetail{.operation = std::string(Policy::kind),
                                    .expected = Policy::max_body_bytes,
                                    .actual = body.size()});
    }

    if constexpr (Policy::free_text) {
      if (!is_valid_utf8(body)) {
        return make_error(ErrorCode::InvalidUtf8,
                          std::string(Policy::kind) + " is not valid UTF-8",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      if (!has_no_control_characters(body)) {
        return make_error(ErrorCode::InvalidCharacter,
                          std::string(Policy::kind) + " contains a control character",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
    } else {
      if (!Policy::first_char_ok(body.front())) {
        return make_error(ErrorCode::InvalidCharacter,
                          std::string(Policy::kind) + " starts with a character outside its domain",
                          ErrorDetail{.operation = std::string(Policy::kind),
                                      .subject = std::string(text)});
      }
      for (const char c : body.substr(1)) {
        if (!Policy::rest_char_ok(c)) {
          return make_error(ErrorCode::InvalidCharacter,
                            std::string(Policy::kind) + " contains a character outside its domain",
                            ErrorDetail{.operation = std::string(Policy::kind),
                                        .subject = std::string(text)});
        }
      }
    }

    return Derived{std::string(text)};
  }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  [[nodiscard]] bool operator==(const CanonicalText& other) const noexcept {
    return text_ == other.text_;
  }
  [[nodiscard]] bool operator!=(const CanonicalText& other) const noexcept {
    return text_ != other.text_;
  }
  [[nodiscard]] bool operator<(const CanonicalText& other) const noexcept {
    return byte_less(text_, other.text_);
  }
  [[nodiscard]] bool operator<=(const CanonicalText& other) const noexcept {
    return !(other < *this);
  }
  [[nodiscard]] bool operator>(const CanonicalText& other) const noexcept { return other < *this; }
  [[nodiscard]] bool operator>=(const CanonicalText& other) const noexcept { return !(*this < other); }

 protected:
  CanonicalText() = default;
  explicit CanonicalText(std::string text) : text_(std::move(text)) {}
  std::string text_;
};

}  // namespace detail

// Declares one canonical identity type with its own text policy. Each type is
// final, has a private value constructor, and is obtained only through
// `parse`, so an unvalidated string can never become an identity by accident.
#define RACK_TURNUP_IDENTITY_TYPE(TypeName, Kind, Prefix, MaxBytes, MaxBody, FreeText, FirstOk, RestOk) \
  struct TypeName##Policy {                                                                            \
    static constexpr std::string_view kind = Kind;                                                     \
    static constexpr std::string_view prefix = Prefix;                                                 \
    static constexpr std::size_t max_bytes = MaxBytes;                                                 \
    static constexpr std::size_t max_body_bytes = MaxBody;                                             \
    static constexpr bool free_text = FreeText;                                                        \
    static constexpr auto first_char_ok = FirstOk;                                                     \
    static constexpr auto rest_char_ok = RestOk;                                                       \
  };                                                                                                   \
  class TypeName final : public detail::CanonicalText<TypeName, TypeName##Policy> {                    \
   private:                                                                                            \
    friend class detail::CanonicalText<TypeName, TypeName##Policy>;                                     \
    using Base = detail::CanonicalText<TypeName, TypeName##Policy>;                                     \
    explicit TypeName(std::string text) : Base(std::move(text)) {}                                      \
                                                                                                       \
   public:                                                                                             \
    TypeName() = default;                                                                              \
  }

// Canonical rack identity, text form "rack:...".
RACK_TURNUP_IDENTITY_TYPE(RackId, "RackId", "rack:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Canonical turnup plan identity, text form "tp:...".
RACK_TURNUP_IDENTITY_TYPE(PlanId, "PlanId", "tp:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Canonical evidence record identity, text form "te:...".
RACK_TURNUP_IDENTITY_TYPE(EvidenceId, "EvidenceId", "te:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Canonical device identity, text form "dev:...".
RACK_TURNUP_IDENTITY_TYPE(DeviceId, "DeviceId", "dev:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Canonical site identity, text form "site:...".
RACK_TURNUP_IDENTITY_TYPE(SiteId, "SiteId", "site:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Identity of one activation attempt, text form "at:...".
RACK_TURNUP_IDENTITY_TYPE(AttemptId, "AttemptId", "at:", kMaxIdentityTextBytes, 96, false,
                          detail::ident_first, detail::ident_rest);

// Canonical identity of a firmware baseline, text form "fw:...".
RACK_TURNUP_IDENTITY_TYPE(FirmwareBaselineId, "FirmwareBaselineId", "fw:", kMaxIdentityTextBytes, 96,
                          false, detail::ident_first, detail::ident_rest);

// Canonical identity of a health-evidence source, text form "hsrc:...".
RACK_TURNUP_IDENTITY_TYPE(HealthSourceId, "HealthSourceId", "hsrc:", kMaxIdentityTextBytes, 96,
                          false, detail::ident_first, detail::ident_rest);

// Caller-supplied identity of one mutation request, used for bounded
// idempotency. It is opaque to the library and carries no prefix.
RACK_TURNUP_IDENTITY_TYPE(RequestId, "RequestId", "", kMaxRequestIdBytes, kMaxRequestIdBytes, false,
                          detail::ident_first, detail::ident_rest);

// Typed reference to an external power domain. The type is distinct from
// CoolingDomainReference and NetworkDomainReference: the three can never be
// interchanged or compared.
RACK_TURNUP_IDENTITY_TYPE(PowerDomainReference, "PowerDomainReference", "",
                          kMaxOpaqueReferenceBytes, kMaxOpaqueReferenceBytes, false,
                          detail::ident_first, detail::opaque_rest);

// Typed reference to an external cooling domain.
RACK_TURNUP_IDENTITY_TYPE(CoolingDomainReference, "CoolingDomainReference", "",
                          kMaxOpaqueReferenceBytes, kMaxOpaqueReferenceBytes, false,
                          detail::ident_first, detail::opaque_rest);

// Typed reference to an external network attachment domain, for example a rack
// fabric domain owned by distributed fabric infrastructure.
RACK_TURNUP_IDENTITY_TYPE(NetworkDomainReference, "NetworkDomainReference", "",
                          kMaxOpaqueReferenceBytes, kMaxOpaqueReferenceBytes, false,
                          detail::ident_first, detail::opaque_rest);

// Opaque reference to an asset owned by another runtime (Asset Registry). This
// library never interprets the value.
RACK_TURNUP_IDENTITY_TYPE(AssetId, "AssetId", "", kMaxOpaqueReferenceBytes,
                          kMaxOpaqueReferenceBytes, false, detail::ident_first, detail::opaque_rest);

// Opaque reference to a fabric topology owned by distributed fabric
// infrastructure.
RACK_TURNUP_IDENTITY_TYPE(FabricTopologyReference, "FabricTopologyReference", "",
                          kMaxOpaqueReferenceBytes, kMaxOpaqueReferenceBytes, false,
                          detail::ident_first, detail::opaque_rest);

// Identity of one that performed a mutation, for provenance.
RACK_TURNUP_IDENTITY_TYPE(ActorId, "ActorId", "", kMaxActorBytes, kMaxActorBytes, false,
                          detail::ident_first, detail::opaque_rest);

// Opaque reference to the upstream record a mutation or evidence record came
// from. It is what makes evidence provenance auditable.
RACK_TURNUP_IDENTITY_TYPE(SourceReference, "SourceReference", "", kMaxSourceReferenceBytes,
                          kMaxSourceReferenceBytes, false, detail::ident_first, detail::opaque_rest);

// A lowercase compatibility trait, for example "power.ac.208v" or
// "fw.baseline.2026q1". Traits are the vocabulary of compatibility checks.
RACK_TURNUP_IDENTITY_TYPE(Trait, "Trait", "", kMaxTraitBytes, kMaxTraitBytes, false,
                          detail::trait_first, detail::trait_rest);

// Human-facing label. Free UTF-8 text without control characters; it never
// participates in identity.
RACK_TURNUP_IDENTITY_TYPE(DisplayLabel, "DisplayLabel", "", kMaxLabelBytes, kMaxLabelBytes, true,
                          detail::ident_first, detail::opaque_rest);

// Free-text note attached to provenance or a rollback. Empty is valid.
RACK_TURNUP_IDENTITY_TYPE(Note, "Note", "", kMaxNoteBytes, kMaxNoteBytes, true, detail::ident_first,
                          detail::opaque_rest);

#undef RACK_TURNUP_IDENTITY_TYPE

// An empty label means "no label". The generic parser rejects empty text, so
// this factory exists for that one case.
[[nodiscard]] RACK_TURNUP_API Result<DisplayLabel> parse_label_or_none(std::string_view text);

// An empty note means "no note".
[[nodiscard]] RACK_TURNUP_API Result<Note> parse_note_or_none(std::string_view text);

// Monotonic counter base. The derived type is final and its value constructor
// is private, so the only ways to obtain one are the named factories below.
template <typename Derived>
class CounterValue {
 public:
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_max() const noexcept {
    return value_ == static_cast<std::uint64_t>(-1);
  }
  // Saturating successor. Callers that must reject exhaustion check is_max()
  // first; this library does exactly that and reports LimitExceeded.
  [[nodiscard]] constexpr Derived next() const noexcept {
    return is_max() ? Derived{value_} : Derived{value_ + 1};
  }

  [[nodiscard]] constexpr bool operator==(const CounterValue& other) const noexcept {
    return value_ == other.value_;
  }
  [[nodiscard]] constexpr bool operator!=(const CounterValue& other) const noexcept {
    return value_ != other.value_;
  }
  [[nodiscard]] constexpr bool operator<(const CounterValue& other) const noexcept {
    return value_ < other.value_;
  }
  [[nodiscard]] constexpr bool operator<=(const CounterValue& other) const noexcept {
    return value_ <= other.value_;
  }
  [[nodiscard]] constexpr bool operator>(const CounterValue& other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool operator>=(const CounterValue& other) const noexcept {
    return value_ >= other.value_;
  }

 protected:
  constexpr CounterValue() noexcept = default;
  constexpr explicit CounterValue(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// Declares a counter type whose value must be at least one, so that zero stays
// available as "unset" everywhere it matters.
#define RACK_TURNUP_COUNTER_POSITIVE(TypeName, MinimumText)                              \
  class TypeName final : public CounterValue<TypeName> {                                 \
   private:                                                                              \
    friend class CounterValue<TypeName>;                                                 \
    constexpr explicit TypeName(std::uint64_t value) noexcept : CounterValue<TypeName>(value) {} \
                                                                                         \
   public:                                                                               \
    constexpr TypeName() noexcept = default;                                             \
    [[nodiscard]] static constexpr TypeName initial() noexcept { return TypeName{1}; }   \
    [[nodiscard]] static Result<TypeName> create(std::uint64_t value) {                  \
      if (value == 0) {                                                                  \
        return make_error(ErrorCode::InvalidRange, MinimumText " must be at least 1",     \
                          ErrorDetail{.operation = #TypeName, .expected = 1, .actual = value}); \
      }                                                                                  \
      return TypeName{value};                                                            \
    }                                                                                    \
  }

// Declares a counter type for which zero is a meaningful starting value.
#define RACK_TURNUP_COUNTER_ZERO_BASED(TypeName)                                         \
  class TypeName final : public CounterValue<TypeName> {                                 \
   private:                                                                              \
    friend class CounterValue<TypeName>;                                                 \
    constexpr explicit TypeName(std::uint64_t value) noexcept : CounterValue<TypeName>(value) {} \
                                                                                         \
   public:                                                                               \
    constexpr TypeName() noexcept = default;                                             \
    [[nodiscard]] static constexpr TypeName initial() noexcept { return TypeName{0}; }   \
    [[nodiscard]] static Result<TypeName> create(std::uint64_t value) {                  \
      return TypeName{value};                                                            \
    }                                                                                    \
  }

// ---------------------------------------------------------------------------
// Plan-scoped generations
// ---------------------------------------------------------------------------

// Revision of one turnup plan. Every accepted mutation of the plan advances it
// by exactly one, whether the mutation was a stage transition, an evidence
// import or an authorization. It starts at 1 when the plan is created.
RACK_TURNUP_COUNTER_POSITIVE(PlanRevision, "PlanRevision");

// Position of a plan in the rack's plan lineage. It starts at 1 for the first
// plan ever created for a rack and advances by exactly one for each successor
// plan, so a superseded plan is distinguishable from a revision of the current
// one.
RACK_TURNUP_COUNTER_POSITIVE(PlanLifetime, "PlanLifetime");

// ---------------------------------------------------------------------------
// Facility generations the plan binds to
// ---------------------------------------------------------------------------

RACK_TURNUP_COUNTER_POSITIVE(RackGeneration, "RackGeneration");
RACK_TURNUP_COUNTER_POSITIVE(CompositionGeneration, "CompositionGeneration");
RACK_TURNUP_COUNTER_POSITIVE(TopologyGeneration, "TopologyGeneration");
RACK_TURNUP_COUNTER_POSITIVE(DependencyGeneration, "DependencyGeneration");
RACK_TURNUP_COUNTER_POSITIVE(PowerGeneration, "PowerGeneration");
RACK_TURNUP_COUNTER_POSITIVE(CoolingGeneration, "CoolingGeneration");
RACK_TURNUP_COUNTER_POSITIVE(NetworkGeneration, "NetworkGeneration");
RACK_TURNUP_COUNTER_POSITIVE(InventoryGeneration, "InventoryGeneration");
RACK_TURNUP_COUNTER_POSITIVE(HealthGeneration, "HealthGeneration");
RACK_TURNUP_COUNTER_POSITIVE(CapacityGeneration, "CapacityGeneration");
RACK_TURNUP_COUNTER_POSITIVE(MaintenanceGeneration, "MaintenanceGeneration");
RACK_TURNUP_COUNTER_POSITIVE(PolicyGeneration, "PolicyGeneration");
RACK_TURNUP_COUNTER_POSITIVE(FirmwareGeneration, "FirmwareGeneration");

// Control epoch of the service the plan belongs to. It advances when operator
// authority is taken over, so a plan created under an older epoch can never be
// used to authorize a turnup.
RACK_TURNUP_COUNTER_POSITIVE(ControlEpoch, "ControlEpoch");

// Identity of one process incarnation of the service. It advances on every
// acquisition of the durable store and fences asynchronous completions that
// belong to a previous incarnation.
RACK_TURNUP_COUNTER_POSITIVE(IncarnationId, "IncarnationId");

// Observation sequence of the service. It advances by exactly one for every
// accepted evidence import and every recorded activation observation, and it is
// what makes the ordering of imported observations explicit rather than
// implied by arrival time.
RACK_TURNUP_COUNTER_ZERO_BASED(ObservationSequence);

// ---------------------------------------------------------------------------
// Durable store authority
// ---------------------------------------------------------------------------

// Ownership epoch of the durable store. It advances when writer authority is
// taken over, so that a writer holding an older epoch is fenced out.
RACK_TURNUP_COUNTER_POSITIVE(StoreEpoch, "StoreEpoch");

// Publication sequence of the durable store. It advances by exactly one for
// every atomically published generation of state.
RACK_TURNUP_COUNTER_ZERO_BASED(StoreSequence);

// ---------------------------------------------------------------------------
// Counts that must never be confused with generations
// ---------------------------------------------------------------------------

// Number of composition members the rack declared at plan time.
RACK_TURNUP_COUNTER_ZERO_BASED(ExpectedDeviceCount);

// Number of composition members for which inventory evidence was observed.
RACK_TURNUP_COUNTER_ZERO_BASED(ObservedDeviceCount);

// Number of composition members whose health evidence was observed and passing.
RACK_TURNUP_COUNTER_ZERO_BASED(HealthyDeviceCount);

#undef RACK_TURNUP_COUNTER_POSITIVE
#undef RACK_TURNUP_COUNTER_ZERO_BASED

// A generation counter of an unknown kind. It is used only where the domain
// genuinely names a generation that this library does not model itself, for
// example a maintenance generation supplied as an opaque precondition.
class GenerationReference final : public CounterValue<GenerationReference> {
 private:
  friend class CounterValue<GenerationReference>;
  constexpr explicit GenerationReference(std::uint64_t value) noexcept
      : CounterValue<GenerationReference>(value) {}

 public:
  constexpr GenerationReference() noexcept = default;
  [[nodiscard]] static constexpr GenerationReference none() noexcept {
    return GenerationReference{0};
  }
  [[nodiscard]] static Result<GenerationReference> create(std::uint64_t value) {
    return GenerationReference{value};
  }
};

}  // namespace rackturnup
