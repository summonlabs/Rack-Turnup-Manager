// Rack Turnup Manager - identity factories for the empty cases.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/ids.hpp"

namespace rackturnup {

Result<DisplayLabel> parse_label_or_none(std::string_view text) {
  if (text.empty()) {
    return DisplayLabel{};
  }
  return DisplayLabel::parse(text);
}

Result<Note> parse_note_or_none(std::string_view text) {
  if (text.empty()) {
    return Note{};
  }
  return Note::parse(text);
}

}  // namespace rackturnup
