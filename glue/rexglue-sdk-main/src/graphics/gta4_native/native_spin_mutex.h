#pragma once

#include <atomic>
#include <thread>

namespace rex::graphics::gta4_native {

// [ps5 port] For locks taken for every command the game submits and almost
// never contended: a spin lock instead of two libkernel calls each time.
// (PS4: a thread that blocks in the kernel can take ~1 ms to run again.)
class NativeSpinMutex {
 public:
  void lock() {
    int spins = 0;
    while (locked_.exchange(true, std::memory_order_acquire)) {
      while (locked_.load(std::memory_order_relaxed)) {
        if (++spins < 1024) {
          __builtin_ia32_pause();
        } else {
          std::this_thread::yield();
          spins = 0;
        }
      }
    }
  }
  bool try_lock() {
    return !locked_.load(std::memory_order_relaxed) &&
           !locked_.exchange(true, std::memory_order_acquire);
  }
  void unlock() { locked_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> locked_{false};
};

}  // namespace rex::graphics::gta4_native
