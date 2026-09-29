// Rack Turnup Manager - durable store implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_turnup/store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "rack_turnup/limits.hpp"
#include "rack_turnup/text.hpp"
#include "rack_turnup/time.hpp"
#include "rack_turnup/version.hpp"

namespace rackturnup {
namespace {

constexpr std::uint32_t kEncodingModel = 1;
constexpr std::uint32_t kHeaderFlagsOffset = 12;
constexpr std::uint32_t kPayloadBytesOffset = 16;
constexpr std::uint32_t kPayloadDigestOffset = 24;
constexpr std::uint32_t kHeaderCrcOffset = 56;
constexpr std::uint32_t kEncodingModelOffset = 60;

// Native path separator as a character, spelled without a literal backslash so
// that the source reads the same in every tool.
constexpr char kNativeSeparator = '\\';

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      const bool low_bit_set = (value & 1u) != 0;
      value >>= 1;
      if (low_bit_set) {
        value ^= 0xEDB88320u;
      }
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size,
                                  std::uint32_t seed) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kCrcTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void write_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* data) noexcept {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

// Returns the error itself rather than a Status so that any Result<T> can be
// returned from it directly.
[[nodiscard]] TurnupError io_failure(std::string message, std::string path,
                                    std::uint64_t code = 0) {
  return make_error(ErrorCode::IoFailure, std::move(message),
                    ErrorDetail{.operation = "durable_store",
                                .subject = std::move(path),
                                .actual = code});
}

// ---------------------------------------------------------------------------
// Native file primitives
// ---------------------------------------------------------------------------

#if defined(_WIN32)

[[nodiscard]] Result<std::wstring> to_native_path(const std::string& path) {
  if (path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a durable store path must not be empty",
                      ErrorDetail{.operation = "to_native_path"});
  }
  if (path.size() > kMaxPathBytes) {
    return make_error(ErrorCode::LimitExceeded, "the durable store path is too long",
                      ErrorDetail{.operation = "to_native_path",
                                  .expected = kMaxPathBytes,
                                  .actual = path.size()});
  }

  std::string normalized = path;
  for (char& character : normalized) {
    if (character == '/') {
      character = kNativeSeparator;
    }
  }

  const bool drive_absolute =
      normalized.size() >= 2 && normalized[1] == ':' &&
      ((normalized[0] >= 'A' && normalized[0] <= 'Z') || (normalized[0] >= 'a' && normalized[0] <= 'z'));
  const bool unc_absolute =
      normalized.size() >= 2 && normalized[0] == kNativeSeparator &&
      normalized[1] == kNativeSeparator;

  // Windows still applies MAX_PATH to paths without the extended prefix, so a
  // long fully qualified path is prefixed rather than silently failing.
  std::string converted = normalized;
  if ((drive_absolute || unc_absolute) && normalized.size() >= 240) {
    if (unc_absolute) {
      converted = std::string("\\\\?\\UNC\\") + normalized.substr(2);
    } else {
      converted = std::string("\\\\?\\") + normalized;
    }
  }

  if (converted.empty() || converted.size() > kMaxPathBytes) {
    return make_error(ErrorCode::LimitExceeded, "the durable store path is too long",
                      ErrorDetail{.operation = "to_native_path"});
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, converted.data(),
                                           static_cast<int>(converted.size()), nullptr, 0);
  if (required <= 0) {
    return make_error(ErrorCode::InvalidCharacter, "the store path is not valid UTF-8",
                      ErrorDetail{.operation = "to_native_path", .subject = path});
  }
  std::wstring wide(static_cast<std::size_t>(required), L' ');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, converted.data(),
                                          static_cast<int>(converted.size()), wide.data(), required);
  if (written != required) {
    return io_failure("the store path could not be converted to UTF-16", path,
                      static_cast<std::uint64_t>(GetLastError()));
  }
  return wide;
}

struct FileBytes {
  bool present = false;
  std::vector<std::uint8_t> bytes;
};

