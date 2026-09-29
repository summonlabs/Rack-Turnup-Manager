// Rack Turnup Manager - child process used to prove persistence and lock claims.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program exists so that crash consistency and cross-process exclusion are
// observed on real processes instead of simulated inside one. Every command
// either commits durable generations or holds the writer lock, and the
// termination commands die without running any destructor, flush or cleanup.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rack_turnup/store.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/time.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace rtm = rackturnup;

// Leaves the process immediately: no stack unwinding, no destructors, no
// buffered output is flushed. This is what makes a crash test a crash test.
[[noreturn]] void terminate_now(int code) {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
  std::_Exit(code);
}

void abort_hook(rtm::StoreAbortPoint point) {
  std::cerr << "abort at " << rtm::store_abort_point_name(point) << std::endl;
  terminate_now(90);
}

[[nodiscard]] bool write_marker(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream << text;
  stream.flush();
  return stream.good();
}

[[nodiscard]] bool marker_exists(const std::string& path) {
  std::ifstream stream(path);
  return stream.is_open();
}

[[nodiscard]] int commit_generations(rtm::DurableStore& store, rtm::ServiceState& state, int count) {
  for (int index = 0; index < count; ++index) {
    state.store_sequence = store.sequence().next();
    state.store_epoch = store.epoch();
    state.updated_at = rtm::WallClock::now();
    const rtm::Status committed = store.commit(state);
    if (!committed.has_value()) {
      std::cerr << "commit failed: " << rtm::describe(committed.error()) << std::endl;
      return 2;
    }
  }
  return 0;
}

[[nodiscard]] int parse_count(const std::string& text, int& out) {
  std::uint64_t value = 0;
  if (!rtm::parse_u64(text, value) || value > 1000000u) {
    return 1;
  }
  out = static_cast<int>(value);
  return 0;
}

[[nodiscard]] std::uint64_t parse_pid(const std::string& text) {
  std::uint64_t value = 0;
  static_cast<void>(rtm::parse_u64(text, value));
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (arguments.size() < 3) {
    std::cerr << "usage: rtm_crash_child <command> <state-path> ..." << std::endl;
    return 1;
  }
  const std::string command = arguments[0];
  const std::string state_path = arguments[1];

  if (command == "hold-lock") {
    if (arguments.size() < 5) {
      return 1;
    }
    const std::string marker = arguments[2];
    const std::string release = arguments[3];
    const std::uint64_t parent = parse_pid(arguments[4]);
    rtm::StoreOptions options;
    options.path = state_path;
    const rtm::Result<rtm::DurableStore> store = rtm::DurableStore::open(options);
    if (!store.has_value()) {
      std::cerr << "child could not take the lock: " << rtm::describe(store.error()) << std::endl;
      return 3;
    }
    if (!write_marker(marker, "held\n")) {
      return 4;
    }
    // The child holds the lock until the parent releases it or dies. Neither
    // wait is a timeout: the release is an explicit file, and the fallback is
    // the parent process actually ending.
    while (true) {
      if (marker_exists(release)) {
        break;
      }
#if defined(_WIN32)
      HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent));
      if (handle == nullptr) {
        break;
      }
      const DWORD waited = WaitForSingleObject(handle, 0);
      CloseHandle(handle);
      if (waited != WAIT_TIMEOUT) {
        break;
      }
#else
      if (::kill(static_cast<pid_t>(parent), 0) != 0) {
        break;
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
  }

  rtm::StoreOptions options;
  options.path = state_path;
  if (command == "commit-abort") {
    if (arguments.size() < 4) {
      return 1;
    }
    const rtm::Result<rtm::StoreAbortPoint> point = rtm::store_abort_point_from_name(arguments[2]);
    if (!point.has_value()) {
      std::cerr << "unknown abort point" << std::endl;
      return 1;
    }
    options.abort_point = point.value();
    options.abort_hook = &abort_hook;
  }
  rtm::Result<rtm::DurableStore> store = rtm::DurableStore::open(options);
  if (!store.has_value()) {
    std::cerr << "open failed: " << rtm::describe(store.error()) << std::endl;
    return 3;
  }
  const rtm::Result<rtm::ServiceState> loaded = store.value().load();
  if (!loaded.has_value()) {
    std::cerr << "load failed: " << rtm::describe(loaded.error()) << std::endl;
    return 3;
  }
  rtm::ServiceState state = loaded.value();

  int generations = 1;
  if (command == "commit-abort") {
    if (arguments.size() >= 5 && parse_count(arguments[3], generations) != 0) {
      return 1;
    }
  } else if (arguments.size() >= 3 && parse_count(arguments[2], generations) != 0) {
    return 1;
  }

  const int committed = commit_generations(store.value(), state, generations);
  if (committed != 0) {
    return committed;
  }
  std::cout << "committed=" << generations << " sequence=" << store.value().sequence().value()
            << " epoch=" << store.value().epoch().value() << std::endl;

  if (command == "commit-then-die") {
    terminate_now(91);
  }
  return 0;
}
