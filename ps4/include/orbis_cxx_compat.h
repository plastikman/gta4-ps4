#pragma once

// The Orbis SDK's bundled libc++ headers predate several C++20/23 facilities
// used by ReXGlue. Supply the small subset needed by the runtime.
// Pull in Orbis's errno values before libc++'s fallback errno definitions.
// In particular, libc++ must compare pthread_cond_timedwait's ETIMEDOUT (60)
// against the Orbis value, not its generic fallback constant.
#include <bits/errno.h>
#include <time.h>
#include <atomic>
#include <bit>
#include <concepts>
#include <functional>
#include <memory>
#include <new>
#include <thread>
#include <time.h>
#include <sys/stat.h>
#include <type_traits>
#include <utility>

#define stat64 stat
#define fstat64 fstat

namespace std {

#if __cplusplus >= 202002L
template <class T>
concept integral = is_integral_v<T>;
template <class From, class To>
concept convertible_to = is_convertible_v<From, To>;
#endif

template <class To, class From>
constexpr To bit_cast(const From& from) noexcept {
  return __builtin_bit_cast(To, from);
}

template <class T>
constexpr typename enable_if<is_integral<T>::value && !is_same<T, bool>::value, T>::type
byteswap(T value) noexcept {
  return sizeof(T) == 1 ? value :
         sizeof(T) == 2 ? static_cast<T>(__builtin_bswap16(static_cast<unsigned short>(value))) :
         sizeof(T) == 4 ? static_cast<T>(__builtin_bswap32(static_cast<unsigned int>(value))) :
                          static_cast<T>(__builtin_bswap64(static_cast<unsigned long long>(value)));
}

template <class T, class... Args>
T* construct_at(T* location, Args&&... args) {
  return ::new (static_cast<void*>(location)) T(std::forward<Args>(args)...);
}
template <class T>
constexpr int bit_width(T value) noexcept {
  return value ? int(sizeof(T) * 8 - __builtin_clzll(static_cast<unsigned long long>(value))) : 0;
}
template <class T>
constexpr T bit_ceil(T value) noexcept {
  return value <= 1 ? T(1) : T(1) << bit_width(T(value - 1));
}
template <class T>
constexpr bool has_single_bit(T value) noexcept {
  return value && !(value & (value - 1));
}

class stop_token {
 public:
  stop_token() = default;
  explicit stop_token(shared_ptr<atomic<bool>> state) : state_(std::move(state)) {}
  bool stop_requested() const noexcept { return state_ && state_->load(memory_order_acquire); }

 private:
  shared_ptr<atomic<bool>> state_;
};

class jthread {
 public:
  using id = thread::id;
  jthread() = default;
  template <class F>
  explicit jthread(F&& function) : stop_(make_shared<atomic<bool>>(false)) {
    auto state = stop_;
    thread_ = thread([state, function = forward<F>(function)]() mutable {
      function(stop_token(state));
    });
  }
  jthread(jthread&& other) noexcept
      : stop_(std::move(other.stop_)), thread_(std::move(other.thread_)) {}
  jthread& operator=(jthread&& other) noexcept {
    if (this != &other) {
      request_stop();
      if (thread_.joinable()) thread_.join();
      stop_ = std::move(other.stop_);
      thread_ = std::move(other.thread_);
    }
    return *this;
  }
  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;
  ~jthread() {
    request_stop();
    if (thread_.joinable()) thread_.join();
  }
  bool request_stop() noexcept {
    if (!stop_) return false;
    return !stop_->exchange(true, memory_order_acq_rel);
  }
  bool joinable() const noexcept { return thread_.joinable(); }
  void join() { thread_.join(); }
  id get_id() const noexcept { return thread_.get_id(); }

 private:
  shared_ptr<atomic<bool>> stop_;
  thread thread_;
};

}  // namespace std
