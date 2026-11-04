/**
 * @file        core/memory_ps4.cpp
 * @brief       PlayStation 4 memory backend (Orbis direct-memory)
 *
 * Unlike Linux/macOS (which use anonymous mmap), PS4 must back guest memory
 * with Orbis direct-memory. Anonymous mmap on Orbis consumes flexible memory
 * (~512 MB process budget) — the guest wants ~4.5 GB, so mmap will OOM.
 *
 * Guest reservations therefore go through:
 *   sceKernelAllocateDirectMemory(0, SCE_KERNEL_MAIN_DMEM_SIZE, len, 2 MB,
 *                                  SCE_KERNEL_WB_ONION, &dmem_off)
 *   sceKernelMapNamedDirectMemory(&vaddr, len, prot, MAP_FIXED, dmem_off, 0, "LR_Guest")
 *   sceKernelReleaseDirectMemory(dmem_off, len)  // on release
 *
 * A side-table tracks vaddr -> (dmem_offset, length) so DeallocFixed can
 * release the backing physical memory. Non-fixed allocations (file views,
 * shm) still use POSIX mmap and are unaffected.
 *
 * Differences from Linux:
 *   - 64-bit file APIs are the default (no *64 suffix variants).
 *   - MAP_ANONYMOUS is spelled MAP_ANON.
 *   - No /proc/self/maps — QueryProtect returns kNoAccess.
 *
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/diagnostics/policy.h>
#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_PS4
#  include <orbis/libkernel.h>
// Orbis kernel memory flags are supplied below for SDK bundles without these defines.
#endif

// POSIX PROT_EXEC is 0x04; OpenOrbis does not define SCE_KERNEL_PROT_CPU_EXEC
// but the kernel accepts the same bit value. Keep a local alias for clarity.
#if REX_PLATFORM_PS4
#  define SCE_KERNEL_WB_ONION 0x00000000
#  define SCE_KERNEL_PROT_CPU_READ 0x01
#  define SCE_KERNEL_PROT_CPU_WRITE 0x02
#  define SCE_KERNEL_MAIN_DMEM_SIZE sceKernelGetDirectMemorySize()
#  ifndef SCE_KERNEL_PROT_CPU_EXEC
#    define SCE_KERNEL_PROT_CPU_EXEC 0x04
#  endif
#endif

// PS4 (FreeBSD-based) uses 64-bit file offsets natively; no *64 API variants needed.
#ifndef ftruncate64
#  define ftruncate64 ftruncate
#endif
#ifndef mmap64
#  define mmap64 mmap
#endif
#ifndef fstat64
#  define fstat64 fstat
#endif
#ifndef off64_t
typedef off_t off64_t;
#endif

// Ensure MAP_ANONYMOUS is defined (FreeBSD / PS4 use MAP_ANON).
#ifndef MAP_ANONYMOUS
#  ifdef MAP_ANON
#    define MAP_ANONYMOUS MAP_ANON
#  else
#    define MAP_ANONYMOUS 0x1000
#  endif
#endif

namespace rex {
namespace memory {

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_PS4
// Orbis limits POSIX shared-memory objects to the flexible-memory budget, which
// is too small for the 4.5 GiB sparse guest address space. A regular file keeps
// the backing sparse on disk and lets mmap fault pages in as the guest uses
// them. The installer creates this writable data directory before startup.
static std::string MakeFileBackingPath(const std::filesystem::path& path) {
  return std::string("/data/LibertyRecomp/") + path.filename().string();
}

static void Ps4MemoryLog(const char* operation, int error, size_t length,
                         const char* path) {
  char line[512];
  std::snprintf(line, sizeof(line),
                "[GTA4 PS4 memory] %s failed: errno=%d length=%zu path=%s\n",
                operation, error, length, path);
  sceKernelDebugOutText(0, line);
}

static void Ps4MemoryMapProgress(const char* stage, size_t offset, size_t length) {
  char line[192];
  std::snprintf(line, sizeof(line),
                "[GTA4 PS4 memory] %s: offset=0x%zx length=0x%zx\n", stage,
                offset, length);
  sceKernelDebugOutText(0, line);
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

#if REX_PLATFORM_PS4
// Convert the rex PageAccess enum to SCE_KERNEL_PROT_CPU_* bits for
// sceKernelMapNamedDirectMemory. POSIX PROT_READ / PROT_WRITE happen to
// match SCE_KERNEL_PROT_CPU_READ / WRITE bit-for-bit on Orbis, so we can
// build the value from ToPosixProtectFlags() and translate PROT_EXEC.
static int32_t ToOrbisCpuProt(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return 0;
    case PageAccess::kReadOnly:
      return SCE_KERNEL_PROT_CPU_READ;
    case PageAccess::kReadWrite:
      return SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE;
    case PageAccess::kExecuteReadOnly:
      return SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_EXEC;
    case PageAccess::kExecuteReadWrite:
      return SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE |
             SCE_KERNEL_PROT_CPU_EXEC;
    default:
      return 0;
  }
}

// Orbis uses 16 KiB VM pages. This is the direct-memory allocation and map
// granularity validated by the PS4 Wine backend in ps4-gladius.
static constexpr size_t kOrbisDirectAlign = 16ULL * 1024ULL;

// Side-table of live direct-memory reservations so DeallocFixed can call
// sceKernelReleaseDirectMemory with the matching (dmem_off, len).
struct DirectMemEntry {
  off_t  dmem_offset;
  off_t  backing_offset;
  size_t backing_length;
  size_t length;
};
static std::mutex                               g_dmem_mutex;
static std::unordered_map<void*, DirectMemEntry> g_dmem_table;
static std::unordered_map<off_t, size_t>         g_dmem_refcounts;
static thread_local void*                        g_alias_source = nullptr;

// Per-16 KiB host page record of which direct memory backs each page of the
// reserved guest range (both the virtual and the physical membase), so a
// commit never replaces backing that already holds guest data. Windows/Xbox
// commit semantics - and the PS5/POSIX backend, where every guest view is one
// mapping fixed at startup - keep existing contents when a range is committed
// again or when a neighbouring 4 KiB guest page shares the host page.
static constexpr uintptr_t kTrackedBase = 0x1000000000ull;
static constexpr size_t    kTrackedSize = 0x200000000ull;
static constexpr off_t     kUnbacked = -1;
static std::vector<off_t>  g_page_backing;  // guarded by g_dmem_mutex

static off_t* PageBackingSlot(uintptr_t address) {
  if (address < kTrackedBase || address >= kTrackedBase + kTrackedSize) return nullptr;
  if (g_page_backing.empty()) {
    g_page_backing.assign(kTrackedSize / kOrbisDirectAlign, kUnbacked);
  }
  return &g_page_backing[(address - kTrackedBase) / kOrbisDirectAlign];
}

static void TraceGuestLine(const char* line, int len) {
  if (len <= 0) return;
  const int fd = open("/data/LibertyRecomp/logs/gta4-guest-trace.txt",
                      O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd >= 0) {
    (void)write(fd, line, static_cast<size_t>(len));
    close(fd);
  }
}

// [PS4 commit-over-live] counts commits that reached host pages which already
// had backing. The previous backend mapped fresh direct memory over them and
// discarded their contents.
static void NoteCommitOverLive(void* base, size_t length, size_t live_pages) {
  static std::atomic<uint64_t> calls{0};
  static std::atomic<uint64_t> pages{0};
  const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t p = pages.fetch_add(live_pages, std::memory_order_relaxed) + live_pages;
  if (n > 64 && (n % 1000) != 0) return;
  char line[200];
  const int len = std::snprintf(
      line, sizeof(line),
      "[PS4 commit-over-live] guest=%08llx length=%zx live_pages=%zu calls=%llu pages=%llu\n",
      static_cast<unsigned long long>(
          (reinterpret_cast<uintptr_t>(base) - kTrackedBase) & 0xFFFFFFFFull),
      length, live_pages, static_cast<unsigned long long>(n),
      static_cast<unsigned long long>(p));
  TraceGuestLine(line, len);
}

static void TraceDirectAlias(const char* phase, void* target, size_t length,
                             void* source, off_t offset, int32_t result) {
  // Successful maps stay capped at 24: startup produces many. Failures are the
  // interesting case and used to be swallowed by that same cap (and only ever
  // went to sceKernelDebugOutText, which this setup does not capture), so they
  // get their own, much larger budget and also go to the guest trace.
  const bool failure = std::strcmp(phase, "map-ok") != 0;
  static std::atomic<unsigned> ok_count{0};
  static std::atomic<unsigned> failure_count{0};
  if (failure ? failure_count.fetch_add(1, std::memory_order_relaxed) >= 512
              : ok_count.fetch_add(1, std::memory_order_relaxed) >= 24) {
    return;
  }
  char line[256];
  const int len = std::snprintf(line, sizeof(line),
                                "[GTA4 PS4 direct alias] %s target=%p length=%zx source=%p "
                                "offset=%llx result=%d\n",
                                phase, target, length, source,
                                static_cast<unsigned long long>(offset), result);
  sceKernelDebugOutText(0, line);
  if (failure && len > 0) {
    const int fd = open("/data/LibertyRecomp/logs/gta4-guest-trace.txt",
                        O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) {
      (void)write(fd, line, static_cast<size_t>(len));
      close(fd);
    }
  }
}

static inline size_t RoundUpTo(size_t v, size_t align) {
  return (v + align - 1) & ~(align - 1);
}
#endif  // REX_PLATFORM_PS4

#if REX_PLATFORM_PS4
void* SetDirectMemoryAliasSource(void* source_address) {
  void* previous = g_alias_source;
  g_alias_source = source_address;
  return previous;
}
#endif

bool IsWritableExecutableMemorySupported() {
  // PS4 retail / userland homebrew cannot map PROT_EXEC pages without a kernel
  // exploit (JIT path). Keep false so the recomp generator routes through its
  // non-RWX code-patch path on PS4.
  return false;
}

#if REX_PLATFORM_PS4
// Fixed-address commit/reserve inside the guest range. Back only the host
// pages that have no direct memory yet (zero-filled, as the guest expects of
// fresh pages) and leave existing backing - and its contents - in place.
static void* AllocFixedTracked(void* base_address, size_t length,
                               AllocationType allocation_type, PageAccess access) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(base_address) & ~(kOrbisDirectAlign - 1);
  const uintptr_t end = RoundUpTo(reinterpret_cast<uintptr_t>(base_address) + length,
                                  kOrbisDirectAlign);
  const int32_t prot =
      allocation_type == AllocationType::kReserve ? 0 : ToOrbisCpuProt(access);
  std::lock_guard<std::mutex> lock(g_dmem_mutex);
  size_t live_pages = 0;
  uintptr_t page = start;
  while (page < end) {
    off_t* slot = PageBackingSlot(page);
    if (!slot) return nullptr;
    if (*slot != kUnbacked) {
      ++live_pages;
      page += kOrbisDirectAlign;
      continue;
    }
    uintptr_t run_end = page + kOrbisDirectAlign;
    while (run_end < end) {
      off_t* next = PageBackingSlot(run_end);
      if (!next || *next != kUnbacked) break;
      run_end += kOrbisDirectAlign;
    }
    const size_t run_length = run_end - page;
    off_t dmem_offset = 0;
    int32_t res = sceKernelAllocateDirectMemory(0, SCE_KERNEL_MAIN_DMEM_SIZE, run_length,
                                                kOrbisDirectAlign, SCE_KERNEL_WB_ONION,
                                                &dmem_offset);
    if (res < 0) {
      char line[200];
      const int len = std::snprintf(
          line, sizeof(line),
          "[PS4 dmem trace] tracked allocate failed base=%p length=%zx result=0x%08x\n",
          reinterpret_cast<void*>(page), run_length, static_cast<unsigned>(res));
      sceKernelDebugOutText(0, line);
      TraceGuestLine(line, len);
      return nullptr;
    }
    void* mapped = reinterpret_cast<void*>(page);
    res = sceKernelMapNamedDirectMemory(&mapped, run_length,
                                        SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE,
                                        MAP_FIXED, dmem_offset, kOrbisDirectAlign, "LR_Guest");
    if (res < 0 || mapped != reinterpret_cast<void*>(page)) {
      sceKernelReleaseDirectMemory(dmem_offset, run_length);
      char line[200];
      const int len = std::snprintf(
          line, sizeof(line),
          "[PS4 dmem trace] tracked map failed base=%p length=%zx result=0x%08x\n",
          reinterpret_cast<void*>(page), run_length, static_cast<unsigned>(res));
      sceKernelDebugOutText(0, line);
      TraceGuestLine(line, len);
      return nullptr;
    }
    std::memset(mapped, 0, run_length);
    g_dmem_table[mapped] = DirectMemEntry{dmem_offset, dmem_offset, run_length, run_length};
    g_dmem_refcounts[dmem_offset] = 1;
    for (uintptr_t p = page; p < run_end; p += kOrbisDirectAlign) {
      *PageBackingSlot(p) = dmem_offset + static_cast<off_t>(p - page);
    }
    page = run_end;
  }
  if (live_pages && allocation_type != AllocationType::kReserve) {
    NoteCommitOverLive(base_address, length, live_pages);
  }
  // Reserving must not revoke access to pages that already hold data.
  if (allocation_type != AllocationType::kReserve || live_pages == 0) {
    sceKernelMprotect(reinterpret_cast<void*>(start), end - start, prot);
  }
  return base_address;
}
#endif

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create inaccessible mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access
  // - New allocation: map backing storage at base_address

#if REX_PLATFORM_PS4
  const size_t aligned_length = RoundUpTo(length, kOrbisDirectAlign);
  void* alias_source = g_alias_source;
  g_alias_source = nullptr;
  if (alias_source) {
    DirectMemEntry source_entry{};
    void* source_base = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_dmem_mutex);
      uintptr_t source = reinterpret_cast<uintptr_t>(alias_source);
      for (const auto& item : g_dmem_table) {
        uintptr_t begin = reinterpret_cast<uintptr_t>(item.first);
        if (source >= begin && source < begin + item.second.length) {
          source_entry = item.second;
          source_base = item.first;
          break;
        }
      }
      if (!source_base || source + aligned_length >
                               reinterpret_cast<uintptr_t>(source_base) + source_entry.length) {
        TraceDirectAlias("source-invalid", base_address, aligned_length, alias_source,
                         source_entry.dmem_offset, -1);
        return nullptr;
      }
    }
    off_t offset = source_entry.dmem_offset +
                   static_cast<off_t>(reinterpret_cast<uintptr_t>(alias_source) -
                                      reinterpret_cast<uintptr_t>(source_base));
    {
      // The physical page table is authoritative: g_dmem_table can hold a
      // stale entry for a range that was later backed again.
      std::lock_guard<std::mutex> lock(g_dmem_mutex);
      const uintptr_t source = reinterpret_cast<uintptr_t>(alias_source);
      off_t* first = PageBackingSlot(source & ~(kOrbisDirectAlign - 1));
      if (first && *first != kUnbacked) {
        const off_t page_offset =
            static_cast<off_t>(source & (kOrbisDirectAlign - 1));
        bool contiguous = true;
        for (size_t i = 1; i < aligned_length / kOrbisDirectAlign; ++i) {
          off_t* slot = PageBackingSlot((source & ~(kOrbisDirectAlign - 1)) +
                                        i * kOrbisDirectAlign);
          if (!slot || *slot != *first + static_cast<off_t>(i * kOrbisDirectAlign)) {
            contiguous = false;
            break;
          }
        }
        if (contiguous) {
          offset = *first + page_offset;
        } else {
          TraceDirectAlias("source-discontiguous", base_address, aligned_length, alias_source,
                           *first, -1);
        }
      }
      // Already aliased to this backing: keep the mapping, update protection.
      if (base_address) {
        bool same = true;
        for (size_t i = 0; i < aligned_length / kOrbisDirectAlign; ++i) {
          off_t* slot = PageBackingSlot(reinterpret_cast<uintptr_t>(base_address) +
                                        i * kOrbisDirectAlign);
          if (!slot || *slot != offset + static_cast<off_t>(i * kOrbisDirectAlign)) {
            same = false;
            break;
          }
        }
        if (same) {
          const int32_t prot =
              allocation_type == AllocationType::kReserve ? 0 : ToOrbisCpuProt(access);
          if (allocation_type != AllocationType::kReserve) {
            sceKernelMprotect(base_address, aligned_length, prot);
          }
          return base_address;
        }
      }
    }
    void* mapped = base_address;
    int32_t res = sceKernelMapNamedDirectMemory(
        &mapped, aligned_length,
        allocation_type == AllocationType::kReserve ? 0 : ToOrbisCpuProt(access),
        base_address ? MAP_FIXED : 0, offset, kOrbisDirectAlign, "LR_GuestAlias");
    if (res < 0) {
      TraceDirectAlias("map-failed", base_address, aligned_length, alias_source, offset, res);
      return nullptr;
    }
    TraceDirectAlias("map-ok", mapped, aligned_length, alias_source, offset, res);
    {
      std::lock_guard<std::mutex> lock(g_dmem_mutex);
      g_dmem_table[mapped] = DirectMemEntry{offset, source_entry.backing_offset,
                                            source_entry.backing_length, aligned_length};
      ++g_dmem_refcounts[source_entry.backing_offset];
      for (size_t i = 0; i < aligned_length / kOrbisDirectAlign; ++i) {
        if (off_t* slot = PageBackingSlot(reinterpret_cast<uintptr_t>(mapped) +
                                          i * kOrbisDirectAlign)) {
          *slot = offset + static_cast<off_t>(i * kOrbisDirectAlign);
        }
      }
    }
    return mapped;
  }

  if (base_address && PageBackingSlot(reinterpret_cast<uintptr_t>(base_address))) {
    return AllocFixedTracked(base_address, length, allocation_type, access);
  }

  // On Orbis we must use direct-memory (physical pool) rather than anonymous
  // mmap, which draws from flexible memory and maxes out at ~512 MB.
  // Direct-memory has 2 MB allocation granularity — round up.

  // Reserve mapping: still created with PROT_NONE, but *backed* by direct mem
  // so the reservation sits in the same pool as future commits.
  const int32_t orbis_prot_initial =
      (allocation_type == AllocationType::kReserve) ? 0 : ToOrbisCpuProt(access);

  // Use sceKernelAllocateDirectMemory (6-arg variant) because the OpenOrbis
  // header declares sceKernelAllocateMainDirectMemory's last arg as off_t
  // (by value) rather than off_t* — passing an address via that signature
  // is not portable. The 6-arg form matches user_malloc_ps4.cpp's pattern.
  off_t        dmem_offset  = 0;
  size_t       effective_len = aligned_length;
  int32_t      res          = sceKernelAllocateDirectMemory(
      /*searchStart*/ 0,
      /*searchEnd*/   SCE_KERNEL_MAIN_DMEM_SIZE,
      /*len*/         effective_len,
      /*alignment*/   kOrbisDirectAlign,
      /*memoryType*/  SCE_KERNEL_WB_ONION,
      /*physAddrOut*/ &dmem_offset);
  if (res < 0) {
    char trace[256];
    std::snprintf(trace, sizeof(trace),
                  "[PS4 dmem trace] allocate failed base=%p length=%zu aligned=%zu result=0x%08x\n",
                  base_address, length, aligned_length, static_cast<unsigned>(res));
    sceKernelDebugOutText(0, trace);
    // Fallback ladder: try progressively smaller reservations so a slightly
    // over-budget request (e.g. guest wants 4.5 GB, only 4 GB free) still
    // succeeds with a reduced footprint. We log the reduction once.
    static bool s_warned = false;
    if (diagnostics::IsEnabled(diagnostics::Category::kLogging) &&
        !s_warned) {
      s_warned = true;
      std::fprintf(stderr,
                   "[memory_ps4] AllocateDirectMemory(%zu) failed "
                   "(res=0x%08x); falling back to smaller sizes.\n",
                   aligned_length, static_cast<unsigned>(res));
    }
    const size_t kFallbacks[] = {
        4ULL * 1024ULL * 1024ULL * 1024ULL,  // 4 GB
        2ULL * 1024ULL * 1024ULL * 1024ULL,  // 2 GB
        1ULL * 1024ULL * 1024ULL * 1024ULL,  // 1 GB
    };
    for (size_t fb : kFallbacks) {
      if (fb >= aligned_length) continue;  // only try *smaller* than the ask
      res = sceKernelAllocateDirectMemory(0, SCE_KERNEL_MAIN_DMEM_SIZE, fb,
                                          kOrbisDirectAlign,
                                          SCE_KERNEL_WB_ONION, &dmem_offset);
      if (res >= 0) {
        if (diagnostics::IsEnabled(diagnostics::Category::kLogging)) {
          std::fprintf(stderr,
                       "[memory_ps4] Reduced reservation: requested=%zu got=%zu\n",
                       aligned_length, fb);
        }
        effective_len = fb;
        break;
      }
    }
    if (res < 0) {
      std::snprintf(trace, sizeof(trace),
                    "[PS4 dmem trace] all allocation fallbacks failed length=%zu result=0x%08x\n",
                    aligned_length, static_cast<unsigned>(res));
      sceKernelDebugOutText(0, trace);
      std::fprintf(stderr,
                   "[memory_ps4] All direct-memory fallbacks exhausted\n");
      return nullptr;
    }
  }

  const size_t final_length = effective_len;
  void*        mapped       = base_address;
  // MAP_FIXED is honored by sceKernelMapNamedDirectMemory when base_address
  // is non-null. If null, the kernel picks an address.
  const int32_t map_flags = base_address ? MAP_FIXED : 0;
  res = sceKernelMapNamedDirectMemory(&mapped, final_length, orbis_prot_initial,
                                      map_flags, dmem_offset, kOrbisDirectAlign, "LR_Guest");
  if (res < 0) {
    sceKernelReleaseDirectMemory(dmem_offset, final_length);
    char trace[256];
    std::snprintf(trace, sizeof(trace),
                  "[PS4 dmem trace] map failed base=%p length=%zu prot=0x%x result=0x%08x\n",
                  base_address, final_length, static_cast<unsigned>(orbis_prot_initial),
                  static_cast<unsigned>(res));
    sceKernelDebugOutText(0, trace);
    std::fprintf(stderr,
                 "[memory_ps4] MapNamedDirectMemory(len=%zu, prot=0x%x) "
                 "failed (res=0x%08x)\n",
                 final_length, static_cast<unsigned>(orbis_prot_initial),
                 static_cast<unsigned>(res));
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(g_dmem_mutex);
    g_dmem_table[mapped] = DirectMemEntry{dmem_offset, dmem_offset, final_length,
                                         final_length};
    g_dmem_refcounts[dmem_offset] = 1;
  }
  return mapped;

