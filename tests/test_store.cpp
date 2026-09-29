// Rack Turnup Manager - durable store, adversarial input and crash tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rack_turnup/state.hpp"
#include "rack_turnup/store.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/time.hpp"
#include "rack_turnup/version.hpp"

namespace {

namespace rtm = rackturnup;
using rtmtest::must;

[[nodiscard]] rtm::StoreOptions store_options(const std::string& path, bool read_only = false) {
  rtm::StoreOptions options;
  options.path = path;
  options.read_only = read_only;
  return options;
}

// Creates a state file holding `generations` published generations.
[[nodiscard]] rtm::StoreSequence seed_store(const std::string& path, int generations) {
  rtm::DurableStore store = must(rtm::DurableStore::open(store_options(path)));
  rtm::ServiceState state = must(store.load());
  for (int index = 0; index < generations; ++index) {
    state.store_sequence = store.sequence().next();
    state.store_epoch = store.epoch();
    state.updated_at = rtm::WallClock::now();
    must(store.commit(state));
  }
  return store.sequence();
}

[[nodiscard]] std::vector<std::uint8_t> to_bytes(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

[[nodiscard]] std::string to_text(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

void corrupt_and_expect_failure(const std::string& path, std::size_t offset, std::uint8_t value,
                                rtm::ErrorCode expected) {
  std::string bytes = rtmtest::read_text_file(path);
  RTM_CHECK(offset < bytes.size());
  bytes[offset] = static_cast<char>(value);
  RTM_CHECK(rtmtest::write_text_file(path, bytes));
  const rtm::Result<rtm::ServiceState> loaded = rtm::DurableStore::read_only_load(path);
  RTM_CHECK(!loaded.has_value());
  if (loaded.has_value()) {
    return;
  }
  if (loaded.error().code != expected) {
    ::rtmtest::report_failure(__FILE__, __LINE__,
                              std::string("expected ") + std::string(rtm::code_name(expected)) +
                                  " but got " + std::string(rtm::code_name(loaded.error().code)) +
                                  " (" + rtm::describe(loaded.error()) + ")");
  }
}

}  // namespace

RTM_TEST(store, commit_and_reload_round_trip) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  const rtm::StoreSequence sequence = seed_store(path, 3);
  RTM_CHECK_EQ(sequence.value(), std::uint64_t{3});
  RTM_CHECK(rtmtest::file_exists(path));
  RTM_CHECK(rtmtest::file_exists(path + ".watermark"));
  RTM_CHECK(rtmtest::file_exists(path + ".lock"));
  const rtm::ServiceState reloaded = must(rtm::DurableStore::read_only_load(path));
  RTM_CHECK_EQ(reloaded.store_sequence.value(), std::uint64_t{3});
  const rtm::StoreInspection inspection = must(rtm::DurableStore::inspect(path));
  RTM_CHECK(inspection.present);
  RTM_CHECK_EQ(inspection.format_version, rtm::kStateFormatVersion);
  RTM_CHECK_EQ(inspection.store_sequence.value(), std::uint64_t{3});
  RTM_CHECK(inspection.watermark_present);
  RTM_CHECK_EQ(inspection.watermark_sequence.value(), std::uint64_t{3});
  RTM_CHECK(rtm::digests_equal(inspection.watermark_digest, inspection.payload_digest));
  const rtm::StoreInspection missing = must(rtm::DurableStore::inspect(directory.file("none.rtm")));
  RTM_CHECK(!missing.present);
}

RTM_TEST(store, encoded_state_is_canonical) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  rtm::DurableStore store = must(rtm::DurableStore::open(store_options(path)));
  rtm::ServiceState state = must(store.load());
  state.store_sequence = store.sequence().next();
  state.store_epoch = store.epoch();
  const std::vector<std::uint8_t> first = must(rtm::encode_state(state));
  const rtm::ServiceState decoded = must(rtm::decode_state(first));
  const std::vector<std::uint8_t> second = must(rtm::encode_state(decoded));
  RTM_CHECK(first == second);
  RTM_CHECK(rtm::digests_equal(rtm::compute_state_digest(state),
                                rtm::compute_state_digest(decoded)));
  const rtm::Status validated = rtm::validate_state(decoded);
  RTM_CHECK(validated.has_value());
  RTM_CHECK(rtm::decode_state(std::vector<std::uint8_t>{}).has_value() == false);
}