[[nodiscard]] Result<FileBytes> read_file(const std::string& path) {
  const Result<std::wstring> wide = to_native_path(path);
  if (!wide.has_value()) {
    return wide.error();
  }
  HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return FileBytes{};
    }
    return io_failure("the durable file could not be opened for reading", path, error);
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    return io_failure("the durable file size could not be read", path, error);
  }
  if (size.QuadPart > static_cast<LONGLONG>(kMaxStateFileBytes + kStateHeaderBytes +
                                          kStateTrailerBytes)) {
    CloseHandle(handle);
    return make_error(ErrorCode::StateTooLarge, "the durable file exceeds the documented bound",
                      ErrorDetail{.operation = "read_file", .subject = path,
                                  .expected = kMaxStateFileBytes,
                                  .actual = static_cast<std::uint64_t>(size.QuadPart)});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart), 0);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - offset > 0x10000000u) ? 0x10000000u : (bytes.size() - offset));
    DWORD read = 0;
    if (ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) == 0) {
      const DWORD error = GetLastError();
      CloseHandle(handle);
      return io_failure("the durable file could not be read", path, error);
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(handle);
  if (offset != bytes.size()) {
    return make_error(ErrorCode::TruncatedState,
                      "the durable file ended before its recorded length",
                      ErrorDetail{.operation = "read_file", .subject = path,
                                  .expected = bytes.size(), .actual = offset});
  }
  FileBytes result;
  result.present = true;
  result.bytes = std::move(bytes);
  return result;
}

[[nodiscard]] Status write_file_flushed(const std::string& path,
                                        const std::vector<std::uint8_t>& bytes) {
  const Result<std::wstring> wide = to_native_path(path);
  if (!wide.has_value()) {
    return wide.error();
  }
  HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_failure("the staged file could not be created", path, GetLastError());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - offset > 0x10000000u) ? 0x10000000u : (bytes.size() - offset));
    DWORD written = 0;
    if (WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0) {
      const DWORD error = GetLastError();
      CloseHandle(handle);
      return io_failure("the staged file could not be written", path, error);
    }
    if (written == 0) {
      CloseHandle(handle);
      return io_failure("the staged file accepted no bytes", path);
    }
    offset += written;
  }
  if (FlushFileBuffers(handle) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    return io_failure("the staged file could not be flushed to disk", path, error);
  }
  CloseHandle(handle);
  return Status{};
}

[[nodiscard]] Status replace_file(const std::string& from, const std::string& to) {
  const Result<std::wstring> wide_from = to_native_path(from);
  if (!wide_from.has_value()) {
    return wide_from.error();
  }
  const Result<std::wstring> wide_to = to_native_path(to);
  if (!wide_to.has_value()) {
    return wide_to.error();
  }
  if (MoveFileExW(wide_from.value().c_str(), wide_to.value().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return io_failure("the staged file could not be published atomically", to, GetLastError());
  }
  return Status{};
}

void remove_file(const std::string& path) noexcept {
  const Result<std::wstring> wide = to_native_path(path);
  if (!wide.has_value()) {
    return;
  }
  DeleteFileW(wide.value().c_str());
}

[[nodiscard]] Result<void*> acquire_writer_lock(const std::string& lock_path) {
  const Result<std::wstring> wide = to_native_path(lock_path);
  if (!wide.has_value()) {
    return wide.error();
  }
  // FILE_SHARE_READ and no FILE_SHARE_WRITE is the cross-process exclusion: a
  // second writer cannot even open the lock file, and a reader never opens it.
  HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another process already holds the writer lock",
                        ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path});
    }
    return make_error(ErrorCode::LockIoFailure, "the writer lock file could not be opened",
                      ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path,
                                  .actual = error});
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                 &overlapped) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another process already holds the writer lock",
                        ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path});
    }
    return make_error(ErrorCode::LockIoFailure, "the writer lock could not be taken",
                      ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path,
                                  .actual = error});
  }
  return static_cast<void*>(handle);
}

void release_writer_lock(void* handle) noexcept {
  if (handle == nullptr) {
    return;
  }
  HANDLE native = static_cast<HANDLE>(handle);
  OVERLAPPED overlapped{};
  UnlockFileEx(native, 0, 1, 0, &overlapped);
  CloseHandle(native);
}

#else  // POSIX

[[nodiscard]] Result<std::string> to_native_path(const std::string& path) {
  if (path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a durable store path must not be empty",
                      ErrorDetail{.operation = "to_native_path"});
  }
  if (path.size() > kMaxPathBytes) {
    return make_error(ErrorCode::LimitExceeded, "the durable store path is too long",
                      ErrorDetail{.operation = "to_native_path",
                                  .expected = kMaxPathBytes,
                                  .actual = path.size()});
  }
  return path;
}

struct FileBytes {
  bool present = false;
  std::vector<std::uint8_t> bytes;
};

