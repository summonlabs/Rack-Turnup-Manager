// Rack Turnup Manager - command line entry point.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "rack_turnup/cli.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
  }
  return rackturnup::run_cli(arguments, std::cout, std::cerr);
}
