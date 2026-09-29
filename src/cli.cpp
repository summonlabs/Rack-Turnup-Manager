// Rack Turnup Manager - command line interface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/cli.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rack_turnup/authorization.hpp"
#include "rack_turnup/composition.hpp"
#include "rack_turnup/digest.hpp"
#include "rack_turnup/errors.hpp"
#include "rack_turnup/evaluation.hpp"
#include "rack_turnup/evidence.hpp"
#include "rack_turnup/ids.hpp"
#include "rack_turnup/model.hpp"
#include "rack_turnup/observation.hpp"
#include "rack_turnup/plan.hpp"
#include "rack_turnup/service.hpp"
#include "rack_turnup/state.hpp"
#include "rack_turnup/store.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/time.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

// Exit codes are part of the command line contract, so a caller can separate a
// malformed invocation from a rejected request and from a store it could not
// open without parsing the error text.
constexpr int kExitSuccess = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRejected = 2;
constexpr int kExitFailure = 3;

constexpr std::string_view kProgramName = "rack-turnup";
constexpr std::string_view kDefaultStorePath = "rack_turnup.state";
constexpr std::string_view kDefaultActor = "cli";

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

// One reported value. A report is an object of ordered fields, and a field is
// either a scalar or a list of records. The text renderer and the JSON renderer
// walk the same structure, so the two formats can never disagree about what was
// reported, and the field order is the order the command added them in.
class Value final {
 public:
  enum class Kind : std::uint8_t { Object, List, Text, Number, Flag };

  Value() = default;

  [[nodiscard]] static Value of_list() {
    Value value;
    value.kind_ = Kind::List;
    return value;
  }

  [[nodiscard]] static Value of_text(std::string text) {
    Value value;
    value.kind_ = Kind::Text;
    value.text_ = std::move(text);
    return value;
  }

  [[nodiscard]] static Value of_number(std::uint64_t number) {
    Value value;
    value.kind_ = Kind::Number;
    value.number_ = number;
    return value;
  }

  [[nodiscard]] static Value of_flag(bool flag) {
    Value value;
    value.kind_ = Kind::Flag;
    value.flag_ = flag;
    return value;
  }

  Value& put(std::string name, Value value) {
    fields_.emplace_back(std::move(name), std::move(value));
    return *this;
  }

  Value& add(Value value) {
    elements_.push_back(std::move(value));
    return *this;
  }

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& as_text() const noexcept { return text_; }
  [[nodiscard]] std::uint64_t as_number() const noexcept { return number_; }
  [[nodiscard]] bool as_flag() const noexcept { return flag_; }
  [[nodiscard]] const std::vector<Value>& elements() const noexcept { return elements_; }
  [[nodiscard]] const std::vector<std::pair<std::string, Value>>& fields() const noexcept {
    return fields_;
  }

 private:
  Kind kind_ = Kind::Object;
  std::string text_;
  std::uint64_t number_ = 0;
  bool flag_ = false;
  std::vector<Value> elements_;
  std::vector<std::pair<std::string, Value>> fields_;
};

[[nodiscard]] Value text_value(std::string_view text) { return Value::of_text(std::string(text)); }

[[nodiscard]] Value number_value(std::uint64_t number) { return Value::of_number(number); }

[[nodiscard]] Value flag_value(bool flag) { return Value::of_flag(flag); }

[[nodiscard]] Value list_value() { return Value::of_list(); }

template <typename Counter>
[[nodiscard]] Value counter_value(const Counter& counter) {
  return Value::of_number(counter.value());
}

// The scalar text of a value that is not a container, used by the text renderer
// and by the list-of-scalars case.
[[nodiscard]] std::string scalar_text(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Text:
      return value.as_text();
    case Value::Kind::Number:
      return std::to_string(value.as_number());
    case Value::Kind::Flag:
      return value.as_flag() ? std::string("true") : std::string("false");
    case Value::Kind::Object:
    case Value::Kind::List:
      break;
  }
  return std::string();
}

// Writes one line per field. A list of records becomes a numbered header line
// followed by the indented fields of each record, so the text stays line based
// and deterministic.
void write_text(const Value& value, std::ostream& out, const std::string& indent) {
  for (const std::pair<std::string, Value>& field : value.fields()) {
    const Value& child = field.second;
    if (child.kind() == Value::Kind::List) {
      std::uint64_t index = 0;
      for (const Value& element : child.elements()) {
        ++index;
        if (element.kind() == Value::Kind::Object) {
          out << indent << field.first << "=" << index << "\n";
          write_text(element, out, indent + "  ");
        } else {
          out << indent << field.first << "=" << scalar_text(element) << "\n";
        }
      }
    } else if (child.kind() == Value::Kind::Object) {
      write_text(child, out, indent);
    } else {
      out << indent << field.first << "=" << scalar_text(child) << "\n";
    }
  }
}

void write_json_string(std::ostream& out, std::string_view text) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  out.put('"');
  for (const char raw : text) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (byte < 0x20) {
          out << "\\u00" << kHexDigits[(byte >> 4) & 0x0F] << kHexDigits[byte & 0x0F];
        } else {
          out.put(raw);
        }
        break;
    }
  }
  out.put('"');
}

void write_json(const Value& value, std::ostream& out, std::size_t depth) {
  switch (value.kind()) {
    case Value::Kind::Text:
      write_json_string(out, value.as_text());
      break;
    case Value::Kind::Number:
      out << value.as_number();
      break;
    case Value::Kind::Flag:
      out << (value.as_flag() ? "true" : "false");
      break;
    case Value::Kind::List: {
      const std::vector<Value>& elements = value.elements();
      if (elements.empty()) {
        out << "[]";
        break;
      }
      out << "[\n";
      for (std::size_t index = 0; index < elements.size(); ++index) {
        out << std::string((depth + 1) * 2, ' ');
        write_json(elements[index], out, depth + 1);
        out << ((index + 1 < elements.size()) ? ",\n" : "\n");
      }
      out << std::string(depth * 2, ' ') << "]";
      break;
    }
    case Value::Kind::Object: {
      const std::vector<std::pair<std::string, Value>>& fields = value.fields();
      if (fields.empty()) {
        out << "{}";
        break;
      }
      out << "{\n";
      for (std::size_t index = 0; index < fields.size(); ++index) {
        out << std::string((depth + 1) * 2, ' ');
        write_json_string(out, fields[index].first);
        out << ": ";
        write_json(fields[index].second, out, depth + 1);
        out << ((index + 1 < fields.size()) ? ",\n" : "\n");
      }
      out << std::string(depth * 2, ' ') << "}";
      break;
    }
  }
}

void emit_report(const Value& report, bool json, std::ostream& out) {
  if (json) {
    write_json(report, out, 0);
    out.put('\n');
    return;
  }
  write_text(report, out, std::string());
}

// ---------------------------------------------------------------------------
// Failures
// ---------------------------------------------------------------------------

// Prints the rejection and maps its category onto the process exit code: a
// request the domain refused exits 2, a store that could not be read or written
// exits 3.
[[nodiscard]] int report_error(const TurnupError& error, std::ostream& err) {
  err << describe(error) << "\n";
  const ErrorCategory category = category_of(error.code);
  if (category == ErrorCategory::Persistence || category == ErrorCategory::Writer) {
    return kExitFailure;
  }
  return kExitRejected;
}

[[nodiscard]] int usage_error(const std::string& message, std::ostream& err) {
  err << kProgramName << ": error: " << message << "\n";
  err << kProgramName << ": run '" << kProgramName << " help' for usage\n";
  return kExitUsage;
}

// The last resort for a failure no command anticipates. It never lets a broken
// output stream turn a reported failure into an escaping exception.
[[nodiscard]] int report_internal(std::string_view message, std::ostream& err) noexcept {
  try {
    err << kProgramName << ": error: " << message << "\n";
  } catch (...) {
    return kExitFailure;
  }
  return kExitFailure;
}

// ---------------------------------------------------------------------------
// Global options
// ---------------------------------------------------------------------------

// The resolved global options. Every command reads the authority time from
// here, so one run reports one instant.
struct Options {
  std::string store_path;
  WallClock authority_time;
  ActorId actor;
  SourceReference source;
  Note note;
  RequestId request;
  bool read_only = false;
  bool json = false;
};

struct GlobalRequest {
  bool help = false;
  bool version = false;
};

// Everything one command needs besides the service.
struct Invocation {
  const Options& options;
  const std::vector<std::string>& tokens;
  std::ostream& out;
  std::ostream& err;
};

// Consumes the value of the option at the cursor, which the caller has already
// identified as an option token.
[[nodiscard]] bool option_value(const std::vector<std::string>& arguments, std::size_t& index,
                                std::string_view option, std::string& out, std::string& error) {
  if (index + 1 >= arguments.size()) {
    error = "option " + std::string(option) + " requires a value";
    return false;
  }
  ++index;
  out = arguments[index];
  return true;
}