[[nodiscard]] Result<FileBytes> read_file(const std::string& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return FileBytes{};
    }
    return io_failure("the durable file could not be opened for reading", path,
                      static_cast<std::uint64_t>(errno));
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0) {
    const int error = errno;
    ::close(descriptor);
    return io_failure("the durable file size could not be read", path,
                      static_cast<std::uint64_t>(error));
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(descriptor);
    return io_failure("the durable path is not a regular file", path);
  }
  if (static_cast<std::uint64_t>(info.st_size) >
      kMaxStateFileBytes + kStateHeaderBytes + kStateTrailerBytes) {
    ::close(descriptor);
    return make_error(ErrorCode::StateTooLarge, "the durable file exceeds the documented bound",
                      ErrorDetail{.operation = "read_file", .subject = path,
                                  .expected = kMaxStateFileBytes,
                                  .actual = static_cast<std::uint64_t>(info.st_size)});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size), 0);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      const int error = errno;
      ::close(descriptor);
      return io_failure("the durable file could not be read", path,
                        static_cast<std::uint64_t>(error));
    }
    if (count == 0) {
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(descriptor);
  if (offset != bytes.size()) {
    return make_error(ErrorCode::TruncatedState,
                      "the durable file ended before its recorded length",
                      ErrorDetail{.operation = "read_file", .subject = path,
                                  .expected = bytes.size(), .actual = offset});
  }
  FileBytes result;
  result.present = true;
  result.bytes = std::move(bytes);
  return result;
}

[[nodiscard]] Status write_file_flushed(const std::string& path,
                                        const std::vector<std::uint8_t>& bytes) {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return io_failure("the staged file could not be created", path,
                      static_cast<std::uint64_t>(errno));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      const int error = errno;
      ::close(descriptor);
      return io_failure("the staged file could not be written", path,
                        static_cast<std::uint64_t>(error));
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    const int error = errno;
    ::close(descriptor);
    return io_failure("the staged file could not be flushed to disk", path,
                      static_cast<std::uint64_t>(error));
  }
  ::close(descriptor);
  return Status{};
}

[[nodiscard]] Status replace_file(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return io_failure("the staged file could not be published atomically", to,
                      static_cast<std::uint64_t>(errno));
  }
  const std::size_t separator = to.find_last_of('/');
  const std::string directory = (separator == std::string::npos) ? std::string(".")
                                                                 : to.substr(0, separator);
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor >= 0) {
    ::fsync(descriptor);
    ::close(descriptor);
  }
  return Status{};
}

void remove_file(const std::string& path) noexcept { ::unlink(path.c_str()); }

[[nodiscard]] Result<void*> acquire_writer_lock(const std::string& lock_path) {
  const int descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return make_error(ErrorCode::LockIoFailure, "the writer lock file could not be opened",
                      ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path,
                                  .actual = static_cast<std::uint64_t>(errno)});
  }
  struct flock lock {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 1;
  if (::fcntl(descriptor, F_SETLK, &lock) != 0) {
    const int error = errno;
    ::close(descriptor);
    if (error == EACCES || error == EAGAIN) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another process already holds the writer lock",
                        ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path});
    }
    return make_error(ErrorCode::LockIoFailure, "the writer lock could not be taken",
                      ErrorDetail{.operation = "acquire_writer_lock", .subject = lock_path,
                                  .actual = static_cast<std::uint64_t>(error)});
  }
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor + 1));
}

void release_writer_lock(void* handle) noexcept {
  if (handle == nullptr) {
    return;
  }
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle)) - 1;
  ::close(descriptor);
}

#endif

// ---------------------------------------------------------------------------
// Container layout
// ---------------------------------------------------------------------------

