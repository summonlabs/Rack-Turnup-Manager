// Rack Turnup Manager - command line interface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "rack_turnup/export.hpp"

namespace rackturnup {

// Runs the command line interface. `arguments` excludes the program name.
// Returns the process exit code.
[[nodiscard]] RACK_TURNUP_API int run_cli(const std::vector<std::string>& arguments,
                                          std::ostream& out, std::ostream& err);

}  // namespace rackturnup
