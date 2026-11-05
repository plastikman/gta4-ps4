// Small ABI gaps between the SDK's older libc++/libc++abi and the versions
// used by ReXGlue. These are only needed by the static PS4 target.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>

#include <emmintrin.h>

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

// The SDK's libc (musl) compares one byte per loop iteration. The renderer's
// constant-bank updates compare every changed range before copying it, which
// made memcmp ~20% of the render worker (ps5diag profile, 2026-11-04). These
// definitions take the place of libc.a's for the whole eboot.
// no_builtin: the byte tail must not be turned back into a memcmp call.
extern "C" __attribute__((no_builtin)) int memcmp(const void* left, const void* right, size_t n) {
  const auto* l = static_cast<const uint8_t*>(left);
  const auto* r = static_cast<const uint8_t*>(right);
  while (n >= 16) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(l));
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r));
    const unsigned differ = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(a, b))) ^ 0xFFFFu;
    if (differ) {
      const unsigned i = unsigned(__builtin_ctz(differ));
      return int(l[i]) - int(r[i]);
    }
    l += 16, r += 16, n -= 16;
  }
  if (n >= 8) {
    uint64_t a, b;
    __builtin_memcpy(&a, l, 8);
    __builtin_memcpy(&b, r, 8);
    if (a != b) {
      const unsigned i = unsigned(__builtin_ctzll(a ^ b)) >> 3;
      return int(l[i]) - int(r[i]);
    }
    l += 8, r += 8, n -= 8;
  }
  for (; n; --n, ++l, ++r) {
    if (*l != *r) return int(*l) - int(*r);
  }
  return 0;
}

// clang emits bcmp for memcmp(...) == 0.
extern "C" int bcmp(const void* left, const void* right, size_t n) { return memcmp(left, right, n); }
#endif