// Removes every global option from the argument list, leaving the command words
// and the command's own options in the rest vector. A global option is
// recognized wherever it appears, which is unambiguous because no command uses
// one of these names for its own purpose.
[[nodiscard]] bool extract_globals(const std::vector<std::string>& arguments, Options& options,
                                   GlobalRequest& global, std::vector<std::string>& rest,
                                   std::string& error) {
  bool have_now = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& token = arguments[index];
    std::string value;
    if (token == "--store") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      if (value.empty()) {
        error = "option --store requires a non-empty path";
        return false;
      }
      options.store_path = value;
    } else if (token == "--now") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      const Result<WallClock> parsed = WallClock::parse(value);
      if (!parsed.has_value()) {
        error = "--now: " + parsed.error().message;
        return false;
      }
      options.authority_time = parsed.value();
      have_now = true;
    } else if (token == "--actor") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      const Result<ActorId> parsed = ActorId::parse(value);
      if (!parsed.has_value()) {
        error = "--actor: " + parsed.error().message;
        return false;
      }
      options.actor = parsed.value();
    } else if (token == "--source") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      const Result<SourceReference> parsed = SourceReference::parse(value);
      if (!parsed.has_value()) {
        error = "--source: " + parsed.error().message;
        return false;
      }
      options.source = parsed.value();
    } else if (token == "--note") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      const Result<Note> parsed = parse_note_or_none(value);
      if (!parsed.has_value()) {
        error = "--note: " + parsed.error().message;
        return false;
      }
      options.note = parsed.value();
    } else if (token == "--request-id") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      const Result<RequestId> parsed = RequestId::parse(value);
      if (!parsed.has_value()) {
        error = "--request-id: " + parsed.error().message;
        return false;
      }
      options.request = parsed.value();
    } else if (token == "--format") {
      if (!option_value(arguments, index, token, value, error)) {
        return false;
      }
      if (value == "json") {
        options.json = true;
      } else if (value == "text") {
        options.json = false;
      } else {
        error = "option --format accepts 'text' or 'json', found '" + value + "'";
        return false;
      }
    } else if (token == "--read-only") {
      options.read_only = true;
    } else if (token == "-h" || token == "--help") {
      global.help = true;
    } else if (token == "--version") {
      global.version = true;
    } else {
      rest.push_back(token);
    }
  }
  if (!have_now) {
    options.authority_time = WallClock::now();
  }
  return true;
}

[[nodiscard]] MutationContext make_context(const Options& options) {
  MutationContext context;
  context.request = options.request;
  context.actor = options.actor;
  context.source = options.source;
  context.note = options.note;
  context.authority_time = options.authority_time;
  return context;
}

// ---------------------------------------------------------------------------
// Command line specs
// ---------------------------------------------------------------------------

[[nodiscard]] bool split_field(const std::string& part, std::string_view& key,
                               std::string_view& value, std::string& error) {
  const std::size_t equals = part.find('=');
  if (equals == std::string::npos || equals == 0) {
    error = "fields must be written as key=value, found '" + part + "'";
    return false;
  }
  key = std::string_view(part.data(), equals);
  value = std::string_view(part.data() + equals + 1, part.size() - equals - 1);
  return true;
}

// Remembers which keys a spec already used. A field that may appear once is
// refused when it appears twice, because the caller would be stating two
// different facts in one field.
class FieldKeys final {
 public:
  [[nodiscard]] bool claim(std::string_view key, std::string& error) {
    for (const std::string& seen : seen_) {
      if (seen == key) {
        error = "field '" + std::string(key) + "' appears more than once";
        return false;
      }
    }
    seen_.emplace_back(key);
    return true;
  }

 private:
  std::vector<std::string> seen_;
};

// The fields a composition member and an inventory entry have in common.
struct PlacementFields {
  DeviceId device;
  AssetId asset;
  FirmwareBaselineId firmware_baseline;
  TraitSet traits;
  SlotCoordinate slot;
  DisplayLabel label;
  bool readable = true;
};

