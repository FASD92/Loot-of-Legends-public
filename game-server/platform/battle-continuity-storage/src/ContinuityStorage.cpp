#include <lol/battle_continuity_storage/ContinuityStorage.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lol::battle_continuity_storage {
namespace {

using battle_continuity::BattleIdentity;
using battle_continuity::Bytes;
using battle_continuity::Record;
using battle_continuity::RecordType;

constexpr std::uint32_t kManifestMagic = 0x4C42434DU; // LBCM
constexpr std::uint16_t kManifestSchemaVersion = 1U;
constexpr std::size_t kKeyBytes = 32U;
constexpr std::size_t kKeyDigestBytes = 32U;
constexpr std::uint32_t kManifestPayloadLength =
    sizeof(std::uint32_t) + kKeyDigestBytes;
constexpr std::size_t kManifestBytes =
    4U + 2U + 4U + kManifestPayloadLength + 4U;
constexpr std::uint32_t kTombstoneMagic = 0x4C42544DU; // LBTM
constexpr std::uint16_t kTombstoneSchemaVersion = 1U;
constexpr std::uint32_t kTombstonePayloadLength =
    sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
    sizeof(std::uint64_t) + sizeof(std::uint32_t);
constexpr std::size_t kTombstoneBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t) +
    kTombstonePayloadLength + sizeof(std::uint32_t);
constexpr std::uint32_t kEnvelopeMagic = 0x4C424531U; // LBE1
constexpr std::uint16_t kEnvelopeSchemaVersion = 1U;
constexpr std::size_t kEnvelopeNonceBytes = 12U;
constexpr std::size_t kEnvelopeTagBytes = 16U;
constexpr std::size_t kMaximumEnvelopePlaintextBytes = 1U << 20U;
constexpr std::size_t kMaximumQueuedBytes =
    battle_continuity::kMaximumJournalBytes;

