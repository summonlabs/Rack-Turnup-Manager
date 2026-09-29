// Rack Turnup Manager - durable store with an explicit commit point.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rack_turnup/digest.hpp"
#include "rack_turnup/export.hpp"
#include "rack_turnup/result.hpp"
#include "rack_turnup/state.hpp"

namespace rackturnup {

// The durable file is a versioned, bounded, integrity-checked record:
//
//   header    magic "RTMSTAT1", format version, reserved flags, payload length,
//             payload SHA-256, header CRC-32, encoding model
//   payload   the canonical service state
//   trailer   payload CRC-32 and its own CRC-32
//
// Bytes after the trailer, a length that disagrees with the file size, a
// non-zero reserved field, a digest or checksum mismatch and an unsupported
// format version are all rejected. The store never repairs what it cannot
// verify.
inline constexpr char kStateFileMagic[8] = {'R', 'T', 'M', 'S', 'T', 'A', 'T', '1'};
inline constexpr std::uint32_t kStateHeaderBytes = 64;
inline constexpr std::uint32_t kStateTrailerBytes = 8;

// Points in the commit sequence at which a test can terminate the process. The
// names are stable because they appear in test output and in the crash child
// command line.
enum class StoreAbortPoint : std::uint8_t {
  None = 0,
  BeforeWrite = 1,
  AfterWrite = 2,
  AfterFlush = 3,
  AfterReadBack = 4,
  BeforePublish = 5,
  AfterPublish = 6,
  AfterWatermark = 7,
};

[[nodiscard]] RACK_TURNUP_API std::string_view store_abort_point_name(
    StoreAbortPoint point) noexcept;
[[nodiscard]] RACK_TURNUP_API Result<StoreAbortPoint> store_abort_point_from_name(
    std::string_view name);

struct StoreOptions {
  // Path of the state file. The lock and watermark files are derived from it by
  // appending ".lock" and ".watermark".
  std::string path;
  bool read_only = false;
  bool create_if_missing = true;
  StoreAbortPoint abort_point = StoreAbortPoint::None;
  // Invoked when the commit sequence reaches abort_point. Returning from the
  // hook continues the commit; a hook that terminates the process is what makes
  // crash consistency a real observation rather than a simulation.
  void (*abort_hook)(StoreAbortPoint) = nullptr;
};

// What a read-only inspection of a state file found.
struct StoreInspection {
  bool present = false;
  std::uint32_t format_version = 0;
  std::uint32_t encoding_model = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t file_bytes = 0;
  Digest payload_digest;
  Digest state_digest;
  StoreEpoch store_epoch;
  StoreSequence store_sequence;
  std::size_t rack_count = 0;
  bool watermark_present = false;
  StoreSequence watermark_sequence;
  Digest watermark_digest;
  IncarnationId incarnation;
};

// A single-writer durable store.
//
// Cross-process exclusion is a real operating-system lock on a separate lock
// file, taken in fail-fast mode: a second writer is refused with
// WriterLockHeld instead of waiting. Readers do not take the lock and never see
// a partially published generation, because publication is a single atomic
// replacement of the state file.
class DurableStore final {
 public:
  DurableStore() = default;
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&& other) noexcept;
  DurableStore& operator=(DurableStore&& other) noexcept;

  [[nodiscard]] static Result<DurableStore> open(const StoreOptions& options);

  // Reads the authoritative generation. A missing state file either yields an
  // empty state (when the store may create one) or NoAuthoritativeState.
  [[nodiscard]] Result<ServiceState> load();

  // Publishes one new generation: stage, flush, read back and verify, atomically
  // replace, update the watermark, and only then advance the in-memory fencing.
  [[nodiscard]] Status commit(const ServiceState& state);

  // Reads and verifies a state file without taking writer authority.
  [[nodiscard]] static Result<StoreInspection> inspect(const std::string& path);
  [[nodiscard]] static Result<ServiceState> read_only_load(const std::string& path);

  void close() noexcept;

  [[nodiscard]] bool is_writable() const noexcept { return writable_; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] StoreSequence sequence() const noexcept { return sequence_; }
  [[nodiscard]] StoreEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] const std::string& path() const noexcept { return options_.path; }
  [[nodiscard]] const std::string& lock_path() const noexcept { return lock_path_; }
  [[nodiscard]] const std::string& watermark_path() const noexcept { return watermark_path_; }

 private:
  void release_lock() noexcept;
  void reach_abort_point(StoreAbortPoint point);

  StoreOptions options_;
  std::string lock_path_;
  std::string watermark_path_;
  void* lock_handle_ = nullptr;
  bool open_ = false;
  bool writable_ = false;
  StoreSequence sequence_;
  StoreEpoch epoch_;
  std::uint64_t temp_counter_ = 0;
};

}  // namespace rackturnup