#else  // Non-PS4 fallback — retained so the file remains compilable if the
       // CMake gate ever slips. macOS/Linux builds use memory_mac.cpp /
       // memory_posix.cpp instead and never reach this path.
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#  if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#  else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#  endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  return result == MAP_FAILED ? nullptr : result;
#endif
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: drop permissions but keep the address-space reservation.
      // The direct-memory backing stays tied to the virtual range; we just
      // disable access. madvise doesn't apply to direct-memory mappings.
#if REX_PLATFORM_PS4
      return sceKernelMprotect(base_address, length, 0) == 0;
#else
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#  if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#  endif
      return true;
#endif
    }
    case DeallocationType::kRelease: {
#if REX_PLATFORM_PS4
      // Look up the dmem backing, unmap the virtual range, release physical.
      DirectMemEntry entry{};
      bool           found = false;
      bool           release_backing = false;
      {
        std::lock_guard<std::mutex> lock(g_dmem_mutex);
        auto                        it = g_dmem_table.find(base_address);
        if (it != g_dmem_table.end()) {
          entry = it->second;
          g_dmem_table.erase(it);
          auto ref = g_dmem_refcounts.find(entry.backing_offset);
          if (ref != g_dmem_refcounts.end()) {
            if (--ref->second == 0) {
              g_dmem_refcounts.erase(ref);
              release_backing = true;
            }
          }
          found = true;
        }
      }
      if (!found) {
        // Fall back to munmap — safest for pointers we didn't allocate via
        // AllocFixed (shouldn't happen in practice but avoids leaks).
        return munmap(base_address, length) == 0;
      }
      {
        std::lock_guard<std::mutex> lock(g_dmem_mutex);
        for (size_t i = 0; i < entry.length / kOrbisDirectAlign; ++i) {
          if (off_t* slot = PageBackingSlot(reinterpret_cast<uintptr_t>(base_address) +
                                            i * kOrbisDirectAlign)) {
            *slot = kUnbacked;
          }
        }
      }
      bool ok = (sceKernelMunmap(base_address, entry.length) == 0);
      if (release_backing) {
        ok &= (sceKernelReleaseDirectMemory(entry.backing_offset, entry.backing_length) == 0);
      }
      return ok;
#else
      return munmap(base_address, length) == 0;
#endif
    }
    default:
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    // No /proc/self/maps on PS4 — best effort only.
    *out_old_access = PageAccess::kNoAccess;
  }