// A member spec is device=<id>,unit=<n>,slot=<n> plus optional asset, baseline,
// label, readable and any number of traits. The device, unit and slot are
// required, because a member without a position cannot be closed against an
// inventory.
[[nodiscard]] bool parse_placement_spec(std::string_view text, PlacementFields& out,
                                        bool allow_readable, std::string& error) {
  FieldKeys keys;
  std::uint64_t unit = 0;
  std::uint64_t slot = 0;
  bool have_unit = false;
  bool have_slot = false;
  std::vector<Trait> traits;
  for (const std::string& part : split(text, ',')) {
    std::string_view key;
    std::string_view value;
    if (!split_field(part, key, value, error)) {
      return false;
    }
    if (key != "trait" && !keys.claim(key, error)) {
      return false;
    }
    if (key == "device") {
      const Result<DeviceId> parsed = DeviceId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.device = parsed.value();
    } else if (key == "unit") {
      if (!parse_u64(value, unit)) {
        error = "unit is not a non-negative decimal integer";
        return false;
      }
      have_unit = true;
    } else if (key == "slot") {
      if (!parse_u64(value, slot)) {
        error = "slot is not a non-negative decimal integer";
        return false;
      }
      have_slot = true;
    } else if (key == "asset") {
      const Result<AssetId> parsed = AssetId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.asset = parsed.value();
    } else if (key == "baseline") {
      const Result<FirmwareBaselineId> parsed = FirmwareBaselineId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.firmware_baseline = parsed.value();
    } else if (key == "label") {
      const Result<DisplayLabel> parsed = parse_label_or_none(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.label = parsed.value();
    } else if (key == "trait") {
      const Result<Trait> parsed = Trait::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      traits.push_back(parsed.value());
    } else if (allow_readable && key == "readable") {
      if (value == "true") {
        out.readable = true;
      } else if (value == "false") {
        out.readable = false;
      } else {
        error = "readable must be 'true' or 'false', found '" + std::string(value) + "'";
        return false;
      }
    } else {
      error = "unknown member field '" + std::string(key) + "'";
      return false;
    }
  }
  if (out.device.empty()) {
    error = "a member spec must declare device";
    return false;
  }
  if (!have_unit || !have_slot) {
    error = "a member spec must declare unit and slot";
    return false;
  }
  const Result<SlotCoordinate> coordinate = make_slot_coordinate(unit, slot);
  if (!coordinate.has_value()) {
    error = coordinate.error().message;
    return false;
  }
  out.slot = coordinate.value();
  const Result<TraitSet> set = TraitSet::create(std::move(traits));
  if (!set.has_value()) {
    error = set.error().message;
    return false;
  }
  out.traits = set.value();
  return true;
}

[[nodiscard]] bool parse_member_spec(std::string_view text, CompositionMember& out,
                                     std::string& error) {
  PlacementFields fields;
  if (!parse_placement_spec(text, fields, false, error)) {
    return false;
  }
  out.device = fields.device;
  out.asset = fields.asset;
  out.firmware_baseline = fields.firmware_baseline;
  out.traits = fields.traits;
  out.slot = fields.slot;
  out.label = fields.label;
  return true;
}

[[nodiscard]] bool parse_inventory_spec(std::string_view text, InventoryEntry& out,
                                        std::string& error) {
  PlacementFields fields;
  if (!parse_placement_spec(text, fields, true, error)) {
    return false;
  }
  out.device = fields.device;
  out.asset = fields.asset;
  out.firmware_baseline = fields.firmware_baseline;
  out.traits = fields.traits;
  out.slot = fields.slot;
  out.readable = fields.readable;
  return true;
}

// A compatibility spec states exactly the fields its kind uses; the library
// factory decides whether the combination is possible.
[[nodiscard]] bool parse_compatibility_spec(std::string_view text,
                                            CompatibilityRequirement& out, std::string& error) {
  FieldKeys keys;
  RequirementKind kind = RequirementKind::TraitOnEveryMember;
  bool have_kind = false;
  bool mandatory = true;
  Trait trait;
  DeviceId device;
  FirmwareBaselineId baseline;
  Note note;
  for (const std::string& part : split(text, ',')) {
    std::string_view key;
    std::string_view value;
    if (!split_field(part, key, value, error)) {
      return false;
    }
    if (!keys.claim(key, error)) {
      return false;
    }
    if (key == "kind") {
      const Result<RequirementKind> parsed = requirement_kind_from_name(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      kind = parsed.value();
      have_kind = true;
    } else if (key == "trait") {
      const Result<Trait> parsed = Trait::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      trait = parsed.value();
    } else if (key == "device") {
      const Result<DeviceId> parsed = DeviceId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      device = parsed.value();
    } else if (key == "baseline") {
      const Result<FirmwareBaselineId> parsed = FirmwareBaselineId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      baseline = parsed.value();
    } else if (key == "mandatory") {
      if (value == "true") {
        mandatory = true;
      } else if (value == "false") {
        mandatory = false;
      } else {
        error = "mandatory must be 'true' or 'false', found '" + std::string(value) + "'";
        return false;
      }
    } else if (key == "note") {
      const Result<Note> parsed = parse_note_or_none(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      note = parsed.value();
    } else {
      error = "unknown compatibility field '" + std::string(key) + "'";
      return false;
    }
  }
  if (!have_kind) {
    error = "a compatibility spec must declare kind";
    return false;
  }
  const Result<CompatibilityRequirement> requirement =
      make_compatibility_requirement(kind, trait, device, baseline, mandatory, note);
  if (!requirement.has_value()) {
    error = requirement.error().message;
    return false;
  }
  out = requirement.value();
  return true;
}

// What one member of the rack actually is, as reported by compatibility
// evidence: device=<id> with an optional baseline and any number of traits.
[[nodiscard]] bool parse_membership_spec(std::string_view text, CompatibilityMembership& out,
                                         std::string& error) {
  FieldKeys keys;
  std::vector<Trait> traits;
  for (const std::string& part : split(text, ',')) {
    std::string_view key;
    std::string_view value;
    if (!split_field(part, key, value, error)) {
      return false;
    }
    if (key != "trait" && !keys.claim(key, error)) {
      return false;
    }
    if (key == "device") {
      const Result<DeviceId> parsed = DeviceId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.device = parsed.value();
    } else if (key == "baseline") {
      const Result<FirmwareBaselineId> parsed = FirmwareBaselineId::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      out.firmware_baseline = parsed.value();
    } else if (key == "trait") {
      const Result<Trait> parsed = Trait::parse(value);
      if (!parsed.has_value()) {
        error = parsed.error().message;
        return false;
      }
      traits.push_back(parsed.value());
    } else {
      error = "unknown member field '" + std::string(key) + "'";
      return false;
    }
  }
  if (out.device.empty()) {
    error = "a member spec must declare device";
    return false;
  }
  const Result<TraitSet> set = TraitSet::create(std::move(traits));
  if (!set.has_value()) {
    error = set.error().message;
    return false;
  }
  out.traits = set.value();
  return true;
}

// A health sample is written device=status, for example dev:sw1=passing.
[[nodiscard]] bool parse_health_sample(std::string_view text, DeviceHealthSample& out,
                                       std::string& error) {
  const std::size_t equals = text.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    error = "a health sample must be written device=status, found '" + std::string(text) + "'";
    return false;
  }
  const Result<DeviceId> device = DeviceId::parse(text.substr(0, equals));
  if (!device.has_value()) {
    error = device.error().message;
    return false;
  }
  const Result<HealthStatus> status = health_status_from_name(text.substr(equals + 1));
  if (!status.has_value()) {
    error = status.error().message;
    return false;
  }
  out.device = device.value();
  out.status = status.value();
  return true;
}

// ---------------------------------------------------------------------------
// Option reader
// ---------------------------------------------------------------------------

// Reads the option tokens of one command. Taking an option consumes it, and
// every value reader consumes exactly one following token, so a missing value
// is reported instead of being taken from the next option.
class FlagReader final {
 public:
  explicit FlagReader(const std::vector<std::string>& tokens) : tokens_(tokens) {}

  [[nodiscard]] bool at_end() const noexcept { return index_ >= tokens_.size(); }
  [[nodiscard]] const std::string& peek() const noexcept { return tokens_[index_]; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  [[nodiscard]] bool take(std::string_view name) {
    if (at_end() || peek() != name) {
      return false;
    }
    ++index_;
    current_ = name;
    return true;
  }

  [[nodiscard]] bool flag(bool& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    if (value == "true") {
      out = true;
      return true;
    }
    if (value == "false") {
      out = false;
      return true;
    }
    return fail("expected 'true' or 'false', found '" + value + "'");
  }

  [[nodiscard]] bool u64(std::uint64_t& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    if (!parse_u64(value, out)) {
      return fail("expected a non-negative decimal integer, found '" + value + "'");
    }
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& out) {
    std::uint64_t value = 0;
    if (!u64(value)) {
      return false;
    }
    if (value > 0xFFFFFFFFull) {
      return fail("expected an integer no greater than 4294967295, found '" +
                  std::to_string(value) + "'");
    }
    out = static_cast<std::uint32_t>(value);
    return true;
  }

  template <typename Identity>
  [[nodiscard]] bool identity(Identity& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<Identity> parsed = Identity::parse(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  template <typename Counter>
  [[nodiscard]] bool counter(Counter& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::uint64_t number = 0;
    if (!parse_u64(value, number)) {
      return fail("expected a non-negative decimal integer, found '" + value + "'");
    }
    const Result<Counter> parsed = Counter::create(number);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool label(DisplayLabel& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<DisplayLabel> parsed = parse_label_or_none(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool note(Note& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<Note> parsed = parse_note_or_none(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool duration(Millis& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<Millis> parsed = parse_duration(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool timestamp(WallClock& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<WallClock> parsed = WallClock::parse(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool digest(Digest& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<Digest> parsed = Digest::parse(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool outcome(ActivationOutcome& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    const Result<ActivationOutcome> parsed = activation_outcome_from_name(value);
    if (!parsed.has_value()) {
      return fail(parsed.error().message);
    }
    out = parsed.value();
    return true;
  }

  [[nodiscard]] bool member_spec(CompositionMember& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::string detail;
    if (!parse_member_spec(value, out, detail)) {
      return fail(detail);
    }
    return true;
  }

  [[nodiscard]] bool inventory_spec(InventoryEntry& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::string detail;
    if (!parse_inventory_spec(value, out, detail)) {
      return fail(detail);
    }
    return true;
  }

  [[nodiscard]] bool compatibility_spec(CompatibilityRequirement& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::string detail;
    if (!parse_compatibility_spec(value, out, detail)) {
      return fail(detail);
    }
    return true;
  }

  [[nodiscard]] bool membership_spec(CompatibilityMembership& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::string detail;
    if (!parse_membership_spec(value, out, detail)) {
      return fail(detail);
    }
    return true;
  }

  [[nodiscard]] bool health_sample(DeviceHealthSample& out) {
    std::string value;
    if (!raw(value)) {
      return false;
    }
    std::string detail;
    if (!parse_health_sample(value, out, detail)) {
      return fail(detail);
    }
    return true;
  }

 private:
  [[nodiscard]] bool raw(std::string& out) {
    if (index_ >= tokens_.size()) {
      error_ = "option " + current_ + " requires a value";
      return false;
    }
    out = tokens_[index_];
    ++index_;
    return true;
  }

  [[nodiscard]] bool fail(std::string message) {
    error_ = current_ + ": " + std::move(message);
    return false;
  }

  const std::vector<std::string>& tokens_;
  std::size_t index_ = 0;
  std::string current_;
  std::string error_;
};

// Reports a token the command does not accept. The wording separates a
// misspelled option from a stray positional argument.
[[nodiscard]] int unexpected(const FlagReader& flags, std::ostream& err) {
  const std::string& token = flags.peek();
  if (!token.empty() && token.front() == '-') {
    return usage_error("unknown option '" + token + "'", err);
  }
  return usage_error("unexpected argument '" + token + "'", err);
}

[[nodiscard]] int emit(const Invocation& invocation, const Value& report) {
  emit_report(report, invocation.options.json, invocation.out);
  return kExitSuccess;
}

// ---------------------------------------------------------------------------
// State the command line reads before it mutates
// ---------------------------------------------------------------------------

// The CLI holds the writer lock for the whole run, so reading the current plan
// revision and then mutating is a read-then-write against one authoritative
// state. A revision is never invented: when the rack or its plan is absent the
// revision stays unset and the service reports the real reason.
[[nodiscard]] PlanRevision plan_revision_of(const TurnupService& service, const RackId& rack) {
  const RackState* state = service.state().find_rack(rack);
  if (state == nullptr) {
    return PlanRevision{};
  }
  const RackTurnupPlan* plan = state->active_plan();
  return (plan == nullptr) ? PlanRevision{} : plan->revision;
}

[[nodiscard]] RackGeneration rack_generation_of(const TurnupService& service, const RackId& rack) {
  const RackState* state = service.state().find_rack(rack);
  return (state == nullptr) ? RackGeneration::initial() : state->rack.generation;
}

[[nodiscard]] CompositionGeneration composition_generation_of(const TurnupService& service,
                                                              const RackId& rack) {
  const RackState* state = service.state().find_rack(rack);
  return (state == nullptr) ? CompositionGeneration::initial()
                            : state->rack.composition_generation();
}

[[nodiscard]] PlanRevision choose_revision(const std::optional<PlanRevision>& given,
                                           const TurnupService& service, const RackId& rack) {
  return given.has_value() ? *given : plan_revision_of(service, rack);
}

// ---------------------------------------------------------------------------
// Report builders
// ---------------------------------------------------------------------------

// Registration, re-composition, plan creation and the summary command all
// answer with the same shape, so an operator reads one report and knows how to
// read the rest.
[[nodiscard]] Value make_rack_summary(const RackSummary& summary) {
  Value report;
  report.put("rack", text_value(summary.id.text()));
  report.put("label", text_value(summary.label.text()));
  report.put("site", text_value(summary.site.text()));
  report.put("lifecycle", text_value(rack_lifecycle_state_name(summary.lifecycle)));
  report.put("rack_generation", counter_value(summary.generation));
  report.put("composition_generation", counter_value(summary.composition_generation));
  report.put("composition", text_value(summary.composition.to_hex()));
  report.put("members", number_value(static_cast<std::uint64_t>(summary.member_count)));
  report.put("has_plan", flag_value(summary.has_plan));
  report.put("plan", text_value(summary.plan.text()));
  report.put("plan_lifetime", counter_value(summary.lifetime));
  report.put("revision", counter_value(summary.revision));
  report.put("plan_state", text_value(plan_state_name(summary.plan_state)));
  report.put("verdict", text_value(turnup_verdict_name(summary.verdict)));
  report.put("blockers", number_value(static_cast<std::uint64_t>(summary.blocker_count)));
  report.put("eligible_evidence",
             number_value(static_cast<std::uint64_t>(summary.eligible_evidence)));
  report.put("registered_at", text_value(summary.registered_at.to_text()));
  report.put("authority_time", text_value(summary.authority_time.to_text()));
  return report;
}

[[nodiscard]] Value blocker_list(const std::vector<Blocker>& blockers) {
  Value list = list_value();
  for (const Blocker& blocker : blockers) {
    Value record;
    record.put("severity", text_value(blocker_severity_name(blocker.severity)));
    record.put("code", text_value(code_name(blocker.code)));
    record.put("stage", text_value(stage_kind_name(blocker.stage)));
    record.put("subsystem", text_value(blocker.has_subsystem
                                           ? subsystem_kind_name(blocker.subsystem)
                                           : std::string_view("-")));
    record.put("subject", text_value(blocker.subject));
    record.put("explanation", text_value(blocker.explanation));
    list.add(std::move(record));
  }
  return list;
}

// Adds the one payload field the record's kind makes meaningful.
void add_payload_fields(Value& record, const EvidencePayload& payload) {
  record.put("payload", text_value(evidence_payload_kind_name(payload.kind)));
  switch (payload.kind) {
    case EvidencePayloadKind::IdentityComposition:
      record.put("registry_composition",
                 text_value(payload.identity.registry_composition.to_hex()));
      record.put("declared_members", number_value(payload.identity.declared_members));
      record.put("unreadable_members", number_value(payload.identity.unreadable_members));
      break;
    case EvidencePayloadKind::Power:
      record.put("domain", text_value(payload.power.domain.text()));
      record.put("available_mw", number_value(payload.power.available_milliwatts));
      record.put("applied_mw", number_value(payload.power.applied_milliwatts));
      record.put("energized_circuits", number_value(payload.power.energized_circuits));
      record.put("redundant", flag_value(payload.power.redundant_feeds_present));
      record.put("applied_verified", flag_value(payload.power.applied_verified));
      break;
    case EvidencePayloadKind::Cooling:
      record.put("domain", text_value(payload.cooling.domain.text()));
      record.put("capacity_mw", number_value(payload.cooling.capacity_milliwatts));
      record.put("delivered_mw", number_value(payload.cooling.delivered_milliwatts));
      record.put("inlet_mc", number_value(payload.cooling.inlet_millidegrees_c));
      record.put("max_inlet_mc", number_value(payload.cooling.max_inlet_millidegrees_c));
      record.put("delivery_verified", flag_value(payload.cooling.delivery_verified));
      break;
    case EvidencePayloadKind::Network:
      record.put("domain", text_value(payload.network.domain.text()));
      record.put("topology", text_value(payload.network.topology.text()));
      record.put("attached_ports", number_value(payload.network.attached_ports));
      record.put("reachable_ports", number_value(payload.network.reachable_ports));
      record.put("authority_verified", flag_value(payload.network.authority_verified));
      break;
    case EvidencePayloadKind::Inventory:
      record.put("entry_count",
                 number_value(static_cast<std::uint64_t>(payload.inventory.entries.size())));
      record.put("unreadable_positions", number_value(payload.inventory.unreadable_positions));
      break;
    case EvidencePayloadKind::Health:
      record.put("sample_count",
                 number_value(static_cast<std::uint64_t>(payload.health.samples.size())));
      record.put("unchecked", number_value(payload.health.unchecked_devices));
      break;
    case EvidencePayloadKind::Compatibility:
      record.put("member_count",
                 number_value(static_cast<std::uint64_t>(payload.compatibility.members.size())));
      break;
  }
}

[[nodiscard]] Value make_evidence_record(const EvidenceRecord& evidence) {
  Value record;
  record.put("id", text_value(evidence.id.text()));
  record.put("subsystem", text_value(subsystem_kind_name(evidence.subsystem)));
  record.put("sequence", counter_value(evidence.sequence));
  record.put("plan", text_value(evidence.plan.text()));
  record.put("plan_revision", counter_value(evidence.plan_revision));
  record.put("observed_at", text_value(evidence.observed_at.to_text()));
  record.put("validity_ms",
             number_value(static_cast<std::uint64_t>(evidence.validity.milliseconds())));
  record.put("source", text_value(evidence.source.text()));
  record.put("producer", text_value(evidence.producer.text()));
  record.put("note", text_value(evidence.note.text()));
  record.put("composition", text_value(evidence.composition.to_hex()));
  record.put("digest", text_value(evidence.record_digest.to_hex()));
  add_payload_fields(record, evidence.payload);
  return record;
}

[[nodiscard]] Value make_provenance_record(const ProvenanceRecord& record) {
  Value value;
  value.put("operation", text_value(operation_kind_name(record.operation)));
  value.put("rack", text_value(record.rack.text()));
  value.put("plan", text_value(record.plan.text()));
  value.put("actor", text_value(record.actor.text()));
  value.put("source", text_value(record.source.text()));
  value.put("at", text_value(record.at.to_text()));
  value.put("revision", counter_value(record.revision));
  value.put("sequence", counter_value(record.sequence));
  value.put("request", text_value(record.request.to_hex()));
  value.put("note", text_value(record.note.text()));
  return value;
}

[[nodiscard]] Value make_rejection_record(const RejectionRecord& record) {
  Value value;
  value.put("code", text_value(code_name(record.code)));
  value.put("operation", text_value(operation_kind_name(record.operation)));
  value.put("rack", text_value(record.rack.text()));
  value.put("plan", text_value(record.plan.text()));
  value.put("message", text_value(record.message));
  value.put("at", text_value(record.at.to_text()));
  value.put("actor", text_value(record.actor.text()));
  value.put("request", text_value(record.request.to_hex()));
  return value;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

[[nodiscard]] int cmd_rack_register(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RegisterRackRequest request;
  request.context = make_context(invocation.options);
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--site")) {
      if (!flags.identity(request.site)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--label")) {
      if (!flags.label(request.label)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--member")) {
      CompositionMember member;
      if (!flags.member_spec(member)) {
        return usage_error(flags.error(), invocation.err);
      }
      request.members.push_back(std::move(member));
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (request.members.empty()) {
    return usage_error("at least one --member is required", invocation.err);
  }
  const Result<RackSummary> summary = service.register_rack(request);
  if (!summary.has_value()) {
    return report_error(summary.error(), invocation.err);
  }
  return emit(invocation, make_rack_summary(summary.value()));
}

[[nodiscard]] int cmd_rack_composition(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  UpdateCompositionRequest request;
  request.context = make_context(invocation.options);
  std::optional<CompositionGeneration> generation;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--generation")) {
      CompositionGeneration value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      generation = value;
    } else if (flags.take("--member")) {
      CompositionMember member;
      if (!flags.member_spec(member)) {
        return usage_error(flags.error(), invocation.err);
      }
      request.members.push_back(std::move(member));
    } else if (flags.take("--replace-commissioned")) {
      request.replace_commissioned = true;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (!generation.has_value()) {
    return usage_error("missing required option --generation", invocation.err);
  }
  if (request.members.empty()) {
    return usage_error("at least one --member is required", invocation.err);
  }
  request.composition_generation = *generation;
  request.expected_rack_generation = rack_generation_of(service, request.rack);
  const Result<RackSummary> summary = service.update_composition(request);
  if (!summary.has_value()) {
    return report_error(summary.error(), invocation.err);
  }
  return emit(invocation, make_rack_summary(summary.value()));
}

[[nodiscard]] int cmd_rack_list(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  const Result<std::vector<RackId>> ids = service.rack_ids();
  if (!ids.has_value()) {
    return report_error(ids.error(), invocation.err);
  }
  Value report;
  report.put("rack_count", number_value(static_cast<std::uint64_t>(ids.value().size())));
  Value racks = list_value();
  for (const RackId& id : ids.value()) {
    const Result<RackSummary> summary = service.summary(id, invocation.options.authority_time);
    if (!summary.has_value()) {
      return report_error(summary.error(), invocation.err);
    }
    racks.add(make_rack_summary(summary.value()));
  }
  report.put("racks", std::move(racks));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_plan_create(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  CreatePlanRequest request;
  request.context = make_context(invocation.options);
  PlanRequirements requirements;
  TurnupPolicy policy;
  policy.generation = PolicyGeneration::initial();
  GenerationStamp stamp;
  stamp.rack = RackGeneration::initial();
  stamp.composition = CompositionGeneration::initial();
  stamp.topology = TopologyGeneration::initial();
  stamp.dependency = DependencyGeneration::initial();
  stamp.power = PowerGeneration::initial();
  stamp.cooling = CoolingGeneration::initial();
  stamp.network = NetworkGeneration::initial();
  stamp.inventory = InventoryGeneration::initial();
  stamp.health = HealthGeneration::initial();
  stamp.capacity = CapacityGeneration::initial();
  stamp.maintenance = MaintenanceGeneration::initial();
  stamp.policy = policy.generation;
  stamp.firmware = FirmwareGeneration::initial();
  std::optional<RackGeneration> rack_generation;
  std::optional<CompositionGeneration> composition_generation;
  bool cancel_authorization = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--rack-generation")) {
      RackGeneration value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      rack_generation = value;
    } else if (flags.take("--composition-generation")) {
      CompositionGeneration value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      composition_generation = value;
    } else if (flags.take("--topology-generation")) {
      if (!flags.counter(stamp.topology)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--dependency-generation")) {
      if (!flags.counter(stamp.dependency)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--power-generation")) {
      if (!flags.counter(stamp.power)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--cooling-generation")) {
      if (!flags.counter(stamp.cooling)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--network-generation")) {
      if (!flags.counter(stamp.network)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--inventory-generation")) {
      if (!flags.counter(stamp.inventory)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--health-generation")) {
      if (!flags.counter(stamp.health)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--capacity-generation")) {
      if (!flags.counter(stamp.capacity)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--maintenance-generation")) {
      if (!flags.counter(stamp.maintenance)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--policy-generation")) {
      if (!flags.counter(policy.generation)) {
        return usage_error(flags.error(), invocation.err);
      }
      stamp.policy = policy.generation;
    } else if (flags.take("--firmware-generation")) {
      if (!flags.counter(stamp.firmware)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--require-power-mw")) {
      if (!flags.u64(requirements.required_power_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--require-cooling-mw")) {
      if (!flags.u64(requirements.required_cooling_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--require-network-ports")) {
      if (!flags.u32(requirements.required_network_ports)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--require-redundant-feeds")) {
      requirements.require_redundant_feeds = true;
    } else if (flags.take("--require-topology")) {
      if (!flags.identity(requirements.required_topology)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--no-applied-power-verification")) {
      requirements.require_applied_power_verification = false;
    } else if (flags.take("--no-cooling-delivery-verification")) {
      requirements.require_cooling_delivery_verification = false;
    } else if (flags.take("--no-network-authority-verification")) {
      requirements.require_network_authority_verification = false;
    } else if (flags.take("--health-not-required-for-every-member")) {
      requirements.require_health_pass_for_every_member = false;
    } else if (flags.take("--allow-degraded")) {
      policy.allow_degraded_subsystems = true;
    } else if (flags.take("--authorization-validity")) {
      if (!flags.duration(policy.authorization_validity)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--default-evidence-validity")) {
      if (!flags.duration(policy.default_evidence_validity)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--compat")) {
      CompatibilityRequirement requirement;
      if (!flags.compatibility_spec(requirement)) {
        return usage_error(flags.error(), invocation.err);
      }
      requirements.compatibility.push_back(std::move(requirement));
    } else if (flags.take("--supersede") || flags.take("--cancel-authorization")) {
      cancel_authorization = true;
    } else if (flags.take("--replace-commissioned")) {
      request.replace_commissioned = true;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const RackGeneration rack_value =
      rack_generation.has_value() ? *rack_generation : rack_generation_of(service, request.rack);
  const CompositionGeneration composition_value =
      composition_generation.has_value() ? *composition_generation
                                         : composition_generation_of(service, request.rack);
  stamp.rack = rack_value;
  stamp.composition = composition_value;
  request.expected_rack_generation = rack_value;
  request.stamp = stamp;
  request.cancel_active_authorization = cancel_authorization;
  const Result<PlanRequirements> validated = make_plan_requirements(requirements);
  if (!validated.has_value()) {
    return usage_error(validated.error().message, invocation.err);
  }
  const Result<TurnupPolicy> validated_policy = make_turnup_policy(policy);
  if (!validated_policy.has_value()) {
    return usage_error(validated_policy.error().message, invocation.err);
  }
  request.requirements = validated.value();
  request.policy = validated_policy.value();
  const Result<RackSummary> summary = service.create_plan(request);
  if (!summary.has_value()) {
    return report_error(summary.error(), invocation.err);
  }
  return emit(invocation, make_rack_summary(summary.value()));
}

[[nodiscard]] int run_import(const Invocation& invocation, TurnupService& service,
                             const ImportEvidenceRequest& request) {
  const Result<EvidenceReceipt> receipt = service.import_evidence(request);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("evidence", text_value(receipt.value().id.text()));
  report.put("sequence", counter_value(receipt.value().sequence));
  report.put("revision", counter_value(receipt.value().revision));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

// Every evidence command builds the same request: the payload field that
// matches the subsystem, the plan's own generation stamp and validity policy,
// and the revision the plan is expected to hold.
[[nodiscard]] int cmd_evidence_power(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Power;
  request.payload.kind = EvidencePayloadKind::Power;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  bool have_domain = false;
  bool have_available = false;
  bool have_applied = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--domain")) {
      if (!flags.identity(request.payload.power.domain)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_domain = true;
    } else if (flags.take("--available-mw")) {
      if (!flags.u64(request.payload.power.available_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_available = true;
    } else if (flags.take("--applied-mw")) {
      if (!flags.u64(request.payload.power.applied_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_applied = true;
    } else if (flags.take("--energized-circuits")) {
      if (!flags.u32(request.payload.power.energized_circuits)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--redundant")) {
      request.payload.power.redundant_feeds_present = true;
    } else if (flags.take("--applied-verified")) {
      request.payload.power.applied_verified = true;
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (!have_domain) {
    return usage_error("missing required option --domain", invocation.err);
  }
  if (!have_available) {
    return usage_error("missing required option --available-mw", invocation.err);
  }
  if (!have_applied) {
    return usage_error("missing required option --applied-mw", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_cooling(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Cooling;
  request.payload.kind = EvidencePayloadKind::Cooling;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  bool have_domain = false;
  bool have_capacity = false;
  bool have_delivered = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--domain")) {
      if (!flags.identity(request.payload.cooling.domain)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_domain = true;
    } else if (flags.take("--capacity-mw")) {
      if (!flags.u64(request.payload.cooling.capacity_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_capacity = true;
    } else if (flags.take("--delivered-mw")) {
      if (!flags.u64(request.payload.cooling.delivered_milliwatts)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_delivered = true;
    } else if (flags.take("--inlet-mc")) {
      if (!flags.u32(request.payload.cooling.inlet_millidegrees_c)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--max-inlet-mc")) {
      if (!flags.u32(request.payload.cooling.max_inlet_millidegrees_c)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--delivery-verified")) {
      request.payload.cooling.delivery_verified = true;
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (!have_domain) {
    return usage_error("missing required option --domain", invocation.err);
  }
  if (!have_capacity) {
    return usage_error("missing required option --capacity-mw", invocation.err);
  }
  if (!have_delivered) {
    return usage_error("missing required option --delivered-mw", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_network(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Network;
  request.payload.kind = EvidencePayloadKind::Network;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  bool have_domain = false;
  bool have_attached = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--domain")) {
      if (!flags.identity(request.payload.network.domain)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_domain = true;
    } else if (flags.take("--topology")) {
      if (!flags.identity(request.payload.network.topology)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--attached-ports")) {
      if (!flags.u32(request.payload.network.attached_ports)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_attached = true;
    } else if (flags.take("--reachable-ports")) {
      if (!flags.u32(request.payload.network.reachable_ports)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--authority-verified")) {
      request.payload.network.authority_verified = true;
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (!have_domain) {
    return usage_error("missing required option --domain", invocation.err);
  }
  if (!have_attached) {
    return usage_error("missing required option --attached-ports", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_identity(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::IdentityComposition;
  request.payload.kind = EvidencePayloadKind::IdentityComposition;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  bool have_registry = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--registry-digest")) {
      if (!flags.digest(request.payload.identity.registry_composition)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_registry = true;
    } else if (flags.take("--declared-members")) {
      if (!flags.u32(request.payload.identity.declared_members)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--unreadable-members")) {
      if (!flags.u32(request.payload.identity.unreadable_members)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (!have_registry) {
    return usage_error("missing required option --registry-digest", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_inventory(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Inventory;
  request.payload.kind = EvidencePayloadKind::Inventory;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--entry")) {
      InventoryEntry entry;
      if (!flags.inventory_spec(entry)) {
        return usage_error(flags.error(), invocation.err);
      }
      request.payload.inventory.entries.push_back(std::move(entry));
    } else if (flags.take("--unreadable-positions")) {
      if (!flags.u32(request.payload.inventory.unreadable_positions)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_health(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Health;
  request.payload.kind = EvidencePayloadKind::Health;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--sample")) {
      DeviceHealthSample sample;
      if (!flags.health_sample(sample)) {
        return usage_error(flags.error(), invocation.err);
      }
      request.payload.health.samples.push_back(std::move(sample));
    } else if (flags.take("--unchecked")) {
      if (!flags.u32(request.payload.health.unchecked_devices)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_compatibility(const Invocation& invocation,
                                             TurnupService& service) {
  FlagReader flags(invocation.tokens);
  ImportEvidenceRequest request;
  request.context = make_context(invocation.options);
  request.subsystem = SubsystemKind::Compatibility;
  request.payload.kind = EvidencePayloadKind::Compatibility;
  request.validity_from_policy = true;
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--member-spec") || flags.take("--member")) {
      CompatibilityMembership membership;
      if (!flags.membership_spec(membership)) {
        return usage_error(flags.error(), invocation.err);
      }
      request.payload.compatibility.members.push_back(std::move(membership));
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  return run_import(invocation, service, request);
}

[[nodiscard]] int cmd_evidence_list(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RackId rack;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const Result<std::vector<EvidenceRecord>> records = service.evidence(rack);
  if (!records.has_value()) {
    return report_error(records.error(), invocation.err);
  }
  Value report;
  report.put("rack", text_value(rack.text()));
  report.put("evidence_count", number_value(static_cast<std::uint64_t>(records.value().size())));
  Value list = list_value();
  for (const EvidenceRecord& record : records.value()) {
    list.add(make_evidence_record(record));
  }
  report.put("evidence", std::move(list));
  return emit(invocation, report);
}

// The library renders one subsystem verdict as the deterministic line
// "name=state evidence=... code=...". The report carries that same text under
// the subsystem name, so the text form of a subsystem line is byte for byte the
// rendering the library documents.
[[nodiscard]] std::string subsystem_line_value(const SubsystemVerdict& verdict) {
  const std::string line = describe_subsystem_verdict(verdict);
  const std::size_t equals = line.find('=');
  return (equals == std::string::npos) ? line : line.substr(equals + 1);
}

// The evaluate report follows the shape the domain defines: the verdict, the
// stage ladder in order, the subsystem verdicts, the two digests, then the
// blockers from most severe first. Everything else the evaluation knows follows
// them, so the required prefix is stable.
[[nodiscard]] int cmd_evaluate(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RackId rack;
  std::optional<WallClock> at;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--at")) {
      WallClock value;
      if (!flags.timestamp(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      at = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const WallClock authority_time = at.has_value() ? *at : invocation.options.authority_time;
  const Result<Evaluation> evaluation = service.evaluate(rack, authority_time);
  if (!evaluation.has_value()) {
    return report_error(evaluation.error(), invocation.err);
  }
  const Evaluation& answer = evaluation.value();
  Value report;
  report.put("verdict", text_value(turnup_verdict_name(answer.verdict)));
  for (const StageKind stage : plan_stage_ladder()) {
    report.put(std::string(stage_kind_name(stage)),
               text_value(stage_state_name(answer.stage(stage).state)));
  }
  for (const SubsystemKind subsystem : all_subsystem_kinds()) {
    report.put(std::string(subsystem_kind_name(subsystem)),
               text_value(subsystem_line_value(answer.subsystem(subsystem))));
  }
  report.put("evidence_set", text_value(answer.evidence_set.to_hex()));
  report.put("verdict_digest", text_value(answer.verdict_digest.to_hex()));
  report.put("blockers", blocker_list(answer.blockers));
  report.put("suppressed_blockers",
             number_value(static_cast<std::uint64_t>(answer.suppressed_blockers)));
  report.put("blocker_count", number_value(static_cast<std::uint64_t>(answer.blockers.size())));
  report.put("rack", text_value(answer.rack.text()));
  report.put("site", text_value(answer.site.text()));
  report.put("lifecycle", text_value(rack_lifecycle_state_name(answer.lifecycle)));
  report.put("rack_generation", counter_value(answer.rack_generation));
  report.put("composition_generation", counter_value(answer.composition_generation));
  report.put("composition", text_value(answer.composition.to_hex()));
  report.put("plan", text_value(answer.plan.text()));
  report.put("plan_lifetime", counter_value(answer.lifetime));
  report.put("revision", counter_value(answer.revision));
  report.put("policy_generation", counter_value(answer.policy));
  report.put("epoch", counter_value(answer.epoch));
  report.put("binding_matches", flag_value(answer.binding_matches));
  report.put("allow_degraded", flag_value(answer.allow_degraded));
  report.put("authority_time", text_value(answer.authority_time.to_text()));
  report.put("explanation", text_value(answer.explanation));
  report.put("authorization_present", flag_value(answer.authorization.present));
  report.put("authorization_attempt", text_value(answer.authorization.attempt.text()));
  report.put("authorization_state",
             text_value(authorization_state_name(answer.authorization.state)));
  report.put("activation_observed", flag_value(answer.activation_observed));
  report.put("commissioned", flag_value(answer.commissioned));
  report.put("eligible_evidence",
             number_value(static_cast<std::uint64_t>(answer.eligible_evidence)));
  report.put("superseded_evidence",
             number_value(static_cast<std::uint64_t>(answer.superseded_evidence)));
  report.put("rejected_evidence",
             number_value(static_cast<std::uint64_t>(answer.rejected_evidence)));
  report.put("expected_members", counter_value(answer.expected_members));
  report.put("observed_members", counter_value(answer.observed_members));
  report.put("healthy_members", counter_value(answer.healthy_members));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_blockers(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RackId rack;
  std::optional<WallClock> at;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--at")) {
      WallClock value;
      if (!flags.timestamp(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      at = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const WallClock authority_time = at.has_value() ? *at : invocation.options.authority_time;
  const Result<BlockerReport> report = service.blockers(rack, authority_time);
  if (!report.has_value()) {
    return report_error(report.error(), invocation.err);
  }
  const BlockerReport& answer = report.value();
  Value value;
  value.put("rack", text_value(answer.rack.text()));
  value.put("plan", text_value(answer.plan.text()));
  value.put("revision", counter_value(answer.revision));
  value.put("authority_time", text_value(answer.authority_time.to_text()));
  value.put("verdict", text_value(turnup_verdict_name(answer.verdict)));
  value.put("blocker_count", number_value(static_cast<std::uint64_t>(answer.blockers.size())));
  value.put("suppressed", number_value(static_cast<std::uint64_t>(answer.suppressed)));
  value.put("blockers", blocker_list(answer.blockers));
  return emit(invocation, value);
}

[[nodiscard]] int cmd_authorize(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  AuthorizeRequest request;
  request.context = make_context(invocation.options);
  std::optional<PlanRevision> expected_revision;
  bool have_validity = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--validity")) {
      if (!flags.duration(request.validity)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_validity = true;
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.validity_from_policy = !have_validity;
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  const Result<AuthorizationReceipt> receipt = service.authorize_turnup(request);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("attempt", text_value(receipt.value().attempt.text()));
  report.put("plan", text_value(receipt.value().plan.text()));
  report.put("revision", counter_value(receipt.value().revision));
  report.put("verdict", text_value(receipt.value().verdict.to_hex()));
  report.put("evidence_set", text_value(receipt.value().evidence_set.to_hex()));
  report.put("evidence_high_water", counter_value(receipt.value().evidence_high_water));
  report.put("issued_at", text_value(receipt.value().issued_at.to_text()));
  report.put("expires_at", text_value(receipt.value().expires_at.to_text()));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_activate(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RecordActivationRequest request;
  request.context = make_context(invocation.options);
  request.observed_at = invocation.options.authority_time;
  std::optional<PlanRevision> expected_revision;
  bool have_validity = false;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--attempt")) {
      if (!flags.identity(request.attempt)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--outcome")) {
      if (!flags.outcome(request.outcome)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--observed-at")) {
      if (!flags.timestamp(request.observed_at)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--members")) {
      if (!flags.u32(request.active_members)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--validity")) {
      if (!flags.duration(request.validity)) {
        return usage_error(flags.error(), invocation.err);
      }
      have_validity = true;
    } else if (flags.take("--observed-composition")) {
      if (!flags.digest(request.observed_composition)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (request.attempt.empty()) {
    return usage_error("missing required option --attempt", invocation.err);
  }
  request.validity_from_policy = !have_validity;
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  const Result<ActivationReceipt> receipt = service.record_activation(request);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("attempt", text_value(receipt.value().attempt.text()));
  report.put("plan", text_value(receipt.value().plan.text()));
  report.put("revision", counter_value(receipt.value().revision));
  report.put("sequence", counter_value(receipt.value().sequence));
  report.put("outcome", text_value(activation_outcome_name(receipt.value().outcome)));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_commission(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  CommissionRequest request;
  request.context = make_context(invocation.options);
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--attempt")) {
      if (!flags.identity(request.attempt)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  if (request.attempt.empty()) {
    return usage_error("missing required option --attempt", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  const Result<CommissionReceipt> receipt = service.commission(request);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("attempt", text_value(receipt.value().attempt.text()));
  report.put("plan", text_value(receipt.value().plan.text()));
  report.put("revision", counter_value(receipt.value().revision));
  report.put("commissioned_at", text_value(receipt.value().commissioned_at.to_text()));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_rollback(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RollbackRequest request;
  request.context = make_context(invocation.options);
  request.reason = invocation.options.note;
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--reason")) {
      if (!flags.note(request.reason)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--acknowledge-active")) {
      request.acknowledge_active = true;
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  const Result<RollbackReceipt> receipt = service.rollback(request);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("revision", counter_value(receipt.value().revision));
  report.put("verdict", text_value(turnup_verdict_name(receipt.value().verdict)));
  report.put("authorizations_fenced",
             number_value(static_cast<std::uint64_t>(receipt.value().authorizations_fenced)));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

// The three lifecycle transitions differ only in the operation they name.
enum class LifecycleAction : std::uint8_t { BeginDrain, CompleteDrain, Decommission };

[[nodiscard]] int lifecycle_command(const Invocation& invocation, TurnupService& service,
                                    LifecycleAction action) {
  FlagReader flags(invocation.tokens);
  LifecycleRequest request;
  request.context = make_context(invocation.options);
  request.note = invocation.options.note;
  std::optional<PlanRevision> expected_revision;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(request.rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else if (flags.take("--expected-revision")) {
      PlanRevision value;
      if (!flags.counter(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      expected_revision = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (request.rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  request.expected_revision = choose_revision(expected_revision, service, request.rack);
  const auto apply = [&service, &request](LifecycleAction chosen) -> Result<LifecycleReceipt> {
    if (chosen == LifecycleAction::BeginDrain) {
      return service.begin_drain(request);
    }
    if (chosen == LifecycleAction::CompleteDrain) {
      return service.complete_drain(request);
    }
    return service.decommission(request);
  };
  const Result<LifecycleReceipt> receipt = apply(action);
  if (!receipt.has_value()) {
    return report_error(receipt.error(), invocation.err);
  }
  Value report;
  report.put("from", text_value(rack_lifecycle_state_name(receipt.value().from)));
  report.put("to", text_value(rack_lifecycle_state_name(receipt.value().to)));
  report.put("revision", counter_value(receipt.value().revision));
  report.put("replayed", flag_value(receipt.value().replayed));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_drain_begin(const Invocation& invocation, TurnupService& service) {
  return lifecycle_command(invocation, service, LifecycleAction::BeginDrain);
}

[[nodiscard]] int cmd_drain_complete(const Invocation& invocation, TurnupService& service) {
  return lifecycle_command(invocation, service, LifecycleAction::CompleteDrain);
}

[[nodiscard]] int cmd_decommission(const Invocation& invocation, TurnupService& service) {
  return lifecycle_command(invocation, service, LifecycleAction::Decommission);
}

[[nodiscard]] int cmd_take_control(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  TakeControlRequest request;
  request.context = make_context(invocation.options);
  const Result<TakeoverReport> report = service.take_control(request);
  if (!report.has_value()) {
    return report_error(report.error(), invocation.err);
  }
  Value value;
  value.put("previous_epoch", counter_value(report.value().previous));
  value.put("current_epoch", counter_value(report.value().current));
  value.put("authorizations_fenced",
            number_value(static_cast<std::uint64_t>(report.value().authorizations_fenced)));
  return emit(invocation, value);
}

[[nodiscard]] int cmd_recover(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  const Result<RecoveryReport> report = service.recover();
  if (!report.has_value()) {
    return report_error(report.error(), invocation.err);
  }
  const RecoveryReport& answer = report.value();
  Value value;
  value.put("racks", number_value(static_cast<std::uint64_t>(answer.racks)));
  value.put("fences_added", number_value(static_cast<std::uint64_t>(answer.fences_added)));
  value.put("plans_superseded",
            number_value(static_cast<std::uint64_t>(answer.plans_superseded)));
  value.put("store_epoch", counter_value(answer.store_epoch));
  value.put("store_sequence", counter_value(answer.store_sequence));
  value.put("incarnation", counter_value(answer.incarnation));
  value.put("control_epoch", counter_value(answer.control_epoch));
  value.put("observation_sequence", counter_value(answer.observation_sequence));
  value.put("published", flag_value(answer.published));
  Value notes = list_value();
  for (const std::string& note : answer.notes) {
    notes.add(text_value(note));
  }
  value.put("notes", std::move(notes));
  return emit(invocation, value);
}

[[nodiscard]] int cmd_summary(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  std::optional<RackId> rack;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      RackId value;
      if (!flags.identity(value)) {
        return usage_error(flags.error(), invocation.err);
      }
      rack = value;
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.has_value()) {
    const Result<RackSummary> summary = service.summary(*rack, invocation.options.authority_time);
    if (!summary.has_value()) {
      return report_error(summary.error(), invocation.err);
    }
    return emit(invocation, make_rack_summary(summary.value()));
  }
  const Result<ServiceSummary> summary = service.summary(invocation.options.authority_time);
  if (!summary.has_value()) {
    return report_error(summary.error(), invocation.err);
  }
  const ServiceSummary& answer = summary.value();
  Value report;
  report.put("rack_count", number_value(static_cast<std::uint64_t>(answer.rack_count)));
  report.put("registered", number_value(static_cast<std::uint64_t>(answer.registered)));
  report.put("planned", number_value(static_cast<std::uint64_t>(answer.planned)));
  report.put("authorized", number_value(static_cast<std::uint64_t>(answer.authorized)));
  report.put("active", number_value(static_cast<std::uint64_t>(answer.active)));
  report.put("commissioned", number_value(static_cast<std::uint64_t>(answer.commissioned)));
  report.put("draining", number_value(static_cast<std::uint64_t>(answer.draining)));
  report.put("drained", number_value(static_cast<std::uint64_t>(answer.drained)));
  report.put("decommissioned", number_value(static_cast<std::uint64_t>(answer.decommissioned)));
  report.put("blocked", number_value(static_cast<std::uint64_t>(answer.blocked)));
  report.put("store_epoch", counter_value(answer.store_epoch));
  report.put("store_sequence", counter_value(answer.store_sequence));
  report.put("incarnation", counter_value(answer.incarnation));
  report.put("control_epoch", counter_value(answer.control_epoch));
  report.put("observation_sequence", counter_value(answer.observation_sequence));
  report.put("created_at", text_value(answer.created_at.to_text()));
  report.put("updated_at", text_value(answer.updated_at.to_text()));
  report.put("authority_time", text_value(answer.authority_time.to_text()));
  report.put("rejection_journal",
             number_value(static_cast<std::uint64_t>(answer.rejection_journal)));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_authority_list(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RackId rack;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const Result<std::vector<TurnupAuthorization>> authorizations = service.authorizations(rack);
  if (!authorizations.has_value()) {
    return report_error(authorizations.error(), invocation.err);
  }
  const Result<std::vector<AuthorizationFence>> fences = service.fences(rack);
  if (!fences.has_value()) {
    return report_error(fences.error(), invocation.err);
  }
  Value report;
  report.put("rack", text_value(rack.text()));
  report.put("authorization_count",
             number_value(static_cast<std::uint64_t>(authorizations.value().size())));
  Value authorization_list = list_value();
  for (const TurnupAuthorization& authorization : authorizations.value()) {
    Value value;
    value.put("attempt", text_value(authorization.attempt.text()));
    value.put("plan", text_value(authorization.plan.text()));
    value.put("plan_lifetime", counter_value(authorization.lifetime));
    value.put("revision", counter_value(authorization.revision));
    value.put("composition", text_value(authorization.composition.to_hex()));
    value.put("epoch", counter_value(authorization.epoch));
    value.put("policy_generation", counter_value(authorization.policy));
    value.put("verdict", text_value(authorization.verdict.to_hex()));
    value.put("evidence_set", text_value(authorization.evidence_set.to_hex()));
    value.put("evidence_high_water", counter_value(authorization.evidence_high_water));
    value.put("issued_at", text_value(authorization.issued_at.to_text()));
    value.put("validity_ms",
              number_value(static_cast<std::uint64_t>(authorization.validity.milliseconds())));
    value.put("actor", text_value(authorization.actor.text()));
    value.put("source", text_value(authorization.source.text()));
    value.put("state", text_value(authorization_state_name(authorization.state)));
    value.put("digest", text_value(authorization.authorization_digest.to_hex()));
    authorization_list.add(std::move(value));
  }
  report.put("authorizations", std::move(authorization_list));
  report.put("fence_count", number_value(static_cast<std::uint64_t>(fences.value().size())));
  Value fence_list = list_value();
  for (const AuthorizationFence& fence : fences.value()) {
    Value value;
    value.put("attempt", text_value(fence.attempt.text()));
    value.put("reason", text_value(code_name(fence.reason)));
    value.put("explanation", text_value(fence.explanation));
    value.put("sequence", counter_value(fence.sequence));
    value.put("fenced_at", text_value(fence.fenced_at.to_text()));
    value.put("actor", text_value(fence.actor.text()));
    value.put("request", text_value(fence.request.to_hex()));
    fence_list.add(std::move(value));
  }
  report.put("fences", std::move(fence_list));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_provenance(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  RackId rack;
  while (!flags.at_end()) {
    if (flags.take("--rack")) {
      if (!flags.identity(rack)) {
        return usage_error(flags.error(), invocation.err);
      }
    } else {
      return unexpected(flags, invocation.err);
    }
  }
  if (rack.empty()) {
    return usage_error("missing required option --rack", invocation.err);
  }
  const Result<std::vector<ProvenanceRecord>> records = service.provenance(rack);
  if (!records.has_value()) {
    return report_error(records.error(), invocation.err);
  }
  Value report;
  report.put("rack", text_value(rack.text()));
  report.put("provenance_count",
             number_value(static_cast<std::uint64_t>(records.value().size())));
  Value list = list_value();
  for (const ProvenanceRecord& record : records.value()) {
    list.add(make_provenance_record(record));
  }
  report.put("provenance", std::move(list));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_rejections(const Invocation& invocation, TurnupService& service) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  const Result<std::vector<RejectionRecord>> records = service.rejections();
  if (!records.has_value()) {
    return report_error(records.error(), invocation.err);
  }
  Value report;
  report.put("rejection_count", number_value(static_cast<std::uint64_t>(records.value().size())));
  Value list = list_value();
  for (const RejectionRecord& record : records.value()) {
    list.add(make_rejection_record(record));
  }
  report.put("rejections", std::move(list));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_store_inspect(const Invocation& invocation) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  const Result<StoreInspection> inspection = DurableStore::inspect(invocation.options.store_path);
  if (!inspection.has_value()) {
    return report_error(inspection.error(), invocation.err);
  }
  const StoreInspection& answer = inspection.value();
  Value report;
  report.put("path", text_value(invocation.options.store_path));
  report.put("present", flag_value(answer.present));
  if (!answer.present) {
    return emit(invocation, report);
  }
  report.put("format_version", number_value(answer.format_version));
  report.put("encoding_model", number_value(answer.encoding_model));
  report.put("payload_bytes", number_value(answer.payload_bytes));
  report.put("file_bytes", number_value(answer.file_bytes));
  report.put("payload_digest", text_value(answer.payload_digest.to_hex()));
  report.put("state_digest", text_value(answer.state_digest.to_hex()));
  report.put("store_epoch", counter_value(answer.store_epoch));
  report.put("store_sequence", counter_value(answer.store_sequence));
  report.put("incarnation", counter_value(answer.incarnation));
  report.put("rack_count", number_value(static_cast<std::uint64_t>(answer.rack_count)));
  report.put("watermark_present", flag_value(answer.watermark_present));
  report.put("watermark_sequence", counter_value(answer.watermark_sequence));
  report.put("watermark_digest", text_value(answer.watermark_digest.to_hex()));
  return emit(invocation, report);
}

[[nodiscard]] int cmd_store_verify(const Invocation& invocation) {
  FlagReader flags(invocation.tokens);
  if (!flags.at_end()) {
    return unexpected(flags, invocation.err);
  }
  const Result<ServiceState> state = DurableStore::read_only_load(invocation.options.store_path);
  if (!state.has_value()) {
    return report_error(state.error(), invocation.err);
  }
  Value report;
  report.put("path", text_value(invocation.options.store_path));
  report.put("state", text_value("valid"));
  report.put("rack_count", number_value(static_cast<std::uint64_t>(state.value().racks.size())));
  report.put("store_epoch", counter_value(state.value().store_epoch));
  report.put("store_sequence", counter_value(state.value().store_sequence));
  report.put("incarnation", counter_value(state.value().incarnation));
  report.put("control_epoch", counter_value(state.value().control_epoch));
  report.put("observation_sequence", counter_value(state.value().observation_sequence));
  report.put("state_digest", text_value(compute_state_digest(state.value()).to_hex()));
  return emit(invocation, report);
}

// ---------------------------------------------------------------------------
// Usage and version
// ---------------------------------------------------------------------------

[[nodiscard]] int write_version(const Invocation& invocation) {
  Value report;
  report.put("version", text_value(version_string()));
  report.put("configuration", text_value(build_configuration()));
  report.put("compiler", text_value(build_compiler()));
  return emit(invocation, report);
}

void write_usage(std::ostream& out) {
  out << "usage: rack-turnup [global options] <command> [options]\n";
  out << "\n";
  out << "Global options:\n";
  out << "  --store <path>       durable state file (default rack_turnup.state)\n";
  out << "  --now <rfc3339>      authority time (default the current wall clock)\n";
  out << "  --actor <id>         actor recorded in provenance (default cli)\n";
  out << "  --source <ref>       source reference recorded in provenance\n";
  out << "  --note <text>        note recorded in provenance\n";
  out << "  --request-id <id>    idempotency identity of one mutation\n";
  out << "  --format text|json   output format (default text)\n";
  out << "  --read-only          open the store for reading only\n";
  out << "  -h, --help, help     print this text\n";
  out << "  --version, version   print version, build configuration and compiler\n";
  out << "\n";
  out << "Commands:\n";
  out << "  rack register --rack <rack:id> [--site <site:id>] [--label <text>]\n";
  out << "                --member <spec> [--member <spec> ...]\n";
  out << "  rack composition --rack <id> --generation <n> --member <spec>\n";
  out << "                   [--member <spec> ...] [--replace-commissioned]\n";
  out << "  rack list\n";
  out << "  plan create --rack <id> [--rack-generation <n>]\n";
  out << "              [--composition-generation <n>] [--topology-generation <n>]\n";
  out << "              [--dependency-generation <n>] [--power-generation <n>]\n";
  out << "              [--cooling-generation <n>] [--network-generation <n>]\n";
  out << "              [--inventory-generation <n>] [--health-generation <n>]\n";
  out << "              [--capacity-generation <n>] [--maintenance-generation <n>]\n";
  out << "              [--policy-generation <n>] [--firmware-generation <n>]\n";
  out << "              [--require-power-mw <n>] [--require-cooling-mw <n>]\n";
  out << "              [--require-network-ports <n>] [--require-redundant-feeds]\n";
  out << "              [--require-topology <ref>] [--no-applied-power-verification]\n";
  out << "              [--no-cooling-delivery-verification]\n";
  out << "              [--no-network-authority-verification]\n";
  out << "              [--health-not-required-for-every-member] [--allow-degraded]\n";
  out << "              [--authorization-validity <dur>]\n";
  out << "              [--default-evidence-validity <dur>] [--compat <spec>]\n";
  out << "              [--supersede] [--replace-commissioned]\n";
  out << "              [--cancel-authorization]\n";
  out << "  evidence power --rack <id> --domain <ref> --available-mw <n>\n";
  out << "                 --applied-mw <n> [--energized-circuits <n>] [--redundant]\n";
  out << "                 [--applied-verified] [--observed-at <rfc3339>]\n";
  out << "                 [--expected-revision <n>]\n";
  out << "  evidence cooling --rack <id> --domain <ref> --capacity-mw <n>\n";
  out << "                   --delivered-mw <n> [--inlet-mc <n>] [--max-inlet-mc <n>]\n";
  out << "                   [--delivery-verified] [--observed-at <rfc3339>]\n";
  out << "                   [--expected-revision <n>]\n";
  out << "  evidence network --rack <id> --domain <ref> [--topology <ref>]\n";
  out << "                   --attached-ports <n> [--reachable-ports <n>]\n";
  out << "                   [--authority-verified] [--observed-at <rfc3339>]\n";
  out << "                   [--expected-revision <n>]\n";
  out << "  evidence identity --rack <id> --registry-digest <hex>\n";
  out << "                    [--declared-members <n>] [--unreadable-members <n>]\n";
  out << "                    [--observed-at <rfc3339>] [--expected-revision <n>]\n";
  out << "  evidence inventory --rack <id> [--entry <spec> ...]\n";
  out << "                     [--unreadable-positions <n>] [--observed-at <rfc3339>]\n";
  out << "                     [--expected-revision <n>]\n";
  out << "  evidence health --rack <id> [--sample <dev:id>=<status> ...]\n";
  out << "                  [--unchecked <n>] [--observed-at <rfc3339>]\n";
  out << "                  [--expected-revision <n>]\n";
  out << "  evidence compatibility --rack <id> [--member-spec <spec> ...]\n";
  out << "                         [--observed-at <rfc3339>] [--expected-revision <n>]\n";
  out << "  evidence list --rack <id>\n";
  out << "  evaluate --rack <id> [--at <rfc3339>]\n";
  out << "  blockers --rack <id> [--at <rfc3339>]\n";
  out << "  authorize --rack <id> [--validity <dur>] [--expected-revision <n>]\n";
  out << "  activate --rack <id> --attempt <at:n> [--outcome active|not_active|unknown]\n";
  out << "           [--observed-at <rfc3339>] [--members <n>] [--validity <dur>]\n";
  out << "           [--observed-composition <hex>] [--expected-revision <n>]\n";
  out << "  commission --rack <id> --attempt <at:n> [--expected-revision <n>]\n";
  out << "  rollback --rack <id> [--reason <text>] [--acknowledge-active]\n";
  out << "           [--expected-revision <n>]\n";
  out << "  drain begin --rack <id> [--expected-revision <n>]\n";
  out << "  drain complete --rack <id> [--expected-revision <n>]\n";
  out << "  decommission --rack <id> [--expected-revision <n>]\n";
  out << "  take-control\n";
  out << "  recover\n";
  out << "  summary [--rack <id>]\n";
  out << "  authority list --rack <id>      authorizations, then fences\n";
  out << "  provenance --rack <id>\n";
  out << "  rejections\n";
  out << "  store inspect\n";
  out << "  store verify\n";
  out << "\n";
  out << "Specs (comma separated key=value, trait may repeat):\n";
  out << "  member      device=<dev:id>,unit=<1..512>,slot=<0..255>[,asset=<id>]\n";
  out << "              [,baseline=<fw:id>][,label=<text>][,trait=<trait>]...\n";
  out << "  entry       member fields plus [,readable=true|false]\n";
  out << "  compat      kind=<requirement kind>[,trait=<t>][,device=<d>][,baseline=<fw>]\n";
  out << "              [,mandatory=true|false][,note=<text>]\n";
  out << "  membership  device=<dev:id>[,baseline=<fw:id>][,trait=<trait>]...\n";
  out << "  sample      <dev:id>=passing|warning|failing|unknown\n";
  out << "  durations   <n>, <n>ms, <n>s, <n>m, <n>h or <n>d\n";
  out << "\n";
  out << "Exit codes: 0 success, 1 usage error, 2 rejected request, 3 store failure\n";
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Opens the durable store, runs one command against it, and closes it. The
// writer lock is held for the whole run, which is what makes reading the
// current plan revision before a mutation authoritative.
template <typename Handler>
[[nodiscard]] int run_service_command(const Invocation& invocation, Handler handler) {
  StoreOptions store;
  store.path = invocation.options.store_path;
  store.read_only = invocation.options.read_only;
  store.create_if_missing = !invocation.options.read_only;
  Result<TurnupService> service = TurnupService::open(store);
  if (!service.has_value()) {
    return report_error(service.error(), invocation.err);
  }
  return handler(invocation, service.value());
}

[[nodiscard]] bool is_command_family(std::string_view command) {
  return command == "rack" || command == "plan" || command == "evidence" ||
         command == "drain" || command == "authority" || command == "store";
}

[[nodiscard]] int dispatch(const std::vector<std::string>& arguments, std::ostream& out,
                           std::ostream& err) {
  Options options;
  options.store_path = std::string(kDefaultStorePath);
  // The default actor is a valid identity by construction; value_or keeps the
  // function total instead of asserting that.
  options.actor = ActorId::parse(kDefaultActor).value_or(ActorId{});
  GlobalRequest global;
  std::vector<std::string> rest;
  std::string error;
  if (!extract_globals(arguments, options, global, rest, error)) {
    return usage_error(error, err);
  }

  // The command is one or two leading words; everything after it belongs to the
  // command.
  std::vector<std::string> words;
  std::size_t consumed = 0;
  while (consumed < rest.size() && words.size() < 2 &&
         (rest[consumed].empty() || rest[consumed].front() != '-')) {
    words.push_back(rest[consumed]);
    ++consumed;
  }
  const std::vector<std::string> tokens(rest.begin() + static_cast<std::ptrdiff_t>(consumed),
                                        rest.end());
  const Invocation invocation{options, tokens, out, err};

  if (global.help) {
    write_usage(out);
    return kExitSuccess;
  }
  if (global.version) {
    return write_version(invocation);
  }
  if (words.empty()) {
    return usage_error("a command is required", err);
  }
  const std::string& command = words.front();
  if (command == "help") {
    write_usage(out);
    return kExitSuccess;
  }
  if (command == "version") {
    return write_version(invocation);
  }
  const std::string subcommand = (words.size() > 1) ? words[1] : std::string();
  const std::string spelled = join(words, ' ');

  if (words.size() == 1) {
    if (command == "evaluate") {
      return run_service_command(invocation, cmd_evaluate);
    }
    if (command == "blockers") {
      return run_service_command(invocation, cmd_blockers);
    }
    if (command == "authorize") {
      return run_service_command(invocation, cmd_authorize);
    }
    if (command == "activate") {
      return run_service_command(invocation, cmd_activate);
    }
    if (command == "commission") {
      return run_service_command(invocation, cmd_commission);
    }
    if (command == "rollback") {
      return run_service_command(invocation, cmd_rollback);
    }
    if (command == "decommission") {
      return run_service_command(invocation, cmd_decommission);
    }
    if (command == "take-control") {
      return run_service_command(invocation, cmd_take_control);
    }
    if (command == "recover") {
      return run_service_command(invocation, cmd_recover);
    }
    if (command == "summary") {
      return run_service_command(invocation, cmd_summary);
    }
    if (command == "provenance") {
      return run_service_command(invocation, cmd_provenance);
    }
    if (command == "rejections") {
      return run_service_command(invocation, cmd_rejections);
    }
  } else if (command == "rack") {
    if (subcommand == "register") {
      return run_service_command(invocation, cmd_rack_register);
    }
    if (subcommand == "composition") {
      return run_service_command(invocation, cmd_rack_composition);
    }
    if (subcommand == "list") {
      return run_service_command(invocation, cmd_rack_list);
    }
  } else if (command == "plan") {
    if (subcommand == "create") {
      return run_service_command(invocation, cmd_plan_create);
    }
  } else if (command == "evidence") {
    if (subcommand == "power") {
      return run_service_command(invocation, cmd_evidence_power);
    }
    if (subcommand == "cooling") {
      return run_service_command(invocation, cmd_evidence_cooling);
    }
    if (subcommand == "network") {
      return run_service_command(invocation, cmd_evidence_network);
    }
    if (subcommand == "identity") {
      return run_service_command(invocation, cmd_evidence_identity);
    }
    if (subcommand == "inventory") {
      return run_service_command(invocation, cmd_evidence_inventory);
    }
    if (subcommand == "health") {
      return run_service_command(invocation, cmd_evidence_health);
    }
    if (subcommand == "compatibility") {
      return run_service_command(invocation, cmd_evidence_compatibility);
    }
    if (subcommand == "list") {
      return run_service_command(invocation, cmd_evidence_list);
    }
  } else if (command == "drain") {
    if (subcommand == "begin") {
      return run_service_command(invocation, cmd_drain_begin);
    }
    if (subcommand == "complete") {
      return run_service_command(invocation, cmd_drain_complete);
    }
  } else if (command == "authority") {
    if (subcommand == "list") {
      return run_service_command(invocation, cmd_authority_list);
    }
  } else if (command == "store") {
    if (subcommand == "inspect") {
      return cmd_store_inspect(invocation);
    }
    if (subcommand == "verify") {
      return cmd_store_verify(invocation);
    }
  }

  if (is_command_family(command) && subcommand.empty()) {
    return usage_error("the '" + command + "' command requires a subcommand", err);
  }
  return usage_error("unknown command '" + spelled + "'", err);
}

}  // namespace

int run_cli(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
  try {
    return dispatch(arguments, out, err);
  } catch (const std::exception& failure) {
    return report_internal(failure.what(), err);
  } catch (...) {
    return report_internal("the command failed before it produced a result", err);
  }
}

}  // namespace rackturnup