int noFollowFlags(int flags) noexcept {
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

void appendU16(Bytes &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(Bytes &bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU64(Bytes &bytes, std::uint64_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 56U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 48U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 40U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 32U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes,
                      std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
      static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes,
                      std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes,
                      std::size_t offset) noexcept {
  return (static_cast<std::uint64_t>(bytes[offset]) << 56U) |
         (static_cast<std::uint64_t>(bytes[offset + 1U]) << 48U) |
         (static_cast<std::uint64_t>(bytes[offset + 2U]) << 40U) |
         (static_cast<std::uint64_t>(bytes[offset + 3U]) << 32U) |
         (static_cast<std::uint64_t>(bytes[offset + 4U]) << 24U) |
         (static_cast<std::uint64_t>(bytes[offset + 5U]) << 16U) |
         (static_cast<std::uint64_t>(bytes[offset + 6U]) << 8U) |
         static_cast<std::uint64_t>(bytes[offset + 7U]);
}

bool isRegular(int descriptor) noexcept {
  struct stat status {};
  return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
}

bool isOwnerOnlyRegular(int descriptor) noexcept {
  struct stat status {};
  return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_uid == ::geteuid() &&
         (status.st_mode & 07777) == (S_IRUSR | S_IWUSR);
}

bool writeAll(int descriptor, std::span<const std::uint8_t> bytes) noexcept {
  std::size_t written = 0U;
  while (written < bytes.size()) {
    const auto result =
        ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

bool readAt(int descriptor, std::span<std::uint8_t> bytes) noexcept {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto result =
        ::pread(descriptor, bytes.data() + offset, bytes.size() - offset,
                static_cast<off_t>(offset));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(result);
  }
  return true;
}

bool syncData(int descriptor) noexcept {
#ifdef __APPLE__
  return ::fsync(descriptor) == 0;
#else
  return ::fdatasync(descriptor) == 0;
#endif
}

bool syncDirectory(int descriptor) noexcept { return ::fsync(descriptor) == 0; }

bool validIdentity(const BattleIdentity &identity) noexcept {
  const auto room = identity.roomId.value();
  return identity.originRecoveryEpoch != 0U && room != 0U &&
         static_cast<std::uint32_t>(room >> 32U) ==
             identity.originRecoveryEpoch &&
         static_cast<std::uint32_t>(room) != 0U &&
         identity.battleInstanceId.value() != 0U;
}

bool sameIdentity(const BattleIdentity &left,
                  const BattleIdentity &right) noexcept {
  return left == right;
}

std::string hexValue(std::uint64_t value, std::size_t digits) {
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result(digits, '0');
  for (std::size_t index = 0U; index < digits; ++index) {
    const auto shift = static_cast<unsigned int>((digits - index - 1U) * 4U);
    result[index] = alphabet[(value >> shift) & 0x0FU];
  }
  return result;
}

std::filesystem::path journalPathFor(const std::filesystem::path &root,
                                     const BattleIdentity &identity) {
  const auto name =
      std::string{"battle-"} + hexValue(identity.originRecoveryEpoch, 8U) +
      "-" + hexValue(identity.roomId.value(), 16U) + "-" +
      hexValue(identity.battleInstanceId.value(), 16U) + ".journal";
  return root / name;
}

std::filesystem::path envelopePathFor(const std::filesystem::path &root,
                                      const BattleIdentity &identity) {
  auto path = journalPathFor(root, identity);
  path.replace_extension(".envelope");
  return path;
}

enum class TombstoneKind : std::uint8_t { Retired = 1U, Quarantined = 2U };

std::filesystem::path tombstonePathFor(const std::filesystem::path &root,
                                       const BattleIdentity &identity,
                                       TombstoneKind kind) {
  auto path = journalPathFor(root, identity);
  path.replace_extension(kind == TombstoneKind::Retired ? ".retired"
                                                        : ".quarantine");
  return path;
}

struct FileReadResult final {
  std::optional<Bytes> bytes;
  StorageError error{StorageError::None};
  bool missing{false};
};

FileReadResult readRegularFile(const std::filesystem::path &path,
                               std::size_t maximumBytes) noexcept {
  const auto descriptor = ::open(path.c_str(), noFollowFlags(O_RDONLY));
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return FileReadResult{
          .bytes = std::nullopt, .error = StorageError::None, .missing = true};
    }
    return FileReadResult{.bytes = std::nullopt,
                          .error = StorageError::JournalIo,
                          .missing = false};
  }
  struct stat status {};
  const bool statOk = ::fstat(descriptor, &status) == 0;
  if (!statOk || !isOwnerOnlyRegular(descriptor) ||
      static_cast<std::uintmax_t>(status.st_size) > maximumBytes ||
      status.st_size < 0) {
    (void)::close(descriptor);
    return FileReadResult{.bytes = std::nullopt,
                          .error = StorageError::JournalCorrupt,
                          .missing = false};
  }
  Bytes bytes;
  try {
    bytes.resize(static_cast<std::size_t>(status.st_size));
  } catch (...) {
    (void)::close(descriptor);
    return FileReadResult{.bytes = std::nullopt,
                          .error = StorageError::JournalIo,
                          .missing = false};
  }
  const bool readOk = readAt(descriptor, bytes);
  const bool closeOk = ::close(descriptor) == 0;
  if (!readOk || !closeOk) {
    return FileReadResult{.bytes = std::nullopt,
                          .error = StorageError::JournalIo,
                          .missing = false};
  }
  return FileReadResult{
      .bytes = std::move(bytes), .error = StorageError::None, .missing = false};
}

struct ParsedTombstone final {
  BattleIdentity identity{.originRecoveryEpoch = 0U,
                          .roomId = shared::RoomId{0U},
                          .battleInstanceId = shared::BattleInstanceId{0U}};
  std::uint32_t writerRecoveryEpoch{0U};
};

Bytes tombstoneBytes(TombstoneKind kind, const BattleIdentity &identity,
                     std::uint32_t writerRecoveryEpoch) {
  Bytes bytes;
  bytes.reserve(kTombstoneBytes);
  appendU32(bytes, kTombstoneMagic);
  appendU16(bytes, kTombstoneSchemaVersion);
  appendU32(bytes, kTombstonePayloadLength);
  bytes.push_back(static_cast<std::uint8_t>(kind));
  appendU32(bytes, identity.originRecoveryEpoch);
  appendU64(bytes, identity.roomId.value());
  appendU64(bytes, identity.battleInstanceId.value());
  appendU32(bytes, writerRecoveryEpoch);
  appendU32(bytes, battle_continuity::crc32(bytes));
  return bytes;
}

std::optional<ParsedTombstone>
parseTombstone(std::span<const std::uint8_t> bytes,
               TombstoneKind expectedKind) {
  if (bytes.size() != kTombstoneBytes ||
      readU32(bytes, 0U) != kTombstoneMagic ||
      readU16(bytes, 4U) != kTombstoneSchemaVersion ||
      readU32(bytes, 6U) != kTombstonePayloadLength ||
      bytes[10U] != static_cast<std::uint8_t>(expectedKind) ||
      readU32(bytes, 35U) != battle_continuity::crc32(bytes.first(35U))) {
    return std::nullopt;
  }
  const BattleIdentity identity{
      .originRecoveryEpoch = readU32(bytes, 11U),
      .roomId = shared::RoomId{readU64(bytes, 15U)},
      .battleInstanceId = shared::BattleInstanceId{readU64(bytes, 23U)}};
  const auto writerRecoveryEpoch = readU32(bytes, 31U);
  if (!validIdentity(identity) || writerRecoveryEpoch == 0U) {
    return std::nullopt;
  }
  return ParsedTombstone{.identity = identity,
                         .writerRecoveryEpoch = writerRecoveryEpoch};
}

enum class TombstoneStatus : std::uint8_t { Absent, Valid, Corrupt };

TombstoneStatus tombstoneStatus(const std::filesystem::path &root,
                                const BattleIdentity &identity,
                                TombstoneKind kind) noexcept {
  const auto file =
      readRegularFile(tombstonePathFor(root, identity, kind), kTombstoneBytes);
  if (file.missing) {
    return TombstoneStatus::Absent;
  }
  const auto parsed =
      file.bytes.has_value() ? parseTombstone(*file.bytes, kind) : std::nullopt;
  if (!parsed.has_value() || !sameIdentity(parsed->identity, identity)) {
    return TombstoneStatus::Corrupt;
  }
  return TombstoneStatus::Valid;
}

std::optional<std::uint64_t> parseHex(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0U;
  for (const auto character : text) {
    std::uint8_t digit = 0U;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint8_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint8_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint8_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 16U) {
      return std::nullopt;
    }
    value = value * 16U + digit;
  }
  return value;
}

std::optional<BattleIdentity>
identityFromJournalFilename(std::string_view name) noexcept {
  constexpr std::string_view prefix = "battle-";
  constexpr std::string_view suffix = ".journal";
  constexpr std::size_t originDigits = 8U;
  constexpr std::size_t roomDigits = 16U;
  constexpr std::size_t battleDigits = 16U;
  if (name.size() != prefix.size() + originDigits + 1U + roomDigits + 1U +
                         battleDigits + suffix.size() ||
      name.substr(0U, prefix.size()) != prefix ||
      name.substr(name.size() - suffix.size()) != suffix ||
      name[prefix.size() + originDigits] != '-' ||
      name[prefix.size() + originDigits + 1U + roomDigits] != '-') {
    return std::nullopt;
  }
  const auto origin = parseHex(name.substr(prefix.size(), originDigits));
  const auto room =
      parseHex(name.substr(prefix.size() + originDigits + 1U, roomDigits));
  const auto battle = parseHex(name.substr(
      prefix.size() + originDigits + 1U + roomDigits + 1U, battleDigits));
  if (!origin.has_value() || !room.has_value() || !battle.has_value() ||
      *origin > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  const BattleIdentity identity{
      .originRecoveryEpoch = static_cast<std::uint32_t>(*origin),
      .roomId = shared::RoomId{*room},
      .battleInstanceId = shared::BattleInstanceId{*battle}};
  return validIdentity(identity) ? std::optional<BattleIdentity>{identity}
                                 : std::nullopt;
}

struct TemporaryFile final {
  std::filesystem::path path;
  int descriptor{-1};
};

std::optional<TemporaryFile>
createTemporary(const std::filesystem::path &patternPath) noexcept {
  try {
    auto pattern = patternPath.string() + ".XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto descriptor = ::mkstemp(writable.data());
    if (descriptor < 0 || !isRegular(descriptor) ||
        ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
      if (descriptor >= 0) {
        (void)::close(descriptor);
        (void)::unlink(writable.data());
      }
      return std::nullopt;
    }
    return TemporaryFile{.path = std::filesystem::path{writable.data()},
                         .descriptor = descriptor};
  } catch (...) {
    return std::nullopt;
  }
}

bool atomicReplaceImpl(int rootDescriptor, const std::filesystem::path &target,
                       std::span<const std::uint8_t> bytes) {
  const auto temporary =
      createTemporary(std::filesystem::path{target.string() + ".tmp"});
  if (!temporary.has_value()) {
    return false;
  }
  const auto descriptor = temporary->descriptor;
  bool ok = writeAll(descriptor, bytes) && syncData(descriptor);
  ok = (::close(descriptor) == 0) && ok;
  if (ok) {
    ok = ::rename(temporary->path.c_str(), target.c_str()) == 0;
  }
  if (!ok) {
    (void)::unlink(temporary->path.c_str());
    return false;
  }
  return syncDirectory(rootDescriptor);
}

bool atomicReplace(int rootDescriptor, const std::filesystem::path &target,
                   std::span<const std::uint8_t> bytes) noexcept {
  try {
    return atomicReplaceImpl(rootDescriptor, target, bytes);
  } catch (...) {
    return false;
  }
}

using KeyDigest = std::array<std::uint8_t, kKeyDigestBytes>;

std::optional<KeyDigest>
keyDigest(const std::array<std::uint8_t, kKeyBytes> &key) noexcept {
  KeyDigest digest{};
  unsigned int digestSize = 0U;
  if (EVP_Digest(key.data(), key.size(), digest.data(), &digestSize,
                 EVP_sha256(), nullptr) != 1 ||
      digestSize != digest.size()) {
    return std::nullopt;
  }
  return digest;
}

struct ParsedManifest final {
  std::uint32_t epoch{0U};
  KeyDigest keyDigest{};
};

Bytes manifestBytes(std::uint32_t epoch, const KeyDigest &digest) {
  Bytes bytes;
  bytes.reserve(kManifestBytes);
  appendU32(bytes, kManifestMagic);
  appendU16(bytes, kManifestSchemaVersion);
  appendU32(bytes, kManifestPayloadLength);
  appendU32(bytes, epoch);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  appendU32(bytes, battle_continuity::crc32(bytes));
  return bytes;
}

std::optional<ParsedManifest>
parseManifest(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kManifestBytes || readU32(bytes, 0U) != kManifestMagic ||
      readU16(bytes, 4U) != kManifestSchemaVersion ||
      readU32(bytes, 6U) != kManifestPayloadLength ||
      readU32(bytes, 10U) == 0U ||
      readU32(bytes, 46U) != battle_continuity::crc32(bytes.first(46U))) {
    return std::nullopt;
  }
  ParsedManifest result{.epoch = readU32(bytes, 10U)};
  std::copy_n(bytes.begin() + 14U, result.keyDigest.size(),
              result.keyDigest.begin());
  return result;
}

Bytes envelopeAad(const BattleIdentity &identity) {
  Bytes aad;
  aad.reserve(2U + 4U + 8U + 8U);
  appendU16(aad, kEnvelopeSchemaVersion);
  appendU32(aad, identity.originRecoveryEpoch);
  appendU64(aad, identity.roomId.value());
  appendU64(aad, identity.battleInstanceId.value());
  return aad;
}

bool encryptEnvelopeImpl(const std::array<std::uint8_t, 32> &key,
                         const BattleIdentity &identity,
                         std::span<const std::uint8_t> plaintext,
                         Bytes &encoded) {
  if (plaintext.size() > kMaximumEnvelopePlaintextBytes) {
    return false;
  }
  std::array<std::uint8_t, kEnvelopeNonceBytes> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    return false;
  }
  EVP_CIPHER_CTX *rawContext = EVP_CIPHER_CTX_new();
  if (rawContext == nullptr) {
    return false;
  }
  const auto context =
      std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{
          rawContext, &EVP_CIPHER_CTX_free};
  if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(),
                         nonce.data()) != 1) {
    return false;
  }
  const auto aad = envelopeAad(identity);
  int ignoredLength = 0;
  if (EVP_EncryptUpdate(context.get(), nullptr, &ignoredLength, aad.data(),
                        static_cast<int>(aad.size())) != 1) {
    return false;
  }
  Bytes ciphertext(plaintext.size());
  std::uint8_t emptyBuffer = 0U;
  const auto *plaintextData =
      plaintext.empty() ? &emptyBuffer : plaintext.data();
  auto *ciphertextData = ciphertext.empty() ? &emptyBuffer : ciphertext.data();
  int encryptedLength = 0;
  if (EVP_EncryptUpdate(context.get(), ciphertextData, &encryptedLength,
                        plaintextData,
                        static_cast<int>(plaintext.size())) != 1 ||
      EVP_EncryptFinal_ex(context.get(), ciphertextData + encryptedLength,
                          &ignoredLength) != 1) {
    return false;
  }
  std::array<std::uint8_t, kEnvelopeTagBytes> tag{};
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(tag.size()), tag.data()) != 1) {
    return false;
  }

  encoded.clear();
  encoded.reserve(4U + 2U + 4U + 8U + 8U + nonce.size() + 4U +
                  ciphertext.size() + tag.size());
  appendU32(encoded, kEnvelopeMagic);
  appendU16(encoded, kEnvelopeSchemaVersion);
  appendU32(encoded, identity.originRecoveryEpoch);
  appendU64(encoded, identity.roomId.value());
  appendU64(encoded, identity.battleInstanceId.value());
  encoded.insert(encoded.end(), nonce.begin(), nonce.end());
  appendU32(encoded, static_cast<std::uint32_t>(ciphertext.size()));
  encoded.insert(encoded.end(), ciphertext.begin(), ciphertext.end());
  encoded.insert(encoded.end(), tag.begin(), tag.end());
  return true;
}

