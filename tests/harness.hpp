// Rack Turnup Manager - self-contained test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/errors.hpp"
#include "rack_turnup/result.hpp"

namespace rtmtest {

// A failed check aborts only the test that failed: the runner catches this.
class CheckFailure final {
 public:
  explicit CheckFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

 private:
  std::string message_;
};

struct TestCase {
  const char* suite;
  const char* name;
  void (*function)();
};

[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)());
};

void report_failure(const char* file, int line, const std::string& message);

// Unwraps a successful Result, reporting the error as a failed check otherwise.
// It is deliberately not [[nodiscard]]: a test often calls it only for the
// assertion that the operation succeeded.
template <typename T>
T must(rackturnup::Result<T> result) {
  if (!result.has_value()) {
    report_failure(__FILE__, __LINE__,
                   std::string("unexpected failure: ") + rackturnup::describe(result.error()));
  }
  return std::move(result).value();
}

// Deterministic pseudo random generator (splitmix64). Every property test seeds
// it from the command line, and the seed is printed so a failure reproduces.
class Rng final {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept;
  // Uniform value in [0, bound); bound must be greater than zero.
  [[nodiscard]] std::uint64_t uniform(std::uint64_t bound) noexcept;
  [[nodiscard]] bool chance(unsigned percent) noexcept;
  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

// A unique temporary directory removed when the object is destroyed.
class TempDir final {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;

 private:
  std::string path_;
};

struct ProcessResult {
  bool started = false;
  int exit_code = 0;
  std::string standard_output;
  std::string standard_error;
};

// Runs a child process with its output redirected to files, then reads them.
[[nodiscard]] ProcessResult run_child(const std::string& executable,
                                      const std::vector<std::string>& arguments);
[[nodiscard]] bool write_text_file(const std::string& path, const std::string& text);
[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] bool remove_file(const std::string& path);
[[nodiscard]] std::string read_text_file(const std::string& path);
[[nodiscard]] std::uint64_t current_process_id() noexcept;
[[nodiscard]] bool process_alive(std::uint64_t process_id) noexcept;

// Paths of the sibling executables, injected by the build.
[[nodiscard]] std::string crash_child_executable();
[[nodiscard]] std::string cli_executable();

[[nodiscard]] int run_all(const std::vector<std::string>& arguments);

[[nodiscard]] std::uint64_t global_seed() noexcept;
[[nodiscard]] bool verbose() noexcept;

}  // namespace rtmtest

#define RTM_TEST(suite, name)                                                          \
  static void suite##_##name##_body();                                                 \
  static const ::rtmtest::Registrar suite##_##name##_registrar(#suite, #name,          \
                                                              &suite##_##name##_body); \
  static void suite##_##name##_body()

#define RTM_CHECK(condition)                                                       \
  do {                                                                             \
    if (!(condition)) {                                                            \
      ::rtmtest::report_failure(__FILE__, __LINE__, "check failed: " #condition); \
    }                                                                              \
  } while (false)

#define RTM_CHECK_EQ(left, right)                                                       \
  do {                                                                                  \
    const auto rtm_left = (left);                                                       \
    const auto rtm_right = (right);                                                     \
    if (!(rtm_left == rtm_right)) {                                                     \
      ::rtmtest::report_failure(__FILE__, __LINE__,                                     \
                                std::string("check failed: " #left " == " #right)); \
    }                                                                                   \
  } while (false)

#define RTM_CHECK_TEXT(left, right)                                                        \
  do {                                                                                     \
    const std::string rtm_left = std::string(left);                                        \
    const std::string rtm_right = std::string(right);                                      \
    if (rtm_left != rtm_right) {                                                           \
      ::rtmtest::report_failure(__FILE__, __LINE__,                                        \
                                std::string("text mismatch: " #left " == " #right     \
                                            " (left) ") + rtm_left + " (right) " +    \
                                    rtm_right);                                           \
    }                                                                                      \
  } while (false)

// Requires a failed Result carrying exactly the expected code.
#define RTM_CHECK_CODE(expression, expected_code)                                          \
  do {                                                                                     \
    const auto& rtm_result = (expression);                                                 \
    if (rtm_result.has_value()) {                                                          \
      ::rtmtest::report_failure(__FILE__, __LINE__,                                        \
                                std::string("expected failure " #expected_code " from " \
                                            #expression " but it succeeded"));         \
    } else if (rtm_result.error().code != (expected_code)) {                               \
      ::rtmtest::report_failure(__FILE__, __LINE__,                                        \
                                std::string("expected " #expected_code " but got ") +   \
                                    std::string(::rackturnup::code_name(               \
                                        rtm_result.error().code)) +                    \
                                    " (" + ::rackturnup::describe(rtm_result.error()) + \
                                    ")");                                              \
    }                                                                                      \
  } while (false)