struct Container {
  std::uint32_t format_version = 0;
  std::uint32_t encoding_model = 0;
  Digest payload_digest;
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> build_container(
    const std::vector<std::uint8_t>& payload) {
  const Digest payload_digest = sha256(payload.data(), payload.size());
  std::vector<std::uint8_t> out;
  out.reserve(kStateHeaderBytes + payload.size() + kStateTrailerBytes);
  for (std::size_t i = 0; i < sizeof(kStateFileMagic); ++i) {
    out.push_back(static_cast<std::uint8_t>(kStateFileMagic[i]));
  }
  write_u32(out, kStateFormatVersion);
  write_u32(out, 0);
  write_u64(out, static_cast<std::uint64_t>(payload.size()));
  const std::vector<std::uint8_t>& digest_bytes = payload_digest.bytes();
  out.insert(out.end(), digest_bytes.begin(), digest_bytes.end());
  write_u32(out, crc32(out.data(), kHeaderCrcOffset, 0));
  write_u32(out, kEncodingModel);
  out.insert(out.end(), payload.begin(), payload.end());
  const std::uint32_t payload_crc = crc32(payload.data(), payload.size(), 0);
  write_u32(out, payload_crc);
  const std::uint8_t crc_bytes[4] = {static_cast<std::uint8_t>((payload_crc >> 24) & 0xFFu),
                                     static_cast<std::uint8_t>((payload_crc >> 16) & 0xFFu),
                                     static_cast<std::uint8_t>((payload_crc >> 8) & 0xFFu),
                                     static_cast<std::uint8_t>(payload_crc & 0xFFu)};
  write_u32(out, crc32(crc_bytes, 4, 0));
  return out;
}

[[nodiscard]] Result<Container> verify_container(const std::string& path,
                                                 const std::vector<std::uint8_t>& file) {
  if (file.size() < kStateHeaderBytes + kStateTrailerBytes) {
    return make_error(ErrorCode::TruncatedState,
                      "the durable file is shorter than its fixed header and trailer",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = kStateHeaderBytes + kStateTrailerBytes,
                                  .actual = file.size()});
  }
  for (std::size_t i = 0; i < sizeof(kStateFileMagic); ++i) {
    if (file[i] != static_cast<std::uint8_t>(kStateFileMagic[i])) {
      return make_error(ErrorCode::CorruptState,
                        "the durable file does not start with the expected magic",
                        ErrorDetail{.operation = "verify_container", .subject = path});
    }
  }
  const std::uint32_t format_version = read_u32(file.data() + 8);
  if (format_version != kStateFormatVersion) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the durable format version is not supported by this build",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = kStateFormatVersion, .actual = format_version});
  }
  const std::uint32_t flags = read_u32(file.data() + kHeaderFlagsOffset);
  if (flags != 0) {
    return make_error(ErrorCode::ReservedBitsSet, "a reserved header field is not zero",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .actual = flags});
  }
  const std::uint64_t payload_bytes = read_u64(file.data() + kPayloadBytesOffset);
  if (payload_bytes > kMaxStateFileBytes) {
    return make_error(ErrorCode::StateTooLarge, "the declared payload exceeds the documented bound",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = kMaxStateFileBytes, .actual = payload_bytes});
  }
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(kStateHeaderBytes) + payload_bytes + kStateTrailerBytes;
  if (static_cast<std::uint64_t>(file.size()) < expected_size) {
    return make_error(ErrorCode::TruncatedState,
                      "the durable file ends before the declared payload",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = expected_size, .actual = file.size()});
  }
  if (static_cast<std::uint64_t>(file.size()) > expected_size) {
    return make_error(ErrorCode::TrailingBytes,
                      "the durable file continues past the declared payload",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = expected_size, .actual = file.size()});
  }
  const std::uint32_t header_crc = read_u32(file.data() + kHeaderCrcOffset);
  if (header_crc != crc32(file.data(), kHeaderCrcOffset, 0)) {
    return make_error(ErrorCode::IntegrityCheckFailed, "the durable header checksum is wrong",
                      ErrorDetail{.operation = "verify_container", .subject = path});
  }
  const std::uint32_t encoding_model = read_u32(file.data() + kEncodingModelOffset);
  if (encoding_model != kEncodingModel) {
    return make_error(ErrorCode::UnsupportedFormatVersion,
                      "the durable encoding model is not supported by this build",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .expected = kEncodingModel, .actual = encoding_model});
  }

  const std::uint8_t* payload_data = file.data() + kStateHeaderBytes;
  const std::uint32_t payload_crc = read_u32(payload_data + payload_bytes);
  if (payload_crc != crc32(payload_data, static_cast<std::size_t>(payload_bytes), 0)) {
    return make_error(ErrorCode::IntegrityCheckFailed, "the payload checksum is wrong",
                      ErrorDetail{.operation = "verify_container", .subject = path});
  }
  const std::uint32_t trailer_crc = read_u32(payload_data + payload_bytes + 4);
  if (trailer_crc != crc32(payload_data + payload_bytes, 4, 0)) {
    return make_error(ErrorCode::IntegrityCheckFailed, "the trailer checksum is wrong",
                      ErrorDetail{.operation = "verify_container", .subject = path});
  }

  const Digest recorded = Digest::of(std::vector<std::uint8_t>(file.begin() + kPayloadDigestOffset,
                                                               file.begin() + kPayloadDigestOffset +
                                                                   Digest::kBytes));
  const Digest computed = sha256(payload_data, static_cast<std::size_t>(payload_bytes));
  if (!digests_equal(recorded, computed)) {
    return make_error(ErrorCode::IntegrityCheckFailed, "the payload digest is wrong",
                      ErrorDetail{.operation = "verify_container", .subject = path,
                                  .related = computed.to_hex()});
  }

  Container container;
  container.format_version = format_version;
  container.encoding_model = encoding_model;
  container.payload_digest = computed;
  container.payload.assign(payload_data, payload_data + payload_bytes);
  return container;
}

// ---------------------------------------------------------------------------
// Rollback watermark
// ---------------------------------------------------------------------------

struct Watermark {
  bool present = false;
  StoreSequence sequence;
  StoreEpoch epoch;
  Digest digest;
};