#if REX_PLATFORM_PS4
  // sceKernelMprotect on a direct-memory mapping takes CPU_* prot bits.
  return sceKernelMprotect(base_address, length, ToOrbisCpuProt(access)) == 0;
#else
  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, prot) == 0;
#endif
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  // No /proc/self/maps on PS4 (FreeBSD kernel). sceKernelQueryMemoryProtection
  // exists but returns raw CPU/GPU bits we'd have to round-trip back into the
  // rex enum; leaving it as a stub matches memory_mac.cpp semantics.
  // TODO(ps4): wire up sceKernelQueryMemoryProtection if a caller starts
  // relying on out_old_access beyond "non-null / null".
  static bool s_warned = false;
  if (diagnostics::IsEnabled(diagnostics::Category::kLogging) && !s_warned) {
    s_warned = true;
    std::fprintf(stderr,
                 "[memory_ps4] QueryProtect is a stub on PS4; returning "
                 "kNoAccess. Wire sceKernelQueryMemoryProtection if needed.\n");
  }
  (void)base_address;
  access_out = PageAccess::kNoAccess;
  length     = 0;
  return false;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  (void)commit;

  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret >= 0 && ftruncate(ret, static_cast<off_t>(length)) == 0) {
    return static_cast<FileMappingHandle>(ret);
  }