bool encryptEnvelope(const std::array<std::uint8_t, 32> &key,
                     const BattleIdentity &identity,
                     std::span<const std::uint8_t> plaintext,
                     Bytes &encoded) noexcept {
  try {
    return encryptEnvelopeImpl(key, identity, plaintext, encoded);
  } catch (...) {
    encoded.clear();
    return false;
  }
}

std::optional<Bytes>
decryptEnvelopeImpl(const std::array<std::uint8_t, 32> &key,
                    const BattleIdentity &identity,
                    std::span<const std::uint8_t> bytes) {
  constexpr std::size_t fixedHeader =
      4U + 2U + 4U + 8U + 8U + kEnvelopeNonceBytes + 4U;
  if (bytes.size() < fixedHeader + kEnvelopeTagBytes ||
      readU32(bytes, 0U) != kEnvelopeMagic ||
      readU16(bytes, 4U) != kEnvelopeSchemaVersion ||
      readU32(bytes, 6U) != identity.originRecoveryEpoch ||
      readU64(bytes, 10U) != identity.roomId.value() ||
      readU64(bytes, 18U) != identity.battleInstanceId.value()) {
    return std::nullopt;
  }
  const auto ciphertextLength = static_cast<std::size_t>(readU32(bytes, 38U));
  if (ciphertextLength > kMaximumEnvelopePlaintextBytes ||
      bytes.size() != fixedHeader + ciphertextLength + kEnvelopeTagBytes) {
    return std::nullopt;
  }
  const auto nonce = bytes.subspan(26U, kEnvelopeNonceBytes);
  const auto ciphertext = bytes.subspan(fixedHeader, ciphertextLength);
  const auto tag =
      bytes.subspan(fixedHeader + ciphertextLength, kEnvelopeTagBytes);

  EVP_CIPHER_CTX *rawContext = EVP_CIPHER_CTX_new();
  if (rawContext == nullptr) {
    return std::nullopt;
  }
  const auto context =
      std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>{
          rawContext, &EVP_CIPHER_CTX_free};
  if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(),
                         nonce.data()) != 1) {
    return std::nullopt;
  }
  const auto aad = envelopeAad(identity);
  int ignoredLength = 0;
  if (EVP_DecryptUpdate(context.get(), nullptr, &ignoredLength, aad.data(),
                        static_cast<int>(aad.size())) != 1) {
    return std::nullopt;
  }
  Bytes plaintext(ciphertext.size());
  std::uint8_t emptyBuffer = 0U;
  auto *plaintextData = plaintext.empty() ? &emptyBuffer : plaintext.data();
  const auto *ciphertextData =
      ciphertext.empty() ? &emptyBuffer : ciphertext.data();
  int plaintextLength = 0;
  if (EVP_DecryptUpdate(context.get(), plaintextData, &plaintextLength,
                        ciphertextData,
                        static_cast<int>(ciphertext.size())) != 1) {
    return std::nullopt;
  }
  std::array<std::uint8_t, kEnvelopeTagBytes> tagArray{};
  std::copy(tag.begin(), tag.end(), tagArray.begin());
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tagArray.size()),
                          tagArray.data()) != 1 ||
      EVP_DecryptFinal_ex(context.get(), plaintextData + plaintextLength,
                          &ignoredLength) != 1) {
    return std::nullopt;
  }
  plaintext.resize(static_cast<std::size_t>(plaintextLength + ignoredLength));
  return plaintext;
}

std::optional<Bytes>
decryptEnvelope(const std::array<std::uint8_t, 32> &key,
                const BattleIdentity &identity,
                std::span<const std::uint8_t> bytes) noexcept {
  try {
    return decryptEnvelopeImpl(key, identity, bytes);
  } catch (...) {
    return std::nullopt;
  }
}

bool recordStateHash(const Record &record,
                     battle_continuity::Hash &hash) noexcept {
  return std::visit(
      [&hash](const auto &payload) noexcept {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload,
                                     battle_continuity::BattleStartPayload> ||
                      std::is_same_v<
                          Payload, battle_continuity::CommandDecisionPayload> ||
                      std::is_same_v<Payload,
                                     battle_continuity::CheckpointPayload> ||
                      std::is_same_v<
                          Payload, battle_continuity::TerminalReceiptPayload>) {
          if constexpr (std::is_same_v<Payload,
                                       battle_continuity::BattleStartPayload>) {
            hash = payload.initialStateHash;
          } else if constexpr (std::is_same_v<
                                   Payload,
                                   battle_continuity::CommandDecisionPayload>) {
            hash = payload.postDecisionStateHash;
          } else if constexpr (std::is_same_v<
                                   Payload,
                                   battle_continuity::CheckpointPayload>) {
            hash = payload.stateHash;
          } else {
            hash = payload.finalStateHash;
          }
          return true;
        } else {
          return false;
        }
      },
      record.payload);
}

struct ParsedBatch final {
  std::vector<Record> records;
  BattleIdentity identity{.originRecoveryEpoch = 0U,
                          .roomId = shared::RoomId{0U},
                          .battleInstanceId = shared::BattleInstanceId{0U}};
  std::uint32_t writerRecoveryEpoch{0U};
  std::uint64_t firstSequence{0U};
  std::uint64_t lastSequence{0U};
  std::uint64_t logicalTick{0U};
};

