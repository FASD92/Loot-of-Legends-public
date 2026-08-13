#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace lol::runtime::linux {

enum class ReadyKind { Socket, Wake, Timer };

struct ReadyEvent final {
  ReadyKind kind;
  int fileDescriptor;
  bool readable;
  bool writable;
  bool error;
  bool hangup;
};

class EpollReactor final {
public:
  explicit EpollReactor(std::size_t maxReadyEvents);
  ~EpollReactor();

  EpollReactor(const EpollReactor &) = delete;
  EpollReactor &operator=(const EpollReactor &) = delete;

  [[nodiscard]] static bool supported() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool watch(int fileDescriptor, bool writable = false) noexcept;
  [[nodiscard]] bool unwatch(int fileDescriptor) noexcept;
  [[nodiscard]] bool wake() noexcept;
  [[nodiscard]] bool armTimer(std::chrono::milliseconds delay) noexcept;
  [[nodiscard]] std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout);

private:
  [[maybe_unused]] std::size_t maxReadyEvents_;
  int epollFileDescriptor_{-1};
  int wakeFileDescriptor_{-1};
  int timerFileDescriptor_{-1};
};

} // namespace lol::runtime::linux