[[nodiscard]] Result<Watermark> parse_watermark(const std::string& path,
                                                const std::string& text) {
  std::vector<std::string> lines = split(text, '\n');
  // The record is written with a terminating newline, so exactly one empty
  // trailing field is allowed and nothing else.
  if (lines.size() == 5 && lines[4].empty()) {
    lines.resize(4);
  }
  if (lines.size() != 4 || lines[0] != "rtm-watermark-v1") {
    return make_error(ErrorCode::CorruptState, "the publication watermark is malformed",
                      ErrorDetail{.operation = "parse_watermark", .subject = path});
  }
  std::uint64_t sequence = 0;
  std::uint64_t epoch = 0;
  const auto value_of = [&path](const std::string& line, std::string_view key) -> Result<std::string> {
    const std::string prefix = std::string(key) + "=";
    if (line.size() <= prefix.size() || line.compare(0, prefix.size(), prefix) != 0) {
      return make_error(ErrorCode::CorruptState, "the publication watermark has an unexpected field",
                        ErrorDetail{.operation = "parse_watermark", .subject = path,
                                    .related = line});
    }
    return line.substr(prefix.size());
  };
  const Result<std::string> sequence_text = value_of(lines[1], "sequence");
  const Result<std::string> epoch_text = value_of(lines[2], "epoch");
  const Result<std::string> digest_text = value_of(lines[3], "digest");
  if (!sequence_text.has_value()) {
    return sequence_text.error();
  }
  if (!epoch_text.has_value()) {
    return epoch_text.error();
  }
  if (!digest_text.has_value()) {
    return digest_text.error();
  }
  if (!parse_u64(sequence_text.value(), sequence) || !parse_u64(epoch_text.value(), epoch)) {
    return make_error(ErrorCode::CorruptState, "the publication watermark is not numeric",
                      ErrorDetail{.operation = "parse_watermark", .subject = path});
  }
  const Result<Digest> digest = Digest::parse(digest_text.value());
  if (!digest.has_value()) {
    return digest.error();
  }
  const Result<StoreSequence> parsed_sequence = StoreSequence::create(sequence);
  if (!parsed_sequence.has_value()) {
    return parsed_sequence.error();
  }
  const Result<StoreEpoch> parsed_epoch = StoreEpoch::create(epoch);
  if (!parsed_epoch.has_value()) {
    return parsed_epoch.error();
  }
  Watermark watermark;
  watermark.present = true;
  watermark.sequence = parsed_sequence.value();
  watermark.epoch = parsed_epoch.value();
  watermark.digest = digest.value();
  return watermark;
}

[[nodiscard]] Result<Watermark> read_watermark(const std::string& path) {
  const Result<FileBytes> file = read_file(path);
  if (!file.has_value()) {
    return file.error();
  }
  if (!file.value().present) {
    return Watermark{};
  }
  const std::vector<std::uint8_t>& bytes = file.value().bytes;
  return parse_watermark(path, std::string(bytes.begin(), bytes.end()));
}

[[nodiscard]] Status write_watermark(const std::string& path, StoreSequence sequence,
                                     StoreEpoch epoch, const Digest& digest) {
  std::string text = "rtm-watermark-v1\n";
  text.append("sequence=");
  text.append(std::to_string(sequence.value()));
  text.push_back('\n');
  text.append("epoch=");
  text.append(std::to_string(epoch.value()));
  text.push_back('\n');
  text.append("digest=");
  text.append(digest.to_hex());
  text.push_back('\n');
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  const std::string staged = path + ".staged";
  const Status written = write_file_flushed(staged, bytes);
  if (!written.has_value()) {
    return written;
  }
  return replace_file(staged, path);
}

}  // namespace

std::string_view store_abort_point_name(StoreAbortPoint point) noexcept {
  switch (point) {
    case StoreAbortPoint::None:
      return "none";
    case StoreAbortPoint::BeforeWrite:
      return "before_write";
    case StoreAbortPoint::AfterWrite:
      return "after_write";
    case StoreAbortPoint::AfterFlush:
      return "after_flush";
    case StoreAbortPoint::AfterReadBack:
      return "after_read_back";
    case StoreAbortPoint::BeforePublish:
      return "before_publish";
    case StoreAbortPoint::AfterPublish:
      return "after_publish";
    case StoreAbortPoint::AfterWatermark:
      return "after_watermark";
  }
  return "none";
}

