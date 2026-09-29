// Rack Turnup Manager - test harness, temporary directories and child processes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include "rack_turnup/errors.hpp"
#include "rack_turnup/text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rtmtest {
namespace {

std::uint64_t g_seed = 20260211u;
bool g_verbose = false;
std::uint64_t g_unique = 0;

[[nodiscard]] std::string unique_suffix() {
  ++g_unique;
  return std::to_string(current_process_id()) + "-" + std::to_string(g_unique);
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
}

void report_failure(const char* file, int line, const std::string& message) {
  throw CheckFailure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

std::uint64_t Rng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint32_t Rng::next_u32() noexcept { return static_cast<std::uint32_t>(next_u64() >> 32); }

std::uint64_t Rng::uniform(std::uint64_t bound) noexcept {
  return (bound == 0) ? 0 : (next_u64() % bound);
}

bool Rng::chance(unsigned percent) noexcept { return uniform(100) < percent; }

std::uint64_t global_seed() noexcept { return g_seed; }

bool verbose() noexcept { return g_verbose; }

TempDir::TempDir(const std::string& label) {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  path_ = (base / ("rack-turnup-" + label + "-" + unique_suffix())).string();
  std::error_code error;
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

bool write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  return stream.good();
}

bool file_exists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

bool remove_file(const std::string& path) {
  std::error_code error;
  return std::filesystem::remove(path, error);
}

std::string read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

bool process_alive(std::uint64_t process_id) noexcept {
#if defined(_WIN32)
  HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
  if (handle == nullptr) {
    return false;
  }
  const DWORD waited = WaitForSingleObject(handle, 0);
  CloseHandle(handle);
  return waited == WAIT_TIMEOUT;
#else
  return ::kill(static_cast<pid_t>(process_id), 0) == 0;
#endif
}

std::string crash_child_executable() {
#ifdef RTM_CRASH_CHILD_PATH
  return std::string(RTM_CRASH_CHILD_PATH);
#else
  return std::string();
#endif
}

std::string cli_executable() {
#ifdef RTM_CLI_PATH
  return std::string(RTM_CLI_PATH);
#else
  return std::string();
#endif
}

namespace {

[[nodiscard]] std::string quote_argument(const std::string& argument) {
  // Command line quoting shared by every platform: wrap in quotes and escape an
  // embedded quote with a backslash.
  std::string quoted = "\"";
  for (const char character : argument) {
    if (character == '"') {
      quoted.append("\\\"");
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
  return quoted;
}

#if defined(_WIN32)

[[nodiscard]] std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(required), L' ');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                      required);
  return wide;
}

#endif

}  // namespace

ProcessResult run_child(const std::string& executable, const std::vector<std::string>& arguments) {
  ProcessResult result;
  TempDir output("child");
  const std::string out_path = output.file("stdout.txt");
  const std::string err_path = output.file("stderr.txt");
#if defined(_WIN32)
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE out_handle = CreateFileW(to_wide(out_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_handle = CreateFileW(to_wide(err_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE) {
    return result;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION information{};
  std::wstring mutable_command = to_wide(command);
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &information);
  if (created == 0) {
    CloseHandle(out_handle);
    CloseHandle(err_handle);
    return result;
  }
  WaitForSingleObject(information.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(information.hProcess, &exit_code);
  CloseHandle(information.hProcess);
  CloseHandle(information.hThread);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  result.started = true;
  result.exit_code = static_cast<int>(exit_code);
#else
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  command.append(" >");
  command.append(quote_argument(out_path));
  command.append(" 2>");
  command.append(quote_argument(err_path));
  const int status = std::system(command.c_str());
  result.started = true;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  result.standard_output = read_text_file(out_path);
  result.standard_error = read_text_file(err_path);
  return result;
}

int run_all(const std::vector<std::string>& arguments) {
  std::string suite_filter;
  std::string test_filter;
  bool list_only = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "--suite" && i + 1 < arguments.size()) {
      suite_filter = arguments[++i];
    } else if (argument == "--test" && i + 1 < arguments.size()) {
      test_filter = arguments[++i];
    } else if (argument == "--seed" && i + 1 < arguments.size()) {
      std::uint64_t seed = 0;
      if (rackturnup::parse_u64(arguments[++i], seed)) {
        g_seed = seed;
      }
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--verbose") {
      g_verbose = true;
    }
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const TestCase& test : registry()) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (list_only) {
      std::cout << full << "\n";
      continue;
    }
    if (!suite_filter.empty() && suite_filter != test.suite) {
      ++skipped;
      continue;
    }
    if (!test_filter.empty() && test_filter != test.name) {
      ++skipped;
      continue;
    }
    try {
      test.function();
      ++passed;
      std::cout << "ok " << full << "\n";
    } catch (const CheckFailure& failure) {
      ++failed;
      std::cout << "not ok " << full << "\n  " << failure.message() << "\n";
    } catch (const std::exception& error) {
      ++failed;
      std::cout << "not ok " << full << "\n  unexpected exception: " << error.what() << "\n";
    } catch (...) {
      ++failed;
      std::cout << "not ok " << full << "\n  unexpected non standard exception\n";
    }
    std::cout.flush();
  }
  if (list_only) {
    return 0;
  }
  std::cout << "# seed=" << g_seed << " passed=" << passed << " failed=" << failed
            << " skipped=" << skipped << "\n";
  return (failed == 0) ? 0 : 1;
}

}  // namespace rtmtest

#ifndef RTM_TEST_MAIN_DEFINED
#define RTM_TEST_MAIN_DEFINED
int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.push_back(argv[i]);
  }
  return rtmtest::run_all(arguments);
}
#endif