#if REX_PLATFORM_PS4
  int shm_error = errno;
  if (ret >= 0) {
    Ps4MemoryLog("shared-memory ftruncate", shm_error, length, full_path.c_str());
    close(ret);
    shm_unlink(full_path.c_str());
  } else {
    Ps4MemoryLog("shm_open", shm_error, length, full_path.c_str());
  }

  const std::string file_path = MakeFileBackingPath(path);
  ret = open(file_path.c_str(), O_CREAT | O_RDWR, 0600);
  if (ret < 0) {
    Ps4MemoryLog("open sparse backing file", errno, length, file_path.c_str());
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, static_cast<off_t>(length)) != 0) {
    int saved_errno = errno;
    close(ret);
    unlink(file_path.c_str());
    Ps4MemoryLog("sparse backing ftruncate", saved_errno, length, file_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#else
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  close(ret);
  shm_unlink(full_path.c_str());
  return kFileMappingHandleInvalid;
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#if REX_PLATFORM_PS4
  const std::string file_path = MakeFileBackingPath(path);
  unlink(file_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;
  // File views replace prior anonymous reservations, so we need MAP_FIXED.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
#if REX_PLATFORM_PS4
  if (base_address) {
    constexpr size_t kMapChunkSize = 64ULL * 1024ULL * 1024ULL;
    auto* target_base = static_cast<uint8_t*>(base_address);
    for (size_t offset = 0; offset < length;) {
      const size_t chunk_length = std::min(kMapChunkSize, length - offset);
      if ((offset % (256ULL * 1024ULL * 1024ULL)) == 0 || offset + chunk_length == length) {
        Ps4MemoryMapProgress("file-map chunk begin", file_offset + offset, chunk_length);
      }
      void* target = target_base + offset;
      void* result = mmap(target, chunk_length, prot, flags, static_cast<int>(handle),
                          static_cast<off_t>(file_offset + offset));
      if (result == MAP_FAILED || result != target) {
        const int saved_errno = errno;
        Ps4MemoryLog("file-map chunk", saved_errno, chunk_length, "guest view");
        return nullptr;
      }
      if ((offset % (256ULL * 1024ULL * 1024ULL)) == 0 || offset + chunk_length == length) {
        Ps4MemoryMapProgress("file-map chunk complete", file_offset + offset, chunk_length);
      }
      offset += chunk_length;
    }
    return base_address;
  }
#endif
  void* result = mmap(base_address, length, prot, flags, static_cast<int>(handle),
                      static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex

#if REX_PLATFORM_PS4
namespace rex {
// Free direct memory (largest free extent and total size) for the renderer's
// memory log. The SDK's prototype takes the out-params by value; call through
// the real signature.
void Ps4DirectMemoryStatus(size_t* largest_free, size_t* total) {
  using Fn = int32_t (*)(off_t, off_t, size_t, off_t*, size_t*);
  const auto available = reinterpret_cast<Fn>(&sceKernelAvailableDirectMemorySize);
  off_t start = 0;
  size_t size = 0;
  if (available(0, off_t(SCE_KERNEL_MAIN_DMEM_SIZE), 16 * 1024, &start, &size) != 0) size = 0;
  *largest_free = size;
  *total = SCE_KERNEL_MAIN_DMEM_SIZE;
}
}  // namespace rex

// ---------------------------------------------------------------------------
// Large C++ allocations in direct memory.
//
// malloc and every thread stack come out of the title's flexible memory, a few
// hundred MiB on the PS4. The native renderer keeps CPU copies of buffer,
// index and texture payloads in std::vectors, and those exhausted it (LXXIV:
// std::bad_alloc in UploadBufferResource with ~870 MiB of direct memory still
// free). operator new therefore serves allocations of kLargeAllocMin bytes
// and up from their own direct-memory mappings, falling back to malloc when
// direct memory is refused. Freed mappings are kept in a small cache by size,
// because payload vectors of the same sizes come and go every frame.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <exception>
#include <map>
#include <new>
#include <stdlib.h>

namespace {
constexpr size_t kLargeAllocMin = 128 * 1024;
constexpr size_t kLargeAllocPage = 16 * 1024;
constexpr size_t kLargeCacheLimit = 96ull * 1024 * 1024;

struct LargeBlock {
  off_t offset;
  size_t bytes;
};
// The bookkeeping never allocates (LXXXIV: once musl refused everything, the
// std::unordered_map node for a rescued block could not be allocated either,
// so the rescue failed and the game aborted): fixed tables in .bss.
// Live blocks: open addressing, linear probing, backward-shift deletion.
class LiveTable {
 public:
  static constexpr size_t kCapacity = size_t(1) << 16;
  // false: full (kept under 7/8, so probes stay short).
  bool Insert(uintptr_t address, LargeBlock block) {
    if (count_ >= kCapacity / 8 * 7) return false;
    size_t i = Home(address);
    while (slots_[i].address && slots_[i].address != address) i = (i + 1) & kMask;
    if (!slots_[i].address) ++count_;
    slots_[i] = {address, block};
    return true;
  }
  const LargeBlock* Find(uintptr_t address) const {
    for (size_t i = Home(address); slots_[i].address; i = (i + 1) & kMask)
      if (slots_[i].address == address) return &slots_[i].block;
    return nullptr;
  }
  bool Erase(uintptr_t address, LargeBlock* out) {
    size_t i = Home(address);
    while (slots_[i].address != address) {
      if (!slots_[i].address) return false;
      i = (i + 1) & kMask;
    }
    *out = slots_[i].block;
    for (size_t j = (i + 1) & kMask; slots_[j].address; j = (j + 1) & kMask) {
      const size_t k = Home(slots_[j].address);
      // Move j back into the hole unless its home lies cyclically in (i, j].
      if ((j > i && (k <= i || k > j)) || (j < i && k <= i && k > j)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].address = 0;
    --count_;
    return true;
  }

 private:
  static constexpr size_t kMask = kCapacity - 1;
  static size_t Home(uintptr_t address) {
    return size_t((uint64_t(address) >> 14) * 0x9E3779B97F4A7C15ull >> 48) & kMask;
  }
  struct Slot {
    uintptr_t address;  // 0: empty
    LargeBlock block;
  };
  Slot slots_[kCapacity]{};
  size_t count_ = 0;
};
// Freed blocks kept for reuse (kLargeCacheLimit bytes at most).
struct CacheEntry {
  size_t bytes;
  uintptr_t address;
  off_t offset;
};
constexpr size_t kCacheEntries = 2048;
struct LargeAllocState {
  std::mutex mutex;
  LiveTable live;
  CacheEntry cache[kCacheEntries]{};
  size_t cache_count = 0;
  size_t cache_bytes = 0;
};
// Constant-initialized (.bss): usable before and after static construction.
constinit LargeAllocState g_large_state;
LargeAllocState& LargeState() { return g_large_state; }
std::atomic<uintptr_t> g_large_lo{UINTPTR_MAX};
std::atomic<uintptr_t> g_large_hi{0};
std::atomic<size_t> g_large_live_bytes{0};
std::atomic<size_t> g_large_cache_bytes{0};
std::atomic<uint64_t> g_large_failures{0};
// Live blocks mapped from flexible memory (part of g_large_live_bytes).
std::atomic<size_t> g_large_flexible_bytes{0};
constexpr size_t kFlexibleReserve = 64ull * 1024 * 1024;
// Set while the large path itself allocates (its maps), so those take malloc.
thread_local bool t_in_large_alloc = false;

// Which 16 KiB pages start a live block, readable without the mutex: free and
// delete ask this first, so malloc's own frees (musl's heap is mapped among
// these blocks, inside lo..hi) no longer take the mutex. One bit per page in
// 16 MiB chunks, up to 2^40; chunk bitmaps come from a static pool, set under
// the state mutex. Out of range or out of pool: every pointer is "maybe".
constexpr unsigned kPageShift = 14;
constexpr unsigned kChunkPageShift = 10;  // 1024 pages = 16 MiB
constexpr size_t kChunkWords = (size_t(1) << kChunkPageShift) / 64;
constexpr size_t kChunkCount = size_t(1) << (40 - kPageShift - kChunkPageShift);
constexpr size_t kPoolChunks = 4096;
std::atomic<std::atomic<uint64_t>*> g_block_chunks[kChunkCount];
std::atomic<uint64_t> g_block_pool[kPoolChunks * kChunkWords];
size_t g_block_pool_used = 0;  // (state mutex)
std::atomic<bool> g_block_bits_overflow{false};

// Under the state mutex.
void MarkLargeBlock(uintptr_t address, bool live) {
  const uint64_t page = address >> kPageShift;
  const uint64_t chunk = page >> kChunkPageShift;
  if (chunk >= kChunkCount) {
    g_block_bits_overflow.store(true, std::memory_order_relaxed);
    return;
  }
  std::atomic<uint64_t>* bits = g_block_chunks[chunk].load(std::memory_order_acquire);
  if (!bits) {
    if (!live) return;
    if (g_block_pool_used >= kPoolChunks) {
      g_block_bits_overflow.store(true, std::memory_order_relaxed);
      return;
    }
    bits = &g_block_pool[g_block_pool_used++ * kChunkWords];
    g_block_chunks[chunk].store(bits, std::memory_order_release);
  }
  const uint64_t index = page & ((uint64_t(1) << kChunkPageShift) - 1);
  const uint64_t mask = uint64_t(1) << (index & 63);
  if (live) {
    bits[index >> 6].fetch_or(mask, std::memory_order_release);
  } else {
    bits[index >> 6].fetch_and(~mask, std::memory_order_release);
  }
}

bool MaybeLargeBlock(uintptr_t address) {
  if (address & ((uintptr_t(1) << kPageShift) - 1)) return false;  // blocks are page aligned
  if (g_block_bits_overflow.load(std::memory_order_relaxed)) return true;
  const uint64_t page = address >> kPageShift;
  const uint64_t chunk = page >> kChunkPageShift;
  if (chunk >= kChunkCount) return true;
  const std::atomic<uint64_t>* bits = g_block_chunks[chunk].load(std::memory_order_acquire);
  if (!bits) return false;
  const uint64_t index = page & ((uint64_t(1) << kChunkPageShift) - 1);
  return bits[index >> 6].load(std::memory_order_acquire) & (uint64_t(1) << (index & 63));
}

void* LargeAllocateImpl(size_t size, bool rescue) {
  const size_t bytes = (size + kLargeAllocPage - 1) & ~(kLargeAllocPage - 1);
  auto& state = LargeState();
  t_in_large_alloc = true;
  void* result = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    for (size_t i = 0; i < state.cache_count; ++i) {
      const CacheEntry entry = state.cache[i];
      if (entry.bytes != bytes || !state.live.Insert(entry.address, {entry.offset, bytes})) continue;
      MarkLargeBlock(entry.address, true);
      state.cache[i] = state.cache[--state.cache_count];
      state.cache_bytes -= bytes;
      g_large_cache_bytes.store(state.cache_bytes, std::memory_order_relaxed);
      result = reinterpret_cast<void*>(entry.address);
      break;
    }
  }
  if (!result) {
    // A rescue (malloc refused it) takes flexible memory first (LXXXI: musl's
    // malloc refused 22 KB with 114 MB of the flexible pool free, while the
    // largest free direct block was 5 MB and the GPU's GARLIC textures need
    // direct memory). Only a rescue: LXXXIII took it for every large block,
    // used the pool down to the reserve while the world loaded, and the game
    // could no longer create threads (their stacks come from it). Keep
    // kFlexibleReserve for stacks and libc. offset -1 marks a flexible block.
    size_t flexible_free = 0;
    using AvailableFn = int32_t (*)(size_t*);
    if (rescue &&
        reinterpret_cast<AvailableFn>(&sceKernelAvailableFlexibleMemorySize)(&flexible_free) == 0 &&
        flexible_free >= bytes + kFlexibleReserve) {
      void* mapped = nullptr;
      if (sceKernelMapNamedFlexibleMemory(&mapped, bytes,
                                          SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE, 0,
                                          "LR_FlexHeap") == 0 &&
          mapped) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(mapped);
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.live.Insert(address, {off_t(-1), bytes})) {
          lock.unlock();
          sceKernelMunmap(mapped, bytes);
          g_large_failures.fetch_add(1, std::memory_order_relaxed);
          t_in_large_alloc = false;
          return nullptr;
        }
        MarkLargeBlock(address, true);
        uintptr_t lo = g_large_lo.load(std::memory_order_relaxed);
        while (address < lo && !g_large_lo.compare_exchange_weak(lo, address)) {}
        uintptr_t hi = g_large_hi.load(std::memory_order_relaxed);
        while (address + bytes > hi && !g_large_hi.compare_exchange_weak(hi, address + bytes)) {}
        g_large_flexible_bytes.fetch_add(bytes, std::memory_order_relaxed);
        result = mapped;
      }
    }
  }
  if (!result) {
    off_t offset = 0;
    void* mapped = nullptr;
    if (sceKernelAllocateDirectMemory(0, SCE_KERNEL_MAIN_DMEM_SIZE, bytes, kLargeAllocPage,
                                      SCE_KERNEL_WB_ONION, &offset) < 0) {
      g_large_failures.fetch_add(1, std::memory_order_relaxed);
    } else if (sceKernelMapNamedDirectMemory(&mapped, bytes,
                                             SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE,
                                             0, offset, kLargeAllocPage, "LR_Heap") < 0 ||
               !mapped) {
      sceKernelReleaseDirectMemory(offset, bytes);
      g_large_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uintptr_t address = reinterpret_cast<uintptr_t>(mapped);
      std::unique_lock<std::mutex> lock(state.mutex);
      if (!state.live.Insert(address, {offset, bytes})) {
        lock.unlock();
        sceKernelMunmap(mapped, bytes);
        sceKernelReleaseDirectMemory(offset, bytes);
        g_large_failures.fetch_add(1, std::memory_order_relaxed);
        t_in_large_alloc = false;
        return nullptr;
      }
      MarkLargeBlock(address, true);
      uintptr_t lo = g_large_lo.load(std::memory_order_relaxed);
      while (address < lo && !g_large_lo.compare_exchange_weak(lo, address)) {}
      uintptr_t hi = g_large_hi.load(std::memory_order_relaxed);
      while (address + bytes > hi && !g_large_hi.compare_exchange_weak(hi, address + bytes)) {}
      result = mapped;
    }
  }
  t_in_large_alloc = false;
  if (result) g_large_live_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return result;
}

// force: any size (the rescues below, when malloc has refused it). The maps'
// own nodes come from malloc: if that throws, the flag must not stay set.
void* LargeAllocate(size_t size, bool force = false) {
  if ((size < kLargeAllocMin && !force) || t_in_large_alloc) return nullptr;
  try {
    return LargeAllocateImpl(size, force);
  } catch (...) {
    t_in_large_alloc = false;
    return nullptr;
  }
}

// The block's length if pointer starts one (0: malloc's).
size_t LargeBlockBytes(const void* pointer) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  if (address < g_large_lo.load(std::memory_order_relaxed) ||
      address >= g_large_hi.load(std::memory_order_relaxed) || t_in_large_alloc ||
      !MaybeLargeBlock(address)) {
    return 0;
  }
  auto& state = LargeState();
  std::lock_guard<std::mutex> lock(state.mutex);
  const LargeBlock* live = state.live.Find(address);
  return live ? live->bytes : 0;
}

bool LargeFree(void* pointer) {
  // Inside the large path (its maps rehashing), everything freed came from malloc,
  // and the state mutex is already held.
  if (t_in_large_alloc) return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  if (address < g_large_lo.load(std::memory_order_relaxed) ||
      address >= g_large_hi.load(std::memory_order_relaxed) || !MaybeLargeBlock(address)) {
    return false;
  }
  auto& state = LargeState();
  t_in_large_alloc = true;
  LargeBlock block{};
  bool release = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.live.Erase(address, &block)) {
      t_in_large_alloc = false;
      return false;
    }
    MarkLargeBlock(address, false);
    if (state.cache_bytes + block.bytes <= kLargeCacheLimit && state.cache_count < kCacheEntries) {
      state.cache[state.cache_count++] = {block.bytes, address, block.offset};
      state.cache_bytes += block.bytes;
      g_large_cache_bytes.store(state.cache_bytes, std::memory_order_relaxed);
    } else {
      release = true;
    }
  }
  if (release) {
    sceKernelMunmap(pointer, block.bytes);
    if (block.offset >= 0) sceKernelReleaseDirectMemory(block.offset, block.bytes);
  }
  if (block.offset < 0 && release)
    g_large_flexible_bytes.fetch_sub(block.bytes, std::memory_order_relaxed);
  t_in_large_alloc = false;
  g_large_live_bytes.fetch_sub(block.bytes, std::memory_order_relaxed);
  return true;
}

