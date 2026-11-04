// Small ABI gaps between the SDK's older libc++/libc++abi and the versions
// used by ReXGlue. These are only needed by the static PS4 target.
#include <cstdint>
#include <ctime>

#include <rex/platform.h>

#if REX_PLATFORM_PS4
extern "C" int __cxa_thread_atexit_impl(void (*)(void*), void*, void*) {
  // Orbis libc has no TLS destructor registry. The process lifetime is the
  // title lifetime, so leave registrations inert rather than fail to link.
  return 0;
}

namespace std::__1 {
void __libcpp_atomic_wait(void const volatile* address, long monitor) {
  // The SDK exports the int32 overload only. Poll the 32-bit atomic payload
  // with a short sleep; this keeps waits correct on Orbis without busy-spins.
  const auto* value = reinterpret_cast<volatile const int32_t*>(address);
  timespec delay{0, 1000000};
  while (*value == static_cast<int32_t>(monitor)) nanosleep(&delay, nullptr);
}
}  // namespace std::__1
#endif