Result<StoreAbortPoint> store_abort_point_from_name(std::string_view name) {
  for (std::uint8_t value = 0; value <= 7; ++value) {
    const auto point = static_cast<StoreAbortPoint>(value);
    if (store_abort_point_name(point) == name) {
      return point;
    }
  }
  return make_error(ErrorCode::InvalidEnumValue, "unknown commit abort point name",
                    ErrorDetail{.operation = "store_abort_point_from_name",
                                .subject = std::string(name)});
}

DurableStore::~DurableStore() { close(); }

DurableStore::DurableStore(DurableStore&& other) noexcept
    : options_(std::move(other.options_)),
      lock_path_(std::move(other.lock_path_)),
      watermark_path_(std::move(other.watermark_path_)),
      lock_handle_(other.lock_handle_),
      open_(other.open_),
      writable_(other.writable_),
      sequence_(other.sequence_),
      epoch_(other.epoch_),
      temp_counter_(other.temp_counter_) {
  other.lock_handle_ = nullptr;
  other.open_ = false;
  other.writable_ = false;
}

DurableStore& DurableStore::operator=(DurableStore&& other) noexcept {
  if (this != &other) {
    close();
    options_ = std::move(other.options_);
    lock_path_ = std::move(other.lock_path_);
    watermark_path_ = std::move(other.watermark_path_);
    lock_handle_ = other.lock_handle_;
    open_ = other.open_;
    writable_ = other.writable_;
    sequence_ = other.sequence_;
    epoch_ = other.epoch_;
    temp_counter_ = other.temp_counter_;
    other.lock_handle_ = nullptr;
    other.open_ = false;
    other.writable_ = false;
  }
  return *this;
}

void DurableStore::release_lock() noexcept {
  release_writer_lock(lock_handle_);
  lock_handle_ = nullptr;
}

void DurableStore::close() noexcept {
  release_lock();
  open_ = false;
  writable_ = false;
}

void DurableStore::reach_abort_point(StoreAbortPoint point) {
  if (options_.abort_hook != nullptr && options_.abort_point == point &&
      point != StoreAbortPoint::None) {
    options_.abort_hook(point);
  }
}

Result<DurableStore> DurableStore::open(const StoreOptions& options) {
  if (options.path.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a durable store requires a path",
                      ErrorDetail{.operation = "DurableStore::open"});
  }
  if (options.path.size() > kMaxPathBytes) {
    return make_error(ErrorCode::LimitExceeded, "the durable store path is too long",
                      ErrorDetail{.operation = "DurableStore::open",
                                  .expected = kMaxPathBytes,
                                  .actual = options.path.size()});
  }

  DurableStore store;
  store.options_ = options;
  store.lock_path_ = options.path + ".lock";
  store.watermark_path_ = options.path + ".watermark";
  store.sequence_ = StoreSequence::initial();
  store.epoch_ = StoreEpoch::initial();

  if (!options.read_only) {
    const Result<void*> handle = acquire_writer_lock(store.lock_path_);
    if (!handle.has_value()) {
      return handle.error();
    }
    store.lock_handle_ = handle.value();
    store.writable_ = true;
  }
  if (!options.create_if_missing) {
    const Result<FileBytes> existing = read_file(options.path);
    if (!existing.has_value()) {
      return existing.error();
    }
    if (!existing.value().present) {
      store.release_lock();
      return make_error(ErrorCode::NoAuthoritativeState,
                        "no durable state exists at the configured path",
                        ErrorDetail{.operation = "DurableStore::open", .subject = options.path});
    }
  }
  store.open_ = true;
  return Result<DurableStore>(std::move(store));
}