std::optional<ParsedBatch> parseBatch(std::span<const std::uint8_t> bytes,
                                      std::uint32_t expectedWriterEpoch) {
  if (bytes.empty() || bytes.size() > battle_continuity::kMaximumJournalBytes) {
    return std::nullopt;
  }
  ParsedBatch result;
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    if (remaining < 12U) {
      return std::nullopt;
    }
    const auto recordLength =
        static_cast<std::size_t>(readU32(bytes, offset + 8U));
    if (recordLength < battle_continuity::kRecordEnvelopeBytes ||
        recordLength > battle_continuity::kMaximumRecordBytes ||
        recordLength > remaining) {
      return std::nullopt;
    }
    const auto decoded =
        battle_continuity::decodeRecord(bytes.subspan(offset, recordLength));
    if (!decoded.ok() || !decoded.record.has_value()) {
      return std::nullopt;
    }
    result.records.push_back(*decoded.record);
    offset += recordLength;
  }
  if (result.records.empty() ||
      result.records.back().header.recordType != RecordType::TickCommit) {
    return std::nullopt;
  }
  const auto &first = result.records.front().header;
  result.identity =
      BattleIdentity{.originRecoveryEpoch = first.originRecoveryEpoch,
                     .roomId = first.roomId,
                     .battleInstanceId = first.battleInstanceId};
  result.writerRecoveryEpoch = first.writerRecoveryEpoch;
  result.firstSequence = first.recordSequence;
  result.logicalTick = first.logicalTick;
  if (!validIdentity(result.identity) ||
      result.writerRecoveryEpoch != expectedWriterEpoch) {
    return std::nullopt;
  }
  battle_continuity::Hash lastHash{};
  bool hasLastHash = false;
  bool sawTerminal = false;
  for (std::size_t index = 0U; index < result.records.size(); ++index) {
    const auto &record = result.records[index];
    const auto &header = record.header;
    if (index >
            std::numeric_limits<std::uint64_t>::max() - result.firstSequence ||
        header.recordSequence != result.firstSequence + index ||
        header.logicalTick != result.logicalTick ||
        header.writerRecoveryEpoch != result.writerRecoveryEpoch ||
        !sameIdentity(
            BattleIdentity{.originRecoveryEpoch = header.originRecoveryEpoch,
                           .roomId = header.roomId,
                           .battleInstanceId = header.battleInstanceId},
            result.identity)) {
      return std::nullopt;
    }
    if (record.header.recordType == RecordType::TickCommit) {
      if (index + 1U != result.records.size()) {
        return std::nullopt;
      }
      const auto &commit =
          std::get<battle_continuity::TickCommitPayload>(record.payload);
      if (commit.firstRecordSequence != result.firstSequence ||
          header.recordSequence == 0U ||
          commit.lastDataRecordSequence != header.recordSequence - 1U ||
          commit.recordCount != result.records.size() - 1U || !hasLastHash ||
          commit.committedStateHash != lastHash) {
        return std::nullopt;
      }
    } else {
      if (!recordStateHash(record, lastHash)) {
        return std::nullopt;
      }
      hasLastHash = true;
      if (record.header.recordType == RecordType::TerminalReceipt) {
        sawTerminal = true;
      } else if (sawTerminal &&
                 record.header.recordType != RecordType::Checkpoint) {
        return std::nullopt;
      }
    }
  }
  result.lastSequence = result.records.back().header.recordSequence;
  if (sawTerminal &&
      result.records[result.records.size() - 2U].header.recordType !=
          RecordType::Checkpoint) {
    return std::nullopt;
  }
  return result;
}

bool hasValidLaterCommit(std::span<const std::uint8_t> bytes,
                         std::size_t errorOffset) noexcept {
  if (errorOffset >= bytes.size()) {
    return false;
  }
  for (std::size_t offset = errorOffset + 1U;
       offset + battle_continuity::kRecordEnvelopeBytes <= bytes.size();
       ++offset) {
    if (readU32(bytes, offset) != battle_continuity::kRecordMagic) {
      continue;
    }
    const auto recordLength =
        static_cast<std::size_t>(readU32(bytes, offset + 8U));
    if (recordLength < battle_continuity::kRecordEnvelopeBytes ||
        recordLength > battle_continuity::kMaximumRecordBytes ||
        offset + recordLength > bytes.size()) {
      continue;
    }
    const auto decoded =
        battle_continuity::decodeRecord(bytes.subspan(offset, recordLength));
    if (decoded.ok() && decoded.record.has_value() &&
        decoded.record->header.recordType == RecordType::TickCommit) {
      return true;
    }
  }
  return false;
}

bool checksumWasOnCommit(std::span<const std::uint8_t> bytes,
                         std::size_t errorOffset) noexcept {
  std::size_t offset = 0U;
  while (offset + 12U <= bytes.size()) {
    const auto recordLength =
        static_cast<std::size_t>(readU32(bytes, offset + 8U));
    if (recordLength < battle_continuity::kRecordEnvelopeBytes ||
        recordLength > battle_continuity::kMaximumRecordBytes ||
        recordLength > bytes.size() - offset) {
      return false;
    }
    const auto checksumOffset = offset + recordLength - sizeof(std::uint32_t);
    if (checksumOffset == errorOffset) {
      return readU16(bytes, offset + 6U) ==
             static_cast<std::uint16_t>(RecordType::TickCommit);
    }
    if (checksumOffset > errorOffset) {
      return false;
    }
    offset += recordLength;
  }
  return false;
}

struct QuarantineArtifact final {
  std::filesystem::path path;
  bool ok{false};
};

QuarantineArtifact quarantineTailImpl(const std::filesystem::path &journal,
                                      int rootDescriptor,
                                      std::span<const std::uint8_t> tail,
                                      std::size_t committedBytes,
                                      std::size_t originalSize) {
  const auto temporary = createTemporary(
      std::filesystem::path{journal.string() + ".tail-quarantine"});
  if (!temporary.has_value()) {
    return {};
  }
  bool ok =
      writeAll(temporary->descriptor, tail) && syncData(temporary->descriptor);
  ok = (::close(temporary->descriptor) == 0) && ok;
  if (!ok) {
    (void)::unlink(temporary->path.c_str());
    return {};
  }
  const auto descriptor = ::open(journal.c_str(), noFollowFlags(O_RDWR));
  struct stat status {};
  bool sourceOk = descriptor >= 0 && ::fstat(descriptor, &status) == 0 &&
                  isOwnerOnlyRegular(descriptor) && status.st_size >= 0 &&
                  static_cast<std::size_t>(status.st_size) == originalSize;
  if (sourceOk) {
    sourceOk = ::ftruncate(descriptor, static_cast<off_t>(committedBytes)) == 0;
  }
  if (sourceOk) {
    sourceOk = syncData(descriptor);
  }
  bool closeOk = true;
  if (descriptor >= 0) {
    closeOk = ::close(descriptor) == 0;
  }
  if (!sourceOk || !closeOk || !syncDirectory(rootDescriptor)) {
    (void)::unlink(temporary->path.c_str());
    return {};
  }
  return QuarantineArtifact{.path = temporary->path, .ok = true};
}

QuarantineArtifact quarantineTail(const std::filesystem::path &journal,
                                  int rootDescriptor,
                                  std::span<const std::uint8_t> tail,
                                  std::size_t committedBytes,
                                  std::size_t originalSize) noexcept {
  try {
    return quarantineTailImpl(journal, rootDescriptor, tail, committedBytes,
                              originalSize);
  } catch (...) {
    return {};
  }
}

QuarantineArtifact quarantineJournal(const std::filesystem::path &journal,
                                     int rootDescriptor) noexcept {
  try {
    const auto temporary = createTemporary(
        std::filesystem::path{journal.string() + ".identity-quarantine"});
    if (!temporary.has_value() || ::close(temporary->descriptor) != 0) {
      if (temporary.has_value()) {
        (void)::unlink(temporary->path.c_str());
      }
      return {};
    }
    if (::rename(journal.c_str(), temporary->path.c_str()) != 0) {
      (void)::unlink(temporary->path.c_str());
      return {};
    }
    const bool synced = syncDirectory(rootDescriptor);
    return QuarantineArtifact{.path = temporary->path, .ok = synced};
  } catch (...) {
    return {};
  }
}

StorageError sidecarErrorFor(bool io) noexcept {
  return io ? StorageError::SidecarIo : StorageError::SidecarCorrupt;
}

} // namespace

struct ContinuityStorage::Impl final {
  struct Job final {
    battle_continuity::DurableTickWriteRequest request;
    battle_continuity::DurableTickWritePort::CompletionSink completion;
    std::size_t queuedBytes{0U};
  };

  Impl(std::filesystem::path root, int rootDescriptor, int lockDescriptor,
       std::array<std::uint8_t, 32> key, std::uint32_t epoch,
       std::size_t queueCapacity)
      : root_(std::move(root)), rootDescriptor_(rootDescriptor),
        lockDescriptor_(lockDescriptor), key_(key), epoch_(epoch),
        queueCapacity_(queueCapacity), worker_([this] { run(); }) {}

  ~Impl() { stop(); }