void DeleteAny(void* pointer) {
  if (!pointer) return;
  if (!LargeFree(pointer)) std::free(pointer);
}
}  // namespace

namespace rex {
void Ps4DirectMemoryStatus(size_t* largest_free, size_t* total);
}

namespace {
// Hands the reuse cache's direct memory back to the system.
void ReleaseLargeCache() {
  auto& state = LargeState();
  t_in_large_alloc = true;
  {
    // (unmapping under the mutex: these calls do not allocate)
    std::lock_guard<std::mutex> lock(state.mutex);
    for (size_t i = 0; i < state.cache_count; ++i) {
      const CacheEntry& entry = state.cache[i];
      sceKernelMunmap(reinterpret_cast<void*>(entry.address), entry.bytes);
      if (entry.offset >= 0) {
        sceKernelReleaseDirectMemory(entry.offset, entry.bytes);
      } else {
        g_large_flexible_bytes.fetch_sub(entry.bytes, std::memory_order_relaxed);
      }
    }
    state.cache_count = 0;
    state.cache_bytes = 0;
    g_large_cache_bytes.store(0, std::memory_order_relaxed);
  }
  t_in_large_alloc = false;
}

// malloc refused an allocation (LXXX: std::bad_alloc in the recorder, both
// pools running low as the city streamed in). Name it in klog, then try the
// other pool: direct memory, whatever the size, after giving back the cache.
void* RescueAllocate(size_t size, size_t alignment) {
  void* result = nullptr;
  if (alignment <= kLargeAllocPage && !t_in_large_alloc) {
    try {
      result = LargeAllocate(size, true);
      if (!result) {
        ReleaseLargeCache();
        result = LargeAllocate(size, true);
      }
    } catch (...) {
      t_in_large_alloc = false;  // (its map could not grow: malloc is out too)
      result = nullptr;
    }
  }
  static std::atomic<uint32_t> reports{0};
  const uint32_t report = reports.fetch_add(1, std::memory_order_relaxed);
  if (report < 24 || !(report % 1024)) {
    size_t flexible_free = 0, dmem_largest = 0, dmem_total = 0;
    using Fn = int32_t (*)(size_t*);
    reinterpret_cast<Fn>(&sceKernelAvailableFlexibleMemorySize)(&flexible_free);
    rex::Ps4DirectMemoryStatus(&dmem_largest, &dmem_total);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[PS4 oom] #%u malloc refused %zu bytes (align %zu); rescued=%d flexible-free=%zu KB "
                  "dmem-largest=%zu KB heap-live=%zu KB (flexible %zu KB) heap-failures=%llu\n",
                  report, size, alignment, result != nullptr, flexible_free >> 10, dmem_largest >> 10,
                  g_large_live_bytes.load(std::memory_order_relaxed) >> 10,
                  g_large_flexible_bytes.load(std::memory_order_relaxed) >> 10,
                  (unsigned long long)g_large_failures.load(std::memory_order_relaxed));
    sceKernelDebugOutText(0, line);
  }
  return result;
}
}  // namespace