Result<ServiceState> DurableStore::load() {
  if (!open_) {
    return make_error(ErrorCode::InvalidArgument, "the durable store is not open",
                      ErrorDetail{.operation = "DurableStore::load"});
  }
  const Result<FileBytes> file = read_file(options_.path);
  if (!file.has_value()) {
    return file.error();
  }
  if (!file.value().present) {
    if (writable_ && options_.create_if_missing) {
      ServiceState state;
      state.store_epoch = StoreEpoch::initial();
      state.store_sequence = StoreSequence::initial();
      state.incarnation = IncarnationId::initial();
      state.control_epoch = ControlEpoch::initial();
      state.created_at = WallClock::now();
      state.updated_at = state.created_at;
      sequence_ = state.store_sequence;
      epoch_ = state.store_epoch;
      return state;
    }
    return make_error(ErrorCode::NoAuthoritativeState,
                      "no durable state exists at the configured path",
                      ErrorDetail{.operation = "DurableStore::load", .subject = options_.path});
  }

  const Result<Container> container = verify_container(options_.path, file.value().bytes);
  if (!container.has_value()) {
    return container.error();
  }
  const Result<Watermark> watermark = read_watermark(watermark_path_);
  if (!watermark.has_value()) {
    return watermark.error();
  }

  const Result<ServiceState> decoded = decode_state(container.value().payload);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  ServiceState state = decoded.value();

  if (watermark.value().present) {
    if (watermark.value().sequence > state.store_sequence) {
      return make_error(ErrorCode::StaleDurableState,
                        "the state file predates the recorded publication watermark",
                        ErrorDetail{.operation = "DurableStore::load", .subject = options_.path,
                                    .expected = watermark.value().sequence.value(),
                                    .actual = state.store_sequence.value()});
    }
    if (watermark.value().sequence == state.store_sequence &&
        !digests_equal(watermark.value().digest, container.value().payload_digest)) {
      return make_error(ErrorCode::IntegrityCheckFailed,
                        "the state file does not match the published generation",
                        ErrorDetail{.operation = "DurableStore::load", .subject = options_.path,
                                    .related = container.value().payload_digest.to_hex()});
    }
  }

  if (!writable_) {
    return state;
  }

  // The store hands the caller the generation it now owns: the epoch advances by
  // one on takeover, and the sequence advances when the next generation is
  // published. Nothing is written here.
  if (state.store_epoch.is_max()) {
    return make_error(ErrorCode::LimitExceeded, "the store epoch is exhausted",
                      ErrorDetail{.operation = "DurableStore::load", .subject = options_.path});
  }
  if (state.incarnation.is_max()) {
    return make_error(ErrorCode::LimitExceeded, "the incarnation counter is exhausted",
                      ErrorDetail{.operation = "DurableStore::load", .subject = options_.path});
  }
  sequence_ = state.store_sequence;
  epoch_ = state.store_epoch.next();
  state.store_epoch = epoch_;
  state.incarnation = state.incarnation.next();
  return state;
}

Status DurableStore::commit(const ServiceState& state) {
  if (!open_) {
    return make_error(ErrorCode::InvalidArgument, "the durable store is not open",
                      ErrorDetail{.operation = "DurableStore::commit"});
  }
  if (!writable_) {
    return make_error(ErrorCode::ReadOnlyStore, "the durable store was opened read-only",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path});
  }
  if (!(state.store_epoch == epoch_)) {
    return make_error(ErrorCode::StoreEpochRegression,
                      "the state does not carry the epoch this writer owns",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                  .expected = epoch_.value(), .actual = state.store_epoch.value()});
  }
  if (sequence_.is_max()) {
    return make_error(ErrorCode::LimitExceeded, "the publication sequence is exhausted",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path});
  }
  if (!(state.store_sequence == sequence_.next())) {
    return make_error(ErrorCode::SequenceRegression,
                      "a commit must publish exactly the next sequence",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                  .expected = sequence_.next().value(),
                                  .actual = state.store_sequence.value()});
  }

  const Status valid = validate_state(state);
  if (!valid.has_value()) {
    return valid;
  }

  // Re-read the generation that is currently on disk before staging anything: a
  // writer whose predecessor published a newer generation, or a state file that
  // was replaced by an older copy, must not be allowed to publish.
  const Result<FileBytes> current = read_file(options_.path);
  if (!current.has_value()) {
    return current.error();
  }
  if (current.value().present) {
    const Result<Container> container = verify_container(options_.path, current.value().bytes);
    if (!container.has_value()) {
      return container.error();
    }
    const Result<ServiceState> on_disk = decode_state(container.value().payload);
    if (!on_disk.has_value()) {
      return on_disk.error();
    }
    if (!(on_disk.value().store_sequence == sequence_)) {
      return make_error(ErrorCode::StaleWriterFenced,
                        "another writer published a different generation",
                        ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                    .expected = sequence_.value(),
                                    .actual = on_disk.value().store_sequence.value()});
    }
    if (on_disk.value().store_epoch.value() > epoch_.value()) {
      return make_error(ErrorCode::WriterIdentityMismatch,
                        "the durable state belongs to a newer writer epoch",
                        ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                    .expected = epoch_.value(),
                                    .actual = on_disk.value().store_epoch.value()});
    }
    const Result<Watermark> watermark = read_watermark(watermark_path_);
    if (!watermark.has_value()) {
      return watermark.error();
    }
    if (watermark.value().present && watermark.value().sequence > sequence_) {
      return make_error(ErrorCode::StaleDurableState,
                        "the state file predates the recorded publication watermark",
                        ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                    .expected = watermark.value().sequence.value(),
                                    .actual = sequence_.value()});
    }
  }

  const Result<std::vector<std::uint8_t>> payload = encode_state(state);
  if (!payload.has_value()) {
    return payload.error();
  }
  if (static_cast<std::uint64_t>(payload.value().size()) > kMaxStateFileBytes) {
    return make_error(ErrorCode::StateTooLarge, "the encoded state exceeds the documented bound",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = options_.path,
                                  .expected = kMaxStateFileBytes,
                                  .actual = payload.value().size()});
  }
  const std::vector<std::uint8_t> container = build_container(payload.value());
  const std::string staged = options_.path + ".staged";

  reach_abort_point(StoreAbortPoint::BeforeWrite);
  const Status written = write_file_flushed(staged, container);
  if (!written.has_value()) {
    remove_file(staged);
    return written;
  }
  reach_abort_point(StoreAbortPoint::AfterWrite);
  reach_abort_point(StoreAbortPoint::AfterFlush);

  const Result<FileBytes> read_back = read_file(staged);
  if (!read_back.has_value()) {
    remove_file(staged);
    return read_back.error();
  }
  if (!read_back.value().present) {
    remove_file(staged);
    return make_error(ErrorCode::PublicationFailed, "the staged file disappeared before verification",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = staged});
  }
  const Result<Container> verified = verify_container(staged, read_back.value().bytes);
  if (!verified.has_value()) {
    remove_file(staged);
    return verified.error();
  }
  const Result<ServiceState> staged_state = decode_state(verified.value().payload);
  if (!staged_state.has_value()) {
    remove_file(staged);
    return staged_state.error();
  }
  if (!digests_equal(compute_state_digest(staged_state.value()), compute_state_digest(state))) {
    remove_file(staged);
    return make_error(ErrorCode::PublicationFailed,
                      "the staged generation does not read back as the state that was committed",
                      ErrorDetail{.operation = "DurableStore::commit", .subject = staged});
  }
  reach_abort_point(StoreAbortPoint::AfterReadBack);

  reach_abort_point(StoreAbortPoint::BeforePublish);
  const Status published = replace_file(staged, options_.path);
  if (!published.has_value()) {
    remove_file(staged);
    return make_error(ErrorCode::PublicationFailed, published.error().message,
                      published.error().detail);
  }
  reach_abort_point(StoreAbortPoint::AfterPublish);

  const Status watermark_written =
      write_watermark(watermark_path_, state.store_sequence, state.store_epoch, verified.value().payload_digest);
  if (!watermark_written.has_value()) {
    return watermark_written;
  }
  reach_abort_point(StoreAbortPoint::AfterWatermark);

  // Durable fencing advances only after the new generation is on disk and
  // verified. A failure above leaves the previous generation authoritative.
  sequence_ = state.store_sequence;
  epoch_ = state.store_epoch;
  return Status{};
}

