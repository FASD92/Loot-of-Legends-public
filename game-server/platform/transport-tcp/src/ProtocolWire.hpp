#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lol::transport::tcp::wire {

class Writer final {
public:
  void uint8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void uint16(std::uint16_t value) {
    uint8(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    uint8(static_cast<std::uint8_t>(value & 0xffU));
  }

  void uint32(std::uint32_t value) {
    uint8(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    uint8(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    uint8(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    uint8(static_cast<std::uint8_t>(value & 0xffU));
  }

  void uint64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      uint8(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) &
                                      0xffU));
    }
  }

  void text(std::string_view value) {
    for (const char character : value) {
      uint8(static_cast<std::uint8_t>(character));
    }
  }

  [[nodiscard]] const std::vector<std::byte> &bytes() const noexcept {
    return bytes_;
  }

  [[nodiscard]] std::vector<std::byte> take() noexcept {
    return std::move(bytes_);
  }

private:
  std::vector<std::byte> bytes_;
};

class Reader final {
public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::optional<std::uint8_t> uint8() noexcept {
    if (remaining() < 1) {
      return std::nullopt;
    }
    return std::to_integer<std::uint8_t>(bytes_[position_++]);
  }

  [[nodiscard]] std::optional<std::uint16_t> uint16() noexcept {
    const auto high = uint8();
    const auto low = uint8();
    if (!high.has_value() || !low.has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(*high) << 8U) | *low);
  }

  [[nodiscard]] std::optional<std::uint32_t> uint32() noexcept {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const auto byte = uint8();
      if (!byte.has_value()) {
        return std::nullopt;
      }
      value = (value << 8U) | *byte;
    }
    return value;
  }

  [[nodiscard]] std::optional<std::uint64_t> uint64() noexcept {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      const auto byte = uint8();
      if (!byte.has_value()) {
        return std::nullopt;
      }
      value = (value << 8U) | *byte;
    }
    return value;
  }

  [[nodiscard]] std::optional<std::string> text(std::size_t length) {
    if (remaining() < length) {
      return std::nullopt;
    }
    std::string value;
    value.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      value.push_back(static_cast<char>(
          std::to_integer<unsigned char>(bytes_[position_++])));
    }
    return value;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t position_{0};
};

inline bool validUtf8(std::string_view text,
                      bool rejectControls = false) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead <= 0x7fU) {
      if (rejectControls && (lead <= 0x1fU || lead == 0x7fU)) {
        return false;
      }
      ++index;
      continue;
    }

    std::size_t continuationCount = 0;
    unsigned char secondMinimum = 0x80U;
    unsigned char secondMaximum = 0xbfU;
    std::uint32_t codePoint = 0;
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuationCount = 1;
      codePoint = static_cast<std::uint32_t>(lead & 0x1fU);
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuationCount = 2;
      codePoint = static_cast<std::uint32_t>(lead & 0x0fU);
      if (lead == 0xe0U) {
        secondMinimum = 0xa0U;
      } else if (lead == 0xedU) {
        secondMaximum = 0x9fU;
      }
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuationCount = 3;
      codePoint = static_cast<std::uint32_t>(lead & 0x07U);
      if (lead == 0xf0U) {
        secondMinimum = 0x90U;
      } else if (lead == 0xf4U) {
        secondMaximum = 0x8fU;
      }
    } else {
      return false;
    }

    if (text.size() - index <= continuationCount) {
      return false;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if (second < secondMinimum || second > secondMaximum) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if (continuation < 0x80U || continuation > 0xbfU) {
        return false;
      }
      codePoint =
          (codePoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3fU);
    }
    if (rejectControls && codePoint >= 0x7fU && codePoint <= 0x9fU) {
      return false;
    }
    index += continuationCount + 1;
  }
  return true;
}

} // namespace lol::transport::tcp::wire