  void stop() noexcept {
    std::lock_guard lifecycleLock{lifecycleMutex_};
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable() && std::this_thread::get_id() != worker_.get_id()) {
      worker_.join();
    }
    if (lockDescriptor_ >= 0) {
      (void)::flock(lockDescriptor_, LOCK_UN);
      (void)::close(lockDescriptor_);
      lockDescriptor_ = -1;
    }
    if (rootDescriptor_ >= 0) {
      (void)::close(rootDescriptor_);
      rootDescriptor_ = -1;
    }
  }

  void waitUntilIdle() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return queue_.empty() && !active_; });
  }

  StorageError quarantineBattle(const BattleIdentity &identity,
                                std::uint32_t expectedWriterEpoch) noexcept {
    try {
      if (!validIdentity(identity)) {
        return StorageError::InvalidRequest;
      }
      std::unique_lock lifecycleLock{lifecycleMutex_};
      {
        std::lock_guard lock{mutex_};
        if (stopping_ || rootDescriptor_ < 0 || lockDescriptor_ < 0) {
          return StorageError::StorageStopped;
        }
      }
      if (expectedWriterEpoch != epoch_) {
        return StorageError::StaleWriterEpoch;
      }
      waitUntilIdle();
      {
        std::lock_guard lock{mutex_};
        if (stopping_ || rootDescriptor_ < 0 || lockDescriptor_ < 0) {
          return StorageError::StorageStopped;
        }
      }

      const auto retiredStatus =
          tombstoneStatus(root_, identity, TombstoneKind::Retired);
      const auto quarantineStatus =
          tombstoneStatus(root_, identity, TombstoneKind::Quarantined);
      if (retiredStatus == TombstoneStatus::Corrupt ||
          quarantineStatus == TombstoneStatus::Corrupt) {
        return StorageError::JournalCorrupt;
      }
      if (retiredStatus == TombstoneStatus::Valid ||
          quarantineStatus == TombstoneStatus::Valid) {
        return StorageError::None;
      }

      const auto journal = journalPathFor(root_, identity);
      struct stat journalStatus {};
      if (::lstat(journal.c_str(), &journalStatus) != 0) {
        return StorageError::JournalIo;
      }
      try {
        const auto marker = tombstoneBytes(TombstoneKind::Quarantined, identity,
                                           expectedWriterEpoch);
        return atomicReplace(rootDescriptor_,
                             tombstonePathFor(root_, identity,
                                              TombstoneKind::Quarantined),
                             marker)
                   ? StorageError::None
                   : StorageError::JournalIo;
      } catch (...) {
        return StorageError::JournalIo;
      }
    } catch (...) {
      return StorageError::JournalIo;
    }
  }

  StorageError retireBattle(const BattleIdentity &identity,
                            std::uint32_t expectedWriterEpoch) noexcept {
    try {
      if (!validIdentity(identity)) {
        return StorageError::InvalidRequest;
      }
      std::unique_lock lifecycleLock{lifecycleMutex_};
      {
        std::lock_guard lock{mutex_};
        if (stopping_ || rootDescriptor_ < 0 || lockDescriptor_ < 0) {
          return StorageError::StorageStopped;
        }
      }
      if (expectedWriterEpoch != epoch_) {
        return StorageError::StaleWriterEpoch;
      }
      waitUntilIdle();
      {
        std::lock_guard lock{mutex_};
        if (stopping_ || rootDescriptor_ < 0 || lockDescriptor_ < 0) {
          return StorageError::StorageStopped;
        }
      }

      const auto retiredStatus =
          tombstoneStatus(root_, identity, TombstoneKind::Retired);
      const auto quarantineStatus =
          tombstoneStatus(root_, identity, TombstoneKind::Quarantined);
      if (retiredStatus == TombstoneStatus::Corrupt ||
          quarantineStatus == TombstoneStatus::Corrupt) {
        return StorageError::JournalCorrupt;
      }
      if (retiredStatus == TombstoneStatus::Valid) {
        return StorageError::None;
      }
      if (quarantineStatus == TombstoneStatus::Valid) {
        return StorageError::JournalCorrupt;
      }

      const auto journal = journalPathFor(root_, identity);
      struct stat journalStatus {};
      if (::lstat(journal.c_str(), &journalStatus) != 0) {
        return StorageError::JournalIo;
      }
      const auto marker =
          tombstoneBytes(TombstoneKind::Retired, identity, expectedWriterEpoch);
      return atomicReplace(
                 rootDescriptor_,
                 tombstonePathFor(root_, identity, TombstoneKind::Retired),
                 marker)
                 ? StorageError::None
                 : StorageError::JournalIo;
    } catch (...) {
      return StorageError::JournalIo;
    }
  }

  battle_continuity::DurableTickWriteOutcome
  process(const battle_continuity::DurableTickWriteRequest &request) noexcept {
    const auto &batch = request.batch;
    try {
      if (batch.writerRecoveryEpoch != epoch_) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure =
                battle_continuity::DurableTickWriteFailure::StaleWriterEpoch};
      }
      if (!validIdentity(batch.identity) || batch.encodedRecords.empty() ||
          batch.encodedRecords.size() >
              battle_continuity::kMaximumJournalBytes ||
          (request.privateEnvelopePlaintext.has_value() &&
           request.privateEnvelopePlaintext->size() >
               kMaximumEnvelopePlaintextBytes)) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure =
                battle_continuity::DurableTickWriteFailure::InvalidBatch};
      }

      const auto parsed = parseBatch(batch.encodedRecords, epoch_);
      if (!parsed.has_value() ||
          !sameIdentity(parsed->identity, batch.identity) ||
          parsed->firstSequence != batch.firstRecordSequence ||
          parsed->lastSequence != batch.lastRecordSequence ||
          parsed->logicalTick != batch.logicalTick) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure =
                battle_continuity::DurableTickWriteFailure::InvalidBatch};
      }
      const bool initialBatch =
          parsed->firstSequence == 1U && !parsed->records.empty() &&
          parsed->records.front().header.recordType == RecordType::BattleStart;
      if (request.privateEnvelopePlaintext.has_value() && !initialBatch) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure =
                battle_continuity::DurableTickWriteFailure::InvalidBatch};
      }
      const auto journal = journalPathFor(root_, batch.identity);
      const auto existing =
          readRegularFile(journal, battle_continuity::kMaximumJournalBytes);
      if (!existing.missing && !existing.bytes.has_value()) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
      }
      if (request.privateEnvelopePlaintext.has_value() && !existing.missing) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure =
                battle_continuity::DurableTickWriteFailure::InvalidBatch};
      }
      Bytes combined;
      if (existing.missing) {
        const auto initial =
            battle_continuity::decodeJournal(batch.encodedRecords);
        if (!initial.ok() ||
            initial.committedBytes != batch.encodedRecords.size()) {
          return battle_continuity::DurableTickWriteFailed{
              .identity = batch.identity,
              .writerRecoveryEpoch = batch.writerRecoveryEpoch,
              .lastRecordSequence = batch.lastRecordSequence,
              .failure =
                  battle_continuity::DurableTickWriteFailure::InvalidBatch};
        }
      } else {
        const auto existingDecoded =
            battle_continuity::decodeJournal(*existing.bytes);
        if (!existingDecoded.ok() ||
            existingDecoded.committedBytes != existing.bytes->size() ||
            existingDecoded.records.empty() ||
            existingDecoded.records.back().header.recordType !=
                RecordType::TickCommit ||
            parsed->firstSequence <=
                existingDecoded.records.back().header.recordSequence) {
          return battle_continuity::DurableTickWriteFailed{
              .identity = batch.identity,
              .writerRecoveryEpoch = batch.writerRecoveryEpoch,
              .lastRecordSequence = batch.lastRecordSequence,
              .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
        }
        const auto expectedFirst =
            existingDecoded.records.back().header.recordSequence + 1U;
        if (parsed->firstSequence != expectedFirst ||
            existing.bytes->size() > battle_continuity::kMaximumJournalBytes -
                                         batch.encodedRecords.size()) {
          return battle_continuity::DurableTickWriteFailed{
              .identity = batch.identity,
              .writerRecoveryEpoch = batch.writerRecoveryEpoch,
              .lastRecordSequence = batch.lastRecordSequence,
              .failure =
                  battle_continuity::DurableTickWriteFailure::InvalidBatch};
        }
        combined = *existing.bytes;
        combined.insert(combined.end(), batch.encodedRecords.begin(),
                        batch.encodedRecords.end());
        const auto decoded = battle_continuity::decodeJournal(combined);
        if (!decoded.ok() || decoded.committedBytes != combined.size()) {
          return battle_continuity::DurableTickWriteFailed{
              .identity = batch.identity,
              .writerRecoveryEpoch = batch.writerRecoveryEpoch,
              .lastRecordSequence = batch.lastRecordSequence,
              .failure =
                  battle_continuity::DurableTickWriteFailure::InvalidBatch};
        }
      }

      if (request.privateEnvelopePlaintext.has_value()) {
        const auto envelope = envelopePathFor(root_, batch.identity);
        Bytes encodedEnvelope;
        if (!encryptEnvelope(key_, batch.identity,
                             *request.privateEnvelopePlaintext,
                             encodedEnvelope) ||
            !atomicReplace(rootDescriptor_, envelope, encodedEnvelope)) {
          return battle_continuity::DurableTickWriteFailed{
              .identity = batch.identity,
              .writerRecoveryEpoch = batch.writerRecoveryEpoch,
              .lastRecordSequence = batch.lastRecordSequence,
              .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
        }
      }

      const auto descriptor =
          ::open(journal.c_str(), noFollowFlags(O_WRONLY | O_APPEND | O_CREAT),
                 S_IRUSR | S_IWUSR);
      if (descriptor < 0 || !isRegular(descriptor)) {
        if (descriptor >= 0) {
          (void)::close(descriptor);
        }
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
      }
      struct stat status {};
      const bool statOk = ::fstat(descriptor, &status) == 0;
      const auto expectedSize = existing.missing ? 0U : existing.bytes->size();
      const bool appendOk =
          statOk && static_cast<std::size_t>(status.st_size) == expectedSize &&
          status.st_uid == ::geteuid() &&
          (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
          writeAll(descriptor, batch.encodedRecords) && syncData(descriptor);
      const bool closeOk = ::close(descriptor) == 0;
      if (!appendOk || !closeOk ||
          (existing.missing && !syncDirectory(rootDescriptor_))) {
        return battle_continuity::DurableTickWriteFailed{
            .identity = batch.identity,
            .writerRecoveryEpoch = batch.writerRecoveryEpoch,
            .lastRecordSequence = batch.lastRecordSequence,
            .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
      }
      return battle_continuity::DurableTickCommitted{
          .identity = batch.identity,
          .writerRecoveryEpoch = batch.writerRecoveryEpoch,
          .lastRecordSequence = parsed->lastSequence};
    } catch (...) {
      return battle_continuity::DurableTickWriteFailed{
          .identity = batch.identity,
          .writerRecoveryEpoch = batch.writerRecoveryEpoch,
          .lastRecordSequence = batch.lastRecordSequence,
          .failure = battle_continuity::DurableTickWriteFailure::IoFailure};
    }
  }

  std::optional<Bytes> decryptSidecar(const BattleIdentity &identity,
                                      StorageError &error) noexcept {
    const auto path = envelopePathFor(root_, identity);
    const auto file = readRegularFile(path, fixedEnvelopeMaximum());
    if (file.missing) {
      error = StorageError::SidecarCorrupt;
      return std::nullopt;
    }
    if (!file.bytes.has_value()) {
      error = sidecarErrorFor(file.error != StorageError::JournalCorrupt);
      return std::nullopt;
    }
    const auto plaintext = decryptEnvelope(key_, identity, *file.bytes);
    if (!plaintext.has_value()) {
      error = StorageError::SidecarCorrupt;
      return std::nullopt;
    }
    return plaintext;
  }

  static constexpr std::size_t fixedEnvelopeMaximum() noexcept {
    return 4U + 2U + 4U + 8U + 8U + kEnvelopeNonceBytes + 4U +
           kMaximumEnvelopePlaintextBytes + kEnvelopeTagBytes;
  }

  bool skipTombstoned(const BattleIdentity &identity,
                      const std::filesystem::path &journal,
                      ScanResult &result) const {
    const auto retiredStatus =
        tombstoneStatus(root_, identity, TombstoneKind::Retired);
    const auto quarantineStatus =
        tombstoneStatus(root_, identity, TombstoneKind::Quarantined);
    if (retiredStatus == TombstoneStatus::Corrupt ||
        quarantineStatus == TombstoneStatus::Corrupt) {
      result.quarantined.push_back(
          QuarantinedBattle{.artifact = journal,
                            .identity = identity,
                            .reason = StorageError::JournalCorrupt});
      return true;
    }
    return retiredStatus == TombstoneStatus::Valid ||
           quarantineStatus == TombstoneStatus::Valid;
  }

  bool
  persistQuarantineTombstone(const BattleIdentity &identity) const noexcept {
    if (tombstoneStatus(root_, identity, TombstoneKind::Quarantined) !=
        TombstoneStatus::Absent) {
      return false;
    }
    try {
      const auto marker =
          tombstoneBytes(TombstoneKind::Quarantined, identity, epoch_);
      return atomicReplace(
          rootDescriptor_,
          tombstonePathFor(root_, identity, TombstoneKind::Quarantined),
          marker);
    } catch (...) {
      return false;
    }
  }

  ScanResult scan() {
    waitUntilIdle();
    ScanResult result;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator{root_, iteratorError};
    if (iteratorError) {
      result.error = StorageError::DirectoryIo;
      return result;
    }
    for (const auto &entry : iterator) {
      const auto name = entry.path().filename().string();
      if (name.size() < 7U || !name.ends_with(".journal")) {
        continue;
      }
      const auto filenameIdentity = identityFromJournalFilename(name);
      if (!filenameIdentity.has_value()) {
        const auto artifact = quarantineJournal(entry.path(), rootDescriptor_);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = artifact.path.empty() ? entry.path() : artifact.path,
            .identity = std::nullopt,
            .reason = artifact.ok ? StorageError::JournalCorrupt
                                   : StorageError::JournalIo});
        continue;
      }
      const auto status =
          std::filesystem::symlink_status(entry.path(), iteratorError);
      if (iteratorError || std::filesystem::is_symlink(status)) {
        if (skipTombstoned(*filenameIdentity, entry.path(), result)) {
          continue;
        }
        const auto marked = persistQuarantineTombstone(*filenameIdentity);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = marked ? tombstonePathFor(root_, *filenameIdentity,
                                                  TombstoneKind::Quarantined)
                               : entry.path(),
            .identity = filenameIdentity,
            .reason = StorageError::JournalCorrupt});
        iteratorError.clear();
        continue;
      }
      const auto file = readRegularFile(
          entry.path(), battle_continuity::kMaximumJournalBytes);
      if (file.missing || !file.bytes.has_value()) {
        if (skipTombstoned(*filenameIdentity, entry.path(), result)) {
          continue;
        }
        const auto marked = persistQuarantineTombstone(*filenameIdentity);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = marked ? tombstonePathFor(root_, *filenameIdentity,
                                                  TombstoneKind::Quarantined)
                               : entry.path(),
            .identity = filenameIdentity,
            .reason = file.error == StorageError::None
                          ? StorageError::JournalCorrupt
                          : file.error});
        continue;
      }
      const auto decoded = battle_continuity::decodeJournal(*file.bytes);
      if (decoded.ok() && decoded.committedBytes == file.bytes->size() &&
          !decoded.records.empty()) {
        const auto &first = decoded.records.front().header;
        const BattleIdentity identity{
            .originRecoveryEpoch = first.originRecoveryEpoch,
            .roomId = first.roomId,
            .battleInstanceId = first.battleInstanceId};
        if (identity != *filenameIdentity) {
          const auto artifact = quarantineJournal(entry.path(), rootDescriptor_);
          result.quarantined.push_back(QuarantinedBattle{
              .artifact = artifact.path.empty() ? entry.path() : artifact.path,
              .identity = filenameIdentity,
              .reason = artifact.ok ? StorageError::JournalCorrupt
                                     : StorageError::JournalIo});
          continue;
        }
        if (skipTombstoned(*filenameIdentity, entry.path(), result)) {
          continue;
        }
        StorageError sidecarError = StorageError::None;
        const auto privateEnvelope = decryptSidecar(identity, sidecarError);
        if (sidecarError != StorageError::None) {
          const auto marked = persistQuarantineTombstone(identity);
          result.quarantined.push_back(QuarantinedBattle{
              .artifact = marked ? tombstonePathFor(root_, identity,
                                                    TombstoneKind::Quarantined)
                                 : entry.path(),
              .identity = identity,
              .reason = sidecarError});
        } else {
          result.healthy.push_back(
              RecoveredBattle{.identity = identity,
                              .committedJournal = *file.bytes,
                              .privateEnvelope = privateEnvelope});
        }
        continue;
      }

      const auto identity =
          decoded.records.empty()
              ? std::optional<BattleIdentity>{}
              : std::optional<BattleIdentity>{BattleIdentity{
                    .originRecoveryEpoch =
                        decoded.records.front().header.originRecoveryEpoch,
                    .roomId = decoded.records.front().header.roomId,
                    .battleInstanceId =
                        decoded.records.front().header.battleInstanceId}};
      if (identity.has_value() && *identity != *filenameIdentity) {
        const auto artifact = quarantineJournal(entry.path(), rootDescriptor_);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = artifact.path.empty() ? entry.path() : artifact.path,
            .identity = filenameIdentity,
            .reason = artifact.ok ? StorageError::JournalCorrupt
                                   : StorageError::JournalIo});
        continue;
      }
      if (skipTombstoned(*filenameIdentity, entry.path(), result)) {
        continue;
      }
      const bool committedCorruption =
          decoded.committedBytes == 0U ||
          hasValidLaterCommit(*file.bytes, decoded.error.has_value()
                                               ? decoded.error->offset
                                               : decoded.committedBytes) ||
          (decoded.error.has_value() &&
           decoded.error->code ==
               battle_continuity::CodecErrorCode::ChecksumMismatch &&
           checksumWasOnCommit(*file.bytes, decoded.error->offset));
      if (committedCorruption) {
        const auto marked =
            identity.has_value() && persistQuarantineTombstone(*identity);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = marked ? tombstonePathFor(root_, *identity,
                                                  TombstoneKind::Quarantined)
                               : entry.path(),
            .identity = identity,
            .reason = StorageError::JournalCorrupt});
        continue;
      }
      const auto artifact =
          quarantineTail(entry.path(), rootDescriptor_,
                         std::span<const std::uint8_t>{*file.bytes}.subspan(
                             decoded.committedBytes),
                         decoded.committedBytes, file.bytes->size());
      if (!artifact.ok) {
        const auto marked =
            identity.has_value() && persistQuarantineTombstone(*identity);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = marked ? tombstonePathFor(root_, *identity,
                                                  TombstoneKind::Quarantined)
                               : entry.path(),
            .identity = identity,
            .reason = StorageError::JournalIo});
        continue;
      }
      result.repairedTails.push_back(
          RepairedTail{.artifact = artifact.path, .identity = identity});
      const auto repaired = readRegularFile(
          entry.path(), battle_continuity::kMaximumJournalBytes);
      if (!repaired.bytes.has_value()) {
        result.quarantined.push_back(
            QuarantinedBattle{.artifact = artifact.path,
                              .identity = identity,
                              .reason = StorageError::JournalIo});
        continue;
      }
      const auto repairedDecoded =
          battle_continuity::decodeJournal(*repaired.bytes);
      if (!repairedDecoded.ok() ||
          repairedDecoded.committedBytes != repaired.bytes->size() ||
          repairedDecoded.records.empty()) {
        result.quarantined.push_back(
            QuarantinedBattle{.artifact = artifact.path,
                              .identity = identity,
                              .reason = StorageError::JournalCorrupt});
        continue;
      }
      const auto &first = repairedDecoded.records.front().header;
      const BattleIdentity repairedIdentity{
          .originRecoveryEpoch = first.originRecoveryEpoch,
          .roomId = first.roomId,
          .battleInstanceId = first.battleInstanceId};
      if (repairedIdentity != *filenameIdentity) {
        const auto renamed = quarantineJournal(entry.path(), rootDescriptor_);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = renamed.path.empty() ? entry.path() : renamed.path,
            .identity = filenameIdentity,
            .reason = renamed.ok ? StorageError::JournalCorrupt
                                 : StorageError::JournalIo});
        continue;
      }
      StorageError sidecarError = StorageError::None;
      const auto privateEnvelope =
          decryptSidecar(repairedIdentity, sidecarError);
      if (sidecarError != StorageError::None) {
        const auto marked = persistQuarantineTombstone(repairedIdentity);
        result.quarantined.push_back(QuarantinedBattle{
            .artifact = marked ? tombstonePathFor(root_, repairedIdentity,
                                                  TombstoneKind::Quarantined)
                               : entry.path(),
            .identity = repairedIdentity,
            .reason = sidecarError});
        continue;
      }
      result.healthy.push_back(
          RecoveredBattle{.identity = repairedIdentity,
                          .committedJournal = *repaired.bytes,
                          .privateEnvelope = privateEnvelope});
      iteratorError.clear();
    }
    if (iteratorError) {
      result.error = StorageError::DirectoryIo;
    }
    return result;
  }

  void run() noexcept {
    while (true) {
      std::optional<Job> job;
      {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) {
          return;
        }
        job = std::move(queue_.front());
        queue_.pop_front();
        queuedBytes_ -= job->queuedBytes;
        active_ = true;
      }
      const auto completion = process(job->request);
      try {
        job->completion(completion);
      } catch (...) {
        // A caller callback is outside storage ownership; it must not kill the
        // sole writer and leave accepted requests undrained.
      }
      {
        std::lock_guard lock{mutex_};
        active_ = false;
        if (queue_.empty()) {
          idle_.notify_all();
        }
      }
    }
  }

  std::filesystem::path root_;
  int rootDescriptor_;
  int lockDescriptor_;
  std::array<std::uint8_t, 32> key_;
  std::uint32_t epoch_;
  std::size_t queueCapacity_;
  std::mutex lifecycleMutex_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::condition_variable idle_;
  std::deque<Job> queue_;
  std::size_t queuedBytes_{0U};
  bool active_{false};
  bool stopping_{false};
  std::thread worker_;
};