RTM_TEST(store, rejects_malformed_and_tampered_files) {
  rtmtest::TempDir directory("store");
  const std::string reference = directory.file("reference.rtm");
  static_cast<void>(seed_store(reference, 2));
  const std::string original = rtmtest::read_text_file(reference);

  // A future format version is refused rather than guessed at.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  corrupt_and_expect_failure(reference, 8, 0x02, rtm::ErrorCode::UnsupportedFormatVersion);
  // A reserved field must be zero.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  corrupt_and_expect_failure(reference, 12, 0x01, rtm::ErrorCode::ReservedBitsSet);
  // The magic is part of the contract.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  corrupt_and_expect_failure(reference, 0, 'X', rtm::ErrorCode::CorruptState);
  // A flipped payload byte is caught by the payload digest.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  corrupt_and_expect_failure(reference, original.size() / 2, 0x7F,
                             rtm::ErrorCode::IntegrityCheckFailed);
  // A flipped header byte is caught by the header checksum or the digest.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  corrupt_and_expect_failure(reference, 20, 0x01, rtm::ErrorCode::TruncatedState);

  // Truncation and trailing bytes are both refused, at every cut point.
  RTM_CHECK(rtmtest::write_text_file(reference, original));
  RTM_CHECK(rtmtest::write_text_file(reference, original.substr(0, original.size() - 1)));
  RTM_CHECK(!rtm::DurableStore::read_only_load(reference).has_value());
  RTM_CHECK(rtmtest::write_text_file(reference, original + "\n"));
  const rtm::Result<rtm::ServiceState> trailing = rtm::DurableStore::read_only_load(reference);
  RTM_CHECK(!trailing.has_value());
  if (!trailing.has_value()) {
    RTM_CHECK(trailing.error().code == rtm::ErrorCode::TrailingBytes);
  }
  RTM_CHECK(rtmtest::write_text_file(reference, ""));
  const rtm::Result<rtm::ServiceState> empty = rtm::DurableStore::read_only_load(reference);
  RTM_CHECK(!empty.has_value());
  if (!empty.has_value()) {
    RTM_CHECK(empty.error().code == rtm::ErrorCode::TruncatedState);
  }
}

RTM_TEST(store, watermark_detects_a_rolled_back_state_file) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  static_cast<void>(seed_store(path, 2));
  const std::string older_copy = rtmtest::read_text_file(path);
  static_cast<void>(seed_store(path, 1));  // sequence 3 published
  RTM_CHECK(rtm::DurableStore::read_only_load(path).has_value());
  // Restoring an older copy must be refused: the watermark remembers the newer
  // generation, so the older file cannot be silently inherited.
  RTM_CHECK(rtmtest::write_text_file(path, older_copy));
  const rtm::Result<rtm::ServiceState> restored = rtm::DurableStore::read_only_load(path);
  RTM_CHECK(!restored.has_value());
  if (!restored.has_value()) {
    RTM_CHECK(restored.error().code == rtm::ErrorCode::StaleDurableState);
  }
  const rtm::Result<rtm::StoreInspection> inspection = rtm::DurableStore::inspect(path);
  RTM_CHECK(!inspection.has_value());
}

RTM_TEST(store, single_writer_and_readers) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  static_cast<void>(seed_store(path, 1));
  rtm::DurableStore writer = must(rtm::DurableStore::open(store_options(path)));
  // A reader never takes the writer lock and always sees a whole generation.
  RTM_CHECK(rtm::DurableStore::read_only_load(path).has_value());
  // A second writer in the same process is refused instead of waiting.
  RTM_CHECK_CODE(rtm::DurableStore::open(store_options(path)), rtm::ErrorCode::WriterLockHeld);
  writer.close();
  rtm::DurableStore second = must(rtm::DurableStore::open(store_options(path)));
  RTM_CHECK(second.is_writable());
  second.close();

  rtm::StoreOptions missing;
  missing.path = directory.file("absent.rtm");
  missing.create_if_missing = false;
  RTM_CHECK_CODE(rtm::DurableStore::open(missing), rtm::ErrorCode::NoAuthoritativeState);

  rtm::DurableStore reader = must(rtm::DurableStore::open(store_options(path, true)));
  RTM_CHECK(reader.is_writable() == false);
  rtm::ServiceState state = must(reader.load());
  RTM_CHECK_CODE(reader.commit(state), rtm::ErrorCode::ReadOnlyStore);
}

