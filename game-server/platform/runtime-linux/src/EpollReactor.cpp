#include <lol/runtime/linux/EpollReactor.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

namespace lol::runtime::linux {

#if defined(__linux__)
namespace {

void closeFileDescriptor(int &fileDescriptor) noexcept {
  if (fileDescriptor >= 0) {
    ::close(fileDescriptor);
    fileDescriptor = -1;
  }
}

bool addInternalDescriptor(int epollFileDescriptor,
                           int fileDescriptor) noexcept {
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = fileDescriptor;
  return ::epoll_ctl(epollFileDescriptor, EPOLL_CTL_ADD, fileDescriptor,
                     &event) == 0;
}

void drainCounter(int fileDescriptor) noexcept {
  std::uint64_t value = 0;
  while (::read(fileDescriptor, &value, sizeof(value)) < 0 && errno == EINTR) {
  }
}

} // namespace
#endif

EpollReactor::EpollReactor(std::size_t maxReadyEvents)
    : maxReadyEvents_(
          std::min(maxReadyEvents,
                   static_cast<std::size_t>(std::numeric_limits<int>::max()))) {
#if defined(__linux__)
  if (maxReadyEvents_ == 0) {
    return;
  }

  epollFileDescriptor_ = ::epoll_create1(EPOLL_CLOEXEC);
  wakeFileDescriptor_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  timerFileDescriptor_ =
      ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (epollFileDescriptor_ < 0 || wakeFileDescriptor_ < 0 ||
      timerFileDescriptor_ < 0 ||
      !addInternalDescriptor(epollFileDescriptor_, wakeFileDescriptor_) ||
      !addInternalDescriptor(epollFileDescriptor_, timerFileDescriptor_)) {
    closeFileDescriptor(timerFileDescriptor_);
    closeFileDescriptor(wakeFileDescriptor_);
    closeFileDescriptor(epollFileDescriptor_);
  }
#endif
}

EpollReactor::~EpollReactor() {
#if defined(__linux__)
  closeFileDescriptor(timerFileDescriptor_);
  closeFileDescriptor(wakeFileDescriptor_);
  closeFileDescriptor(epollFileDescriptor_);
#endif
}

bool EpollReactor::supported() noexcept {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

bool EpollReactor::valid() const noexcept {
  return epollFileDescriptor_ >= 0 && wakeFileDescriptor_ >= 0 &&
         timerFileDescriptor_ >= 0;
}

bool EpollReactor::watch(int fileDescriptor, bool writable) noexcept {
#if defined(__linux__)
  if (!valid() || fileDescriptor < 0) {
    return false;
  }
  epoll_event event{};
  event.events = EPOLLIN | EPOLLRDHUP;
  if (writable) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fileDescriptor;
  if (::epoll_ctl(epollFileDescriptor_, EPOLL_CTL_ADD, fileDescriptor,
                  &event) == 0) {
    return true;
  }
  return errno == EEXIST && ::epoll_ctl(epollFileDescriptor_, EPOLL_CTL_MOD,
                                        fileDescriptor, &event) == 0;
#else
  static_cast<void>(fileDescriptor);
  static_cast<void>(writable);
  return false;
#endif
}

bool EpollReactor::unwatch(int fileDescriptor) noexcept {
#if defined(__linux__)
  return valid() && fileDescriptor >= 0 &&
         ::epoll_ctl(epollFileDescriptor_, EPOLL_CTL_DEL, fileDescriptor,
                     nullptr) == 0;
#else
  static_cast<void>(fileDescriptor);
  return false;
#endif
}

bool EpollReactor::wake() noexcept {
#if defined(__linux__)
  if (!valid()) {
    return false;
  }
  const std::uint64_t value = 1;
  while (true) {
    const auto written = ::write(wakeFileDescriptor_, &value, sizeof(value));
    if (written == static_cast<ssize_t>(sizeof(value))) {
      return true;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return written < 0 && errno == EAGAIN;
  }
#else
  return false;
#endif
}

bool EpollReactor::armTimer(std::chrono::milliseconds delay) noexcept {
#if defined(__linux__)
  if (!valid()) {
    return false;
  }
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(delay);
  if (nanoseconds <= std::chrono::nanoseconds::zero()) {
    nanoseconds = std::chrono::nanoseconds{1};
  }
  constexpr auto nanosecondsPerSecond = 1'000'000'000LL;
  const auto totalNanoseconds = nanoseconds.count();
  itimerspec timer{};
  timer.it_value.tv_sec =
      static_cast<time_t>(totalNanoseconds / nanosecondsPerSecond);
  timer.it_value.tv_nsec =
      static_cast<long>(totalNanoseconds % nanosecondsPerSecond);
  return ::timerfd_settime(timerFileDescriptor_, 0, &timer, nullptr) == 0;
#else
  static_cast<void>(delay);
  return false;
#endif
}

std::vector<ReadyEvent> EpollReactor::wait(std::chrono::milliseconds timeout) {
#if defined(__linux__)
  if (!valid()) {
    return {};
  }
  std::vector<epoll_event> nativeEvents(maxReadyEvents_);
  const auto boundedTimeout = std::clamp<std::int64_t>(
      timeout.count(), 0, std::numeric_limits<int>::max());
  const int readyCount = ::epoll_wait(epollFileDescriptor_, nativeEvents.data(),
                                      static_cast<int>(nativeEvents.size()),
                                      static_cast<int>(boundedTimeout));
  if (readyCount <= 0) {
    return {};
  }

  std::vector<ReadyEvent> readyEvents;
  readyEvents.reserve(static_cast<std::size_t>(readyCount));
  for (int index = 0; index < readyCount; ++index) {
    const epoll_event event = nativeEvents[static_cast<std::size_t>(index)];
    ReadyKind kind = ReadyKind::Socket;
    if (event.data.fd == wakeFileDescriptor_) {
      kind = ReadyKind::Wake;
      drainCounter(wakeFileDescriptor_);
    } else if (event.data.fd == timerFileDescriptor_) {
      kind = ReadyKind::Timer;
      drainCounter(timerFileDescriptor_);
    }
    readyEvents.push_back(ReadyEvent{
        .kind = kind,
        .fileDescriptor = event.data.fd,
        .readable = (event.events & EPOLLIN) != 0,
        .writable = (event.events & EPOLLOUT) != 0,
        .error = (event.events & EPOLLERR) != 0,
        .hangup = (event.events & (EPOLLHUP | EPOLLRDHUP)) != 0,
    });
  }
  return readyEvents;
#else
  static_cast<void>(timeout);
  return {};
#endif
}

} // namespace lol::runtime::linux