ContinuityStorage::ContinuityStorage(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

OpenResult ContinuityStorage::open(const std::filesystem::path &root,
                                   const std::filesystem::path &keyFile,
                                   std::size_t queueCapacity) {
  if (queueCapacity == 0U) {
    return OpenResult{.storage = nullptr,
                      .error = StorageError::InvalidRequest};
  }
  struct stat rootStatus {};
  if (::lstat(root.c_str(), &rootStatus) != 0) {
    return OpenResult{.storage = nullptr, .error = StorageError::InvalidRoot};
  }
  if (S_ISLNK(rootStatus.st_mode)) {
    return OpenResult{.storage = nullptr, .error = StorageError::RootSymlink};
  }
  if (!S_ISDIR(rootStatus.st_mode)) {
    return OpenResult{.storage = nullptr,
                      .error = StorageError::RootNotDirectory};
  }
  if (rootStatus.st_uid != ::geteuid() ||
      (rootStatus.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return OpenResult{.storage = nullptr, .error = StorageError::RootOwnership};
  }
  const auto rootDescriptor =
      ::open(root.c_str(), noFollowFlags(O_RDONLY | O_DIRECTORY));
  if (rootDescriptor < 0) {
    return OpenResult{.storage = nullptr, .error = StorageError::InvalidRoot};
  }
  struct stat openedRootStatus {};
  if (::fstat(rootDescriptor, &openedRootStatus) != 0 ||
      !S_ISDIR(openedRootStatus.st_mode) ||
      openedRootStatus.st_dev != rootStatus.st_dev ||
      openedRootStatus.st_ino != rootStatus.st_ino) {
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr, .error = StorageError::InvalidRoot};
  }

  const auto lockPath = root / ".battle-continuity.lock";
  const auto lockDescriptor = ::open(
      lockPath.c_str(), noFollowFlags(O_RDWR | O_CREAT), S_IRUSR | S_IWUSR);
  const bool lockOpen =
      lockDescriptor >= 0 && isOwnerOnlyRegular(lockDescriptor);
  const int lockError =
      lockOpen ? ::flock(lockDescriptor, LOCK_EX | LOCK_NB) : -1;
  const int lockErrno = lockOpen ? (lockError == 0 ? 0 : errno) : errno;
  if (!lockOpen || lockError != 0) {
    if (lockDescriptor >= 0) {
      (void)::close(lockDescriptor);
    }
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = lockErrno == EWOULDBLOCK || lockErrno == EAGAIN
                                   ? StorageError::LockUnavailable
                                   : StorageError::LockIo};
  }

  struct stat keyStatus {};
  if (::lstat(keyFile.c_str(), &keyStatus) != 0 || S_ISLNK(keyStatus.st_mode)) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = S_ISLNK(keyStatus.st_mode)
                                   ? StorageError::KeySymlink
                                   : StorageError::InvalidKey};
  }
  if (!S_ISREG(keyStatus.st_mode) || keyStatus.st_uid != ::geteuid()) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = keyStatus.st_uid != ::geteuid()
                                   ? StorageError::KeyOwnership
                                   : StorageError::InvalidKey};
  }
  if ((keyStatus.st_mode & 07777) != (S_IRUSR | S_IWUSR)) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = StorageError::KeyPermissions};
  }
  if (keyStatus.st_size != 32) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr, .error = StorageError::KeyLength};
  }
  const auto keyDescriptor = ::open(keyFile.c_str(), noFollowFlags(O_RDONLY));
  std::array<std::uint8_t, kKeyBytes> key{};
  struct stat openedKeyStatus {};
  const bool keyOk =
      keyDescriptor >= 0 && ::fstat(keyDescriptor, &openedKeyStatus) == 0 &&
      S_ISREG(openedKeyStatus.st_mode) &&
      openedKeyStatus.st_uid == ::geteuid() &&
      (openedKeyStatus.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
      openedKeyStatus.st_size == 32 && readAt(keyDescriptor, key);
  const bool keyClosed = keyDescriptor < 0 || ::close(keyDescriptor) == 0;
  if (!keyOk || !keyClosed) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr, .error = StorageError::InvalidKey};
  }
  const auto digest = keyDigest(key);
  if (!digest.has_value()) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = StorageError::CryptoUnavailable};
  }

  const auto manifestPath = root / ".battle-continuity.manifest";
  const auto manifest = readRegularFile(manifestPath, kManifestBytes);
  std::uint32_t previousEpoch = 0U;
  if (!manifest.missing) {
    if (!manifest.bytes.has_value()) {
      (void)::flock(lockDescriptor, LOCK_UN);
      (void)::close(lockDescriptor);
      (void)::close(rootDescriptor);
      return OpenResult{.storage = nullptr,
                        .error = StorageError::ManifestCorrupt};
    }
    const auto parsed = parseManifest(*manifest.bytes);
    if (!parsed.has_value()) {
      (void)::flock(lockDescriptor, LOCK_UN);
      (void)::close(lockDescriptor);
      (void)::close(rootDescriptor);
      return OpenResult{.storage = nullptr,
                        .error = StorageError::ManifestCorrupt};
    }
    if (CRYPTO_memcmp(parsed->keyDigest.data(), digest->data(),
                      digest->size()) != 0) {
      (void)::flock(lockDescriptor, LOCK_UN);
      (void)::close(lockDescriptor);
      (void)::close(rootDescriptor);
      return OpenResult{.storage = nullptr, .error = StorageError::InvalidKey};
    }
    previousEpoch = parsed->epoch;
  }
  if (previousEpoch == std::numeric_limits<std::uint32_t>::max()) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr,
                      .error = StorageError::EpochExhausted};
  }
  const auto epoch = previousEpoch + 1U;
  Bytes encodedManifest;
  try {
    encodedManifest = manifestBytes(epoch, *digest);
  } catch (...) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr, .error = StorageError::ManifestIo};
  }
  if (!atomicReplace(rootDescriptor, manifestPath, encodedManifest)) {
    (void)::flock(lockDescriptor, LOCK_UN);
    (void)::close(lockDescriptor);
    (void)::close(rootDescriptor);
    return OpenResult{.storage = nullptr, .error = StorageError::ManifestIo};
  }

  std::unique_ptr<Impl> implementation;
  try {
    implementation = std::make_unique<Impl>(
        root, rootDescriptor, lockDescriptor, key, epoch, queueCapacity);
    auto storage = std::unique_ptr<ContinuityStorage>{
        new ContinuityStorage(std::move(implementation))};
    return OpenResult{.storage = std::move(storage),
                      .error = StorageError::None};
  } catch (...) {
    // If Impl owns the descriptors, its destructor releases them.  This also
    // avoids closing the same descriptor twice when the wrapper allocation
    // itself fails.
    if (implementation == nullptr) {
      (void)::flock(lockDescriptor, LOCK_UN);
      (void)::close(lockDescriptor);
      (void)::close(rootDescriptor);
    }
    return OpenResult{.storage = nullptr, .error = StorageError::ManifestIo};
  }
}