Result<StoreInspection> DurableStore::inspect(const std::string& path) {
  StoreInspection inspection;
  const Result<FileBytes> file = read_file(path);
  if (!file.has_value()) {
    return file.error();
  }
  if (!file.value().present) {
    return inspection;
  }
  inspection.present = true;
  inspection.file_bytes = file.value().bytes.size();
  const Result<Container> container = verify_container(path, file.value().bytes);
  if (!container.has_value()) {
    return container.error();
  }
  inspection.format_version = container.value().format_version;
  inspection.encoding_model = container.value().encoding_model;
  inspection.payload_bytes = container.value().payload.size();
  inspection.payload_digest = container.value().payload_digest;
  const Result<ServiceState> decoded = decode_state(container.value().payload);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  inspection.state_digest = compute_state_digest(decoded.value());
  inspection.store_epoch = decoded.value().store_epoch;
  inspection.store_sequence = decoded.value().store_sequence;
  inspection.incarnation = decoded.value().incarnation;
  inspection.rack_count = decoded.value().racks.size();
  const Result<Watermark> watermark = read_watermark(path + ".watermark");
  if (!watermark.has_value()) {
    return watermark.error();
  }
  inspection.watermark_present = watermark.value().present;
  inspection.watermark_sequence = watermark.value().sequence;
  inspection.watermark_digest = watermark.value().digest;
  if (inspection.watermark_present && inspection.watermark_sequence > inspection.store_sequence) {
    return make_error(ErrorCode::StaleDurableState,
                      "the state file predates the recorded publication watermark",
                      ErrorDetail{.operation = "DurableStore::inspect", .subject = path,
                                  .expected = inspection.watermark_sequence.value(),
                                  .actual = inspection.store_sequence.value()});
  }
  return inspection;
}

Result<ServiceState> DurableStore::read_only_load(const std::string& path) {
  StoreOptions options;
  options.path = path;
  options.read_only = true;
  options.create_if_missing = false;
  Result<DurableStore> store = DurableStore::open(options);
  if (!store.has_value()) {
    return store.error();
  }
  return store.value().load();
}

}  // namespace rackturnup