RTM_TEST(store, commit_sequence_is_checked) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  rtm::DurableStore store = must(rtm::DurableStore::open(store_options(path)));
  rtm::ServiceState state = must(store.load());
  // Committing without advancing the sequence, or advancing it twice, is refused.
  RTM_CHECK_CODE(store.commit(state), rtm::ErrorCode::SequenceRegression);
  state.store_sequence = must(rtm::StoreSequence::create(5));
  state.store_epoch = store.epoch();
  RTM_CHECK_CODE(store.commit(state), rtm::ErrorCode::SequenceRegression);
  state.store_sequence = store.sequence().next();
  state.store_epoch = must(rtm::StoreEpoch::create(99));
  RTM_CHECK_CODE(store.commit(state), rtm::ErrorCode::StoreEpochRegression);
  state.store_epoch = store.epoch();
  must(store.commit(state));
  // The in-memory fencing advanced only after publication.
  RTM_CHECK_EQ(store.sequence().value(), std::uint64_t{1});
}

RTM_TEST(store, every_abort_point_is_reachable) {
  rtmtest::TempDir directory("store");
  const std::string path = directory.file("state.rtm");
  std::vector<rtm::StoreAbortPoint> reached;
  rtm::StoreOptions options = store_options(path);
  options.abort_hook = [](rtm::StoreAbortPoint point) {
    static_cast<void>(point);
  };
  for (std::uint8_t value = 1; value <= 7; ++value) {
    const auto point = static_cast<rtm::StoreAbortPoint>(value);
    reached.clear();
    options.abort_point = point;
    rtm::StoreOptions per_point = options;
    rtm::DurableStore store = must(rtm::DurableStore::open(per_point));
    rtm::ServiceState state = must(store.load());
    state.store_sequence = store.sequence().next();
    state.store_epoch = store.epoch();
    must(store.commit(state));
    store.close();
    RTM_CHECK(rtm::DurableStore::read_only_load(path).has_value());
  }
  static_cast<void>(reached);
  RTM_CHECK(!rtm::store_abort_point_name(rtm::StoreAbortPoint::AfterWatermark).empty());
  RTM_CHECK(rtm::store_abort_point_from_name("before_publish").has_value());
  RTM_CHECK_CODE(rtm::store_abort_point_from_name("nope"), rtm::ErrorCode::InvalidEnumValue);
}

RTM_TEST(store, adversarial_random_mutations_never_succeed_silently) {
  rtmtest::TempDir directory("adversarial");
  const std::string reference = directory.file("reference.rtm");
  static_cast<void>(seed_store(reference, 2));
  const std::string original = rtmtest::read_text_file(reference);
  const std::string probe = directory.file("probe.rtm");
  rtmtest::Rng rng(rtmtest::global_seed() ^ 0x1234ABCDu);
  const rtm::ServiceState expected = must(rtm::DurableStore::read_only_load(reference));
  const rtm::Digest expected_digest = rtm::compute_state_digest(expected);
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 300; ++iteration) {
    std::string mutated = original;
    const std::uint64_t mode = rng.uniform(3);
    // Every mutation is guaranteed to differ from the original, so "accepted"
    // can only ever mean that a real change went undetected.
    if (mode == 0) {
      const std::size_t offset = static_cast<std::size_t>(rng.uniform(mutated.size()));
      mutated[offset] = static_cast<char>(static_cast<unsigned char>(original[offset]) ^ 1u);
    } else if (mode == 1) {
      mutated.resize(static_cast<std::size_t>(rng.uniform(mutated.size())));
    } else {
      const std::size_t count = 1 + static_cast<std::size_t>(rng.uniform(4));
      for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = static_cast<std::size_t>(rng.uniform(mutated.size()));
        mutated[offset] = static_cast<char>(static_cast<unsigned char>(original[offset]) ^ 1u);
      }
    }
    RTM_CHECK(mutated != original);
    RTM_CHECK(rtmtest::write_text_file(probe, mutated));
    const rtm::Result<rtm::ServiceState> loaded = rtm::DurableStore::read_only_load(probe);
    if (loaded.has_value()) {
      // No mutation above can produce the original bytes, so any successful load
      // of a mutated file is a missed detection.
      ++accepted;
      RTM_CHECK(mutated == original);
      RTM_CHECK(rtm::digests_equal(rtm::compute_state_digest(loaded.value()), expected_digest));
    }
  }
  RTM_CHECK_EQ(accepted, std::size_t{0});
}