namespace rex {
void Ps4LargeAllocStatus(size_t* live_bytes, size_t* cache_bytes, uint64_t* failures,
                         size_t* flexible_free) {
  *live_bytes = g_large_live_bytes.load(std::memory_order_relaxed);
  *cache_bytes = g_large_cache_bytes.load(std::memory_order_relaxed);
  *failures = g_large_failures.load(std::memory_order_relaxed);
  using Fn = int32_t (*)(size_t*);
  const auto flexible = reinterpret_cast<Fn>(&sceKernelAvailableFlexibleMemorySize);
  size_t free_bytes = 0;
  if (flexible(&free_bytes) != 0) free_bytes = 0;
  *flexible_free = free_bytes;
}
}  // namespace rex

namespace {
// An uncaught exception ends the process through std::terminate with nothing
// in the log (LXXIV); name it in klog first.
[[noreturn]] void LogAndAbort() {
  char line[320] = "[PS4 terminate] uncaught exception (unknown type)\n";
  if (std::exception_ptr current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      size_t flexible_free = 0;
      using Fn = int32_t (*)(size_t*);
      reinterpret_cast<Fn>(&sceKernelAvailableFlexibleMemorySize)(&flexible_free);
      std::snprintf(line, sizeof(line),
                    "[PS4 terminate] uncaught %s; flexible-free=%zuKB dmem-heap=%zuKB\n", e.what(),
                    flexible_free >> 10, g_large_live_bytes.load(std::memory_order_relaxed) >> 10);
    } catch (...) {
    }
  }
  sceKernelDebugOutText(0, line);
  std::abort();
}
struct TerminateLogger {
  TerminateLogger() { std::set_terminate(LogAndAbort); }
} g_terminate_logger;
}  // namespace

