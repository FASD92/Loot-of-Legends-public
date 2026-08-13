#include <lol/settlement_storage/StorageProbe.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lol::settlement_storage {
namespace {

int secureFlags(int flags) {
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

bool syncData(int descriptor) {
#ifdef __APPLE__
  return ::fsync(descriptor) == 0;
#else
  return ::fdatasync(descriptor) == 0;
#endif
}

bool syncDirectory(const std::filesystem::path &path) {
  auto flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const auto descriptor = ::open(path.c_str(), secureFlags(flags));
  if (descriptor < 0) {
    return false;
  }
  const auto synced = ::fsync(descriptor) == 0;
  const auto closed = ::close(descriptor) == 0;
  return synced && closed;
}

} // namespace

StorageProbeResult probeJournalDirectory(const std::filesystem::path &path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || status.type() == std::filesystem::file_type::not_found) {
    return StorageProbeResult::MissingDirectory;
  }
  if (status.type() == std::filesystem::file_type::symlink ||
      status.type() != std::filesystem::file_type::directory) {
    return StorageProbeResult::UnsafePath;
  }
  constexpr auto forbiddenPermissions =
      std::filesystem::perms::group_all | std::filesystem::perms::others_all;
  if ((status.permissions() & forbiddenPermissions) !=
      std::filesystem::perms::none) {
    return StorageProbeResult::InsecurePermissions;
  }

  constexpr std::array<std::uint8_t, 8> sentinel = {0x4c, 0x4f, 0x4f, 0x54,
                                                    0x50, 0x52, 0x4f, 0x42};
  std::filesystem::path probePath;
  int descriptor = -1;
  for (std::uint32_t attempt = 0; attempt < 8u && descriptor < 0; ++attempt) {
    probePath = path / (".settlement-probe-" + std::to_string(::getpid()) +
                        "-" + std::to_string(attempt));
    descriptor =
        ::open(probePath.c_str(), secureFlags(O_RDWR | O_CREAT | O_EXCL),
               S_IRUSR | S_IWUSR);
  }
  if (descriptor < 0) {
    return StorageProbeResult::IoFailure;
  }

  std::array<std::uint8_t, sentinel.size()> observed{};
  const auto written = ::write(descriptor, sentinel.data(), sentinel.size()) ==
                       static_cast<ssize_t>(sentinel.size());
  const auto synced = written && syncData(descriptor);
  const auto read =
      synced && ::pread(descriptor, observed.data(), observed.size(), 0) ==
                    static_cast<ssize_t>(observed.size());
  const auto closed = ::close(descriptor) == 0;
  const auto removed = ::unlink(probePath.c_str()) == 0;
  const auto directorySynced = removed && syncDirectory(path);
  return written && synced && read && observed == sentinel && closed &&
                 removed && directorySynced
             ? StorageProbeResult::Ready
             : StorageProbeResult::IoFailure;
}

} // namespace lol::settlement_storage