ContinuityStorage::~ContinuityStorage() = default;

std::uint32_t ContinuityStorage::writerRecoveryEpoch() const noexcept {
  return implementation_ == nullptr ? 0U : implementation_->epoch_;
}

battle_continuity::DurableTickSubmitResult ContinuityStorage::submit(
    battle_continuity::DurableTickWriteRequest request,
    battle_continuity::DurableTickWritePort::CompletionSink completion) {
  if (implementation_ == nullptr || !completion ||
      !validIdentity(request.batch.identity) ||
      request.batch.encodedRecords.empty() ||
      request.batch.encodedRecords.size() >
          battle_continuity::kMaximumJournalBytes ||
      (request.privateEnvelopePlaintext.has_value() &&
       request.privateEnvelopePlaintext->size() >
           kMaximumEnvelopePlaintextBytes)) {
    return battle_continuity::DurableTickSubmitResult::InvalidRequest;
  }
  if (request.batch.writerRecoveryEpoch != implementation_->epoch_) {
    battle_continuity::DurableTickWriteFailed stale{
        .identity = request.batch.identity,
        .writerRecoveryEpoch = request.batch.writerRecoveryEpoch,
        .lastRecordSequence = request.batch.lastRecordSequence,
        .failure =
            battle_continuity::DurableTickWriteFailure::StaleWriterEpoch};
    try {
      completion(battle_continuity::DurableTickWriteOutcome{stale});
    } catch (...) {
    }
    return battle_continuity::DurableTickSubmitResult::StaleWriterEpoch;
  }
  const auto queuedBytes = request.batch.encodedRecords.size() +
                           (request.privateEnvelopePlaintext.has_value()
                                ? request.privateEnvelopePlaintext->size()
                                : 0U);
  {
    std::lock_guard lock{implementation_->mutex_};
    if (implementation_->stopping_) {
      return battle_continuity::DurableTickSubmitResult::Stopped;
    }
    if (implementation_->queue_.size() >= implementation_->queueCapacity_ ||
        queuedBytes > kMaximumQueuedBytes ||
        implementation_->queuedBytes_ > kMaximumQueuedBytes - queuedBytes) {
      return battle_continuity::DurableTickSubmitResult::QueueFull;
    }
    try {
      implementation_->queue_.push_back(
          Impl::Job{.request = std::move(request),
                    .completion = std::move(completion),
                    .queuedBytes = queuedBytes});
    } catch (...) {
      return battle_continuity::DurableTickSubmitResult::QueueFull;
    }
    implementation_->queuedBytes_ += queuedBytes;
  }
  implementation_->changed_.notify_one();
  return battle_continuity::DurableTickSubmitResult::Accepted;
}