// operator new is the OpenOrbis compat library's (it logs running out of
// memory); gta4 links with --wrap for these eight, so the large path sees each
// allocation first and hands the rest to the compat version. (Weak: the rexglue
// tool links this file without the wraps and never calls these.)
extern "C" {
[[gnu::weak]] void* __real__Znwm(size_t);
[[gnu::weak]] void* __real__Znam(size_t);
[[gnu::weak]] void* __real__ZnwmRKSt9nothrow_t(size_t, const std::nothrow_t&) noexcept;
[[gnu::weak]] void* __real__ZnamRKSt9nothrow_t(size_t, const std::nothrow_t&) noexcept;
[[gnu::weak]] void* __real__ZnwmSt11align_val_t(size_t, std::align_val_t);
[[gnu::weak]] void* __real__ZnamSt11align_val_t(size_t, std::align_val_t);
[[gnu::weak]] void* __real__ZnwmSt11align_val_tRKSt9nothrow_t(size_t, std::align_val_t,
                                                const std::nothrow_t&) noexcept;
[[gnu::weak]] void* __real__ZnamSt11align_val_tRKSt9nothrow_t(size_t, std::align_val_t,
                                                const std::nothrow_t&) noexcept;

void* __wrap__Znwm(size_t size) {
  if (void* large = LargeAllocate(size)) return large;
  if (void* small = __real__ZnwmRKSt9nothrow_t(size, std::nothrow)) return small;
  if (void* rescued = RescueAllocate(size, 0)) return rescued;
  throw std::bad_alloc();
}
void* __wrap__Znam(size_t size) {
  if (void* large = LargeAllocate(size)) return large;
  if (void* small = __real__ZnamRKSt9nothrow_t(size, std::nothrow)) return small;
  if (void* rescued = RescueAllocate(size, 0)) return rescued;
  throw std::bad_alloc();
}
void* __wrap__ZnwmRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept {
  if (void* large = LargeAllocate(size)) return large;
  return __real__ZnwmRKSt9nothrow_t(size, tag);
}
void* __wrap__ZnamRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept {
  if (void* large = LargeAllocate(size)) return large;
  return __real__ZnamRKSt9nothrow_t(size, tag);
}
void* __wrap__ZnwmSt11align_val_t(size_t size, std::align_val_t alignment) {
  if (static_cast<size_t>(alignment) <= kLargeAllocPage)
    if (void* large = LargeAllocate(size)) return large;
  if (void* small = __real__ZnwmSt11align_val_tRKSt9nothrow_t(size, alignment, std::nothrow))
    return small;
  if (void* rescued = RescueAllocate(size, static_cast<size_t>(alignment))) return rescued;
  throw std::bad_alloc();
}
void* __wrap__ZnamSt11align_val_t(size_t size, std::align_val_t alignment) {
  if (static_cast<size_t>(alignment) <= kLargeAllocPage)
    if (void* large = LargeAllocate(size)) return large;
  if (void* small = __real__ZnamSt11align_val_tRKSt9nothrow_t(size, alignment, std::nothrow))
    return small;
  if (void* rescued = RescueAllocate(size, static_cast<size_t>(alignment))) return rescued;
  throw std::bad_alloc();
}
void* __wrap__ZnwmSt11align_val_tRKSt9nothrow_t(size_t size, std::align_val_t alignment,
                                                const std::nothrow_t& tag) noexcept {
  if (static_cast<size_t>(alignment) <= kLargeAllocPage)
    if (void* large = LargeAllocate(size)) return large;
  return __real__ZnwmSt11align_val_tRKSt9nothrow_t(size, alignment, tag);
}
void* __wrap__ZnamSt11align_val_tRKSt9nothrow_t(size_t size, std::align_val_t alignment,
                                                const std::nothrow_t& tag) noexcept {
  if (static_cast<size_t>(alignment) <= kLargeAllocPage)
    if (void* large = LargeAllocate(size)) return large;
  return __real__ZnamSt11align_val_tRKSt9nothrow_t(size, alignment, tag);
}
}  // extern "C"