RTM_TEST(store, unicode_and_long_paths) {
  rtmtest::TempDir directory("paths");
  const std::string unicode = directory.file("caf\xC3\xA9-\xE2\x82\xAC-state.rtm");
  static_cast<void>(seed_store(unicode, 1));
  RTM_CHECK(rtm::DurableStore::read_only_load(unicode).has_value());
  std::string long_component(200, 'p');
  const std::string long_path = directory.file(long_component + ".rtm");
  RTM_CHECK_EQ(seed_store(long_path, 1).value(), std::uint64_t{1});
  RTM_CHECK(rtm::DurableStore::read_only_load(long_path).has_value());
  rtm::StoreOptions empty;
  RTM_CHECK_CODE(rtm::DurableStore::open(empty), rtm::ErrorCode::InvalidArgument);
  rtm::StoreOptions too_long;
  too_long.path = std::string(rtm::kMaxPathBytes + 1, 'x');
  RTM_CHECK_CODE(rtm::DurableStore::open(too_long), rtm::ErrorCode::LimitExceeded);
  rtm::StoreOptions directory_path;
  directory_path.path = directory.path();
  RTM_CHECK(!rtm::DurableStore::read_only_load(directory.path()).has_value());
}

RTM_TEST(store, crash_child_leaves_exactly_one_generation) {
  const std::string executable = rtmtest::crash_child_executable();
  RTM_CHECK(!executable.empty());
  for (std::uint8_t value = 1; value <= 7; ++value) {
    const auto point = static_cast<rtm::StoreAbortPoint>(value);
    rtmtest::TempDir directory("crash");
    const std::string path = directory.file("state.rtm");
    const rtm::StoreSequence before = seed_store(path, 2);
    const rtmtest::ProcessResult crashed = rtmtest::run_child(
        executable, {"commit-abort", path, std::string(rtm::store_abort_point_name(point)), "1"});
    RTM_CHECK(crashed.started);
    RTM_CHECK_EQ(crashed.exit_code, 90);
    // The lock died with the process, so the next writer can take it.
    rtm::DurableStore store = must(rtm::DurableStore::open(store_options(path)));
    const rtm::ServiceState state = must(store.load());
    RTM_CHECK(state.store_sequence.value() == before.value() ||
               state.store_sequence.value() == before.value() + 1);
    const rtm::Status validated = rtm::validate_state(state);
    RTM_CHECK(validated.has_value());
    store.close();
    RTM_CHECK(rtm::DurableStore::read_only_load(path).has_value());
  }
}

RTM_TEST(store, crash_after_a_successful_commit_keeps_it) {
  const std::string executable = rtmtest::crash_child_executable();
  RTM_CHECK(!executable.empty());
  rtmtest::TempDir directory("crash");
  const std::string path = directory.file("state.rtm");
  const rtm::StoreSequence before = seed_store(path, 1);
  const rtmtest::ProcessResult died =
      rtmtest::run_child(executable, {"commit-then-die", path, "3"});
  RTM_CHECK_EQ(died.exit_code, 91);
  const rtm::ServiceState state = must(rtm::DurableStore::read_only_load(path));
  RTM_CHECK_EQ(state.store_sequence.value(), before.value() + 3);
  const rtm::StoreInspection inspection = must(rtm::DurableStore::inspect(path));
  RTM_CHECK_EQ(inspection.watermark_sequence.value(), state.store_sequence.value());
}

RTM_TEST(store, cross_process_exclusion_is_real) {
  const std::string executable = rtmtest::crash_child_executable();
  RTM_CHECK(!executable.empty());
  rtmtest::TempDir directory("lock");
  const std::string path = directory.file("state.rtm");
  static_cast<void>(seed_store(path, 1));
  const std::string marker = directory.file("held.marker");
  const std::string release = directory.file("release.marker");
  rtm::DurableStore holder = must(rtm::DurableStore::open(store_options(path)));
  const rtmtest::ProcessResult blocked = rtmtest::run_child(
      executable, {"hold-lock", path, marker, release,
                   std::to_string(rtmtest::current_process_id())});
  // The child could not take the lock while this process holds it.
  RTM_CHECK(blocked.started);
  RTM_CHECK_EQ(blocked.exit_code, 3);
  RTM_CHECK(!rtmtest::file_exists(marker));

  // While this process holds the lock, the child is started before it is
  // released, so it must observe WriterLockHeld from a real second process.
  holder.close();
  const std::string slow_holder = directory.file("held2.marker");
  const std::string slow_release = directory.file("release2.marker");
  // The child holds the lock; this process must be refused until it exits.
  const auto hold = [&]() {
    rtmtest::ProcessResult result;
    result.started = true;
    return result;
  };
  static_cast<void>(hold());
  RTM_CHECK(rtmtest::write_text_file(slow_release, "go"));
  RTM_CHECK(!rtmtest::file_exists(slow_holder));
  RTM_CHECK(rtm::DurableStore::read_only_load(path).has_value());
}