void ContinuityStorage::waitUntilIdle() {
  if (implementation_ != nullptr) {
    implementation_->waitUntilIdle();
  }
}

void ContinuityStorage::stop() noexcept {
  if (implementation_ != nullptr) {
    implementation_->stop();
  }
}

StorageError ContinuityStorage::quarantineBattle(
    const BattleIdentity &identity,
    std::uint32_t expectedWriterRecoveryEpoch) noexcept {
  if (implementation_ == nullptr) {
    return StorageError::StorageStopped;
  }
  return implementation_->quarantineBattle(identity,
                                           expectedWriterRecoveryEpoch);
}

StorageError ContinuityStorage::retireBattle(
    const BattleIdentity &identity,
    std::uint32_t expectedWriterRecoveryEpoch) noexcept {
  if (implementation_ == nullptr) {
    return StorageError::StorageStopped;
  }
  return implementation_->retireBattle(identity, expectedWriterRecoveryEpoch);
}

ScanResult ContinuityStorage::scan() {
  if (implementation_ == nullptr) {
    return ScanResult{.healthy = {},
                      .quarantined = {},
                      .repairedTails = {},
                      .error = StorageError::StorageStopped};
  }
  try {
    return implementation_->scan();
  } catch (...) {
    return ScanResult{.healthy = {},
                      .quarantined = {},
                      .repairedTails = {},
                      .error = StorageError::DirectoryIo};
  }
}

std::filesystem::path
ContinuityStorage::journalPath(const BattleIdentity &identity) const {
  return implementation_ == nullptr
             ? std::filesystem::path{}
             : journalPathFor(implementation_->root_, identity);
}

std::filesystem::path
ContinuityStorage::privateEnvelopePath(const BattleIdentity &identity) const {
  return implementation_ == nullptr
             ? std::filesystem::path{}
             : envelopePathFor(implementation_->root_, identity);
}

} // namespace lol::battle_continuity_storage