// The C allocator too (LXXXI: the driver's own malloc got NULL and the game
// aborted, 23 operator-new refusals after the first). musl's heap is carved
// from direct memory in 128 MiB pieces; once no free direct block is that
// large it refuses even 22 KB. A refusal goes to RescueAllocate; blocks it
// hands out are recognised by address on free and realloc.
extern "C" {
[[gnu::weak]] void* __real_malloc(size_t);
[[gnu::weak]] void* __real_calloc(size_t, size_t);
[[gnu::weak]] void* __real_realloc(void*, size_t);
[[gnu::weak]] void __real_free(void*);
[[gnu::weak]] void* __real_aligned_alloc(size_t, size_t);
[[gnu::weak]] int __real_posix_memalign(void**, size_t, size_t);
[[gnu::weak]] size_t __real_malloc_usable_size(void*);

// musl's __memalign (behind aligned_alloc and posix_memalign) calls malloc and
// then edits the chunk header in front of what it got: a rescued block has
// none. While those run, malloc's refusal is passed through and the outer
// wrapper rescues instead.
thread_local bool t_in_real_memalign = false;
void* __wrap_malloc(size_t size) {
  if (void* p = __real_malloc(size)) return p;
  return size && !t_in_real_memalign ? RescueAllocate(size, 0) : nullptr;
}
void* __wrap_calloc(size_t count, size_t size) {
  if (void* p = __real_calloc(count, size)) return p;
  size_t bytes = 0;
  if (__builtin_mul_overflow(count, size, &bytes) || !bytes) return nullptr;
  void* p = RescueAllocate(bytes, 0);
  if (p) std::memset(p, 0, bytes);  // (a reused block is not zero)
  return p;
}
void __wrap_free(void* pointer) {
  if (!pointer || LargeFree(pointer)) return;
  __real_free(pointer);
}
void* __wrap_realloc(void* pointer, size_t size) {
  if (const size_t old_bytes = pointer ? LargeBlockBytes(pointer) : 0) {
    if (!size) {
      LargeFree(pointer);
      return nullptr;
    }
    void* moved = __wrap_malloc(size);
    if (!moved) return nullptr;
    std::memcpy(moved, pointer, std::min(old_bytes, size));
    LargeFree(pointer);
    return moved;
  }
  if (void* p = __real_realloc(pointer, size)) return p;
  if (!size) return nullptr;
  void* moved = RescueAllocate(size, 0);
  if (moved && pointer) {
    std::memcpy(moved, pointer, std::min(__real_malloc_usable_size(pointer), size));
    __real_free(pointer);
  }
  return moved;
}
size_t __wrap_malloc_usable_size(void* pointer) {
  if (const size_t bytes = pointer ? LargeBlockBytes(pointer) : 0) return bytes;
  return __real_malloc_usable_size(pointer);
}
void* __wrap_aligned_alloc(size_t alignment, size_t size) {
  t_in_real_memalign = true;
  void* const real = __real_aligned_alloc(alignment, size);
  t_in_real_memalign = false;
  if (real) return real;
  return size ? RescueAllocate(size, alignment) : nullptr;
}
int __wrap_posix_memalign(void** out, size_t alignment, size_t size) {
  t_in_real_memalign = true;
  const int result = __real_posix_memalign(out, alignment, size);
  t_in_real_memalign = false;
  if (result != ENOMEM || !size) return result;
  void* p = RescueAllocate(size, alignment);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}
}  // extern "C"

// Every operator delete is wrapped the same way (the extra arguments, a size,
// an alignment or nothrow, change nothing here): a direct-memory block goes
// back to the large path, anything else to free() as libc++'s would.
extern "C" {
void __wrap__ZdlPv(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPv(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdlPvm(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPvm(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdlPvRKSt9nothrow_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPvRKSt9nothrow_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdlPvSt11align_val_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPvSt11align_val_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdlPvmSt11align_val_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPvmSt11align_val_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdlPvSt11align_val_tRKSt9nothrow_t(void* pointer) noexcept { DeleteAny(pointer); }
void __wrap__ZdaPvSt11align_val_tRKSt9nothrow_t(void* pointer) noexcept { DeleteAny(pointer); }
}  // extern "C"
#endif
