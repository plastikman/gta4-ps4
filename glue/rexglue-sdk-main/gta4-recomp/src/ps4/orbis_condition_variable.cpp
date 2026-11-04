#include <condition_variable>
#include <cerrno>
#include <chrono>
#include <limits>
#include <pthread.h>
#include <system_error>
#include <time.h>

namespace std {

// Replace the Orbis SDK libc++ implementation. Its prebuilt version compares
// pthread_cond_timedwait's return code with Linux ETIMEDOUT (110), although
// Orbis returns 60. The header ETIMEDOUT definition cannot fix that compiled
// comparison, so handle Orbis's value here.
void condition_variable::__do_timed_wait(
    unique_lock<mutex>& lock,
    chrono::time_point<chrono::system_clock, chrono::nanoseconds> deadline) noexcept {
  if (!lock.owns_lock()) {
    __throw_system_error(EPERM, "condition_variable::timed wait: mutex not locked");
    return;
  }

  using namespace chrono;
  const nanoseconds duration = deadline.time_since_epoch();
  const seconds whole_seconds = duration_cast<seconds>(duration);
  timespec timeout{};
  using seconds_type = decltype(timeout.tv_sec);
  const seconds_type max_seconds = numeric_limits<seconds_type>::max();
  if (whole_seconds.count() < max_seconds) {
    timeout.tv_sec = static_cast<seconds_type>(whole_seconds.count());
    timeout.tv_nsec = (duration - whole_seconds).count();
  } else {
    timeout.tv_sec = max_seconds;
    timeout.tv_nsec = 999999999;
  }

  const int result = pthread_cond_timedwait(
      native_handle(), lock.mutex()->native_handle(), &timeout);
  if (result != 0 && result != ETIMEDOUT)
    __throw_system_error(result, "condition_variable timed_wait failed");
}

}  // namespace std
