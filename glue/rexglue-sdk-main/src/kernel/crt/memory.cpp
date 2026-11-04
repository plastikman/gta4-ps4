/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/hook.h>
#include <rex/platform.h>

#if REX_PLATFORM_PS4
#include <atomic>
#include <cstdio>
#include <mutex>
#endif

namespace rex::kernel::crt {

#if REX_PLATFORM_PS4
// [PS4 memcpy-overlap] probe: counts guest memcpy calls whose ranges overlap.
// The OpenOrbis libc memcpy is musl's strict forward copy, so an overlapping
// dst > src copy smears; MSVC/360 and FreeBSD/PS5 memcpy are overlap-safe.
static thread_local uint32_t g_memcpy_caller_lr = 0;

static void NoteMemcpyOverlap(void* dst, const void* src, size_t n) {
  static std::atomic<uint64_t> total{0};
  static std::mutex mutex;
  static uint32_t seen_lr[256];
  static uint32_t seen_count = 0;
  const uintptr_t d = reinterpret_cast<uintptr_t>(dst);
  const uintptr_t s = reinterpret_cast<uintptr_t>(src);
  const bool up = d > s;
  const uint64_t count = total.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = g_memcpy_caller_lr;
  std::lock_guard<std::mutex> lock(mutex);
  bool first = true;
  for (uint32_t i = 0; i < seen_count; ++i) {
    if (seen_lr[i] == lr) {
      first = false;
      break;
    }
  }
  if (first && seen_count < 256) {
    seen_lr[seen_count++] = lr;
  }
  if (!first && (count % 1000) != 0) {
    return;
  }
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fprintf(log,
                 "[PS4 memcpy-overlap] %s %s lr=%08x dst=%08x src=%08x n=%#zx total=%llu\n",
                 first ? "NEW" : "periodic", up ? "UP(smears-on-musl)" : "down",
                 lr, static_cast<uint32_t>(d), static_cast<uint32_t>(s), n,
                 static_cast<unsigned long long>(count));
    std::fclose(log);
  }
}
#endif

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

// The guest CRT's memcpy (MSVC) is overlap-safe, and guest code relies on it.
// Host libcs differ (OpenOrbis/musl copies strictly forward), so all memcpy
// variants route through memmove.
static void* native_memcpy(void* dst, const void* src, size_t n) {
#if REX_PLATFORM_PS4
  const auto d = reinterpret_cast<uintptr_t>(dst);
  const auto s = reinterpret_cast<uintptr_t>(src);
  if (n != 0 && d != s && (d < s ? s - d : d - s) < n) {
    NoteMemcpyOverlap(dst, src, n);
  }
#endif
  return std::memmove(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  return native_memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  native_memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

#if REX_PLATFORM_PS4
REX_HOOK_RAW(rexcrt_memcpy) {
  rex::kernel::crt::g_memcpy_caller_lr = static_cast<uint32_t>(ctx.lr);
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_memcpy>(ctx, base);
}
#else
REX_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
#endif
REX_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
REX_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
#if REX_PLATFORM_PS4
REX_HOOK_RAW(rexcrt_XMemCpy) {
  rex::kernel::crt::g_memcpy_caller_lr = static_cast<uint32_t>(ctx.lr);
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_XMemCpy>(ctx, base);
}
#else
REX_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
#endif
REX_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
