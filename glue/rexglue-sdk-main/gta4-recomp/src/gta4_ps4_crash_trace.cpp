#include "gta4_ps4_crash_trace.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/chrono/clock.h>

// Per-event guest tracing opens, appends and closes the guest trace file for
// every event, which dominates load time. Off unless asked for in native.toml.
REXCVAR_DEFINE_BOOL(gta4_ps4_trace, false, "GTA IV/Diagnostics",
                    "PS4: write per-event world-step/crash/loading traces to the guest trace "
                    "(slow; also enables the stall watchdog)");

#if REX_PLATFORM_PS4
#include <orbis/libkernel.h>

#include <pthread.h>
#include <unistd.h>

#include <rex/ps5_diag.h>

namespace {

// [ps4 port] Startup hangs here are deep indirect-call chains, and chasing them
// one instrumented frame per package is slow. Every guest function is a real
// host symbol, so when world-step progress stops we signal the registered
// threads and dump their stacks; the addresses are logged relative to
// rex::ps5diag::Anchor() and symbolized offline against gta4.oelf.
std::atomic<uint64_t> g_world_step_progress{0};

// The rex log rotates every 5 MB and this title fills that in seconds, so a
// stack dump written there is recycled before it can be fetched. Mirror
// ps5diag's lines into the guest trace, which is append-only and pulled
// incrementally off FTP.
void Gta4Ps4DumpContendedLocks();

void Gta4Ps4TraceLine(const char* text) {
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(text, log);
    std::fputc('\n', log);
    std::fflush(log);
    std::fclose(log);
  }
}

void* Gta4Ps4StallWatchdog(void*) {
  rex::ps5diag::RegisterThread("gta4-stall-watchdog");
  uint64_t last = ~uint64_t(0);
  int quiet_seconds = 0;
  int dumps = 0;
  for (;;) {
    sleep(1);
    const uint64_t now = g_world_step_progress.load(std::memory_order_relaxed);
    if (now != last) {
      last = now;
      quiet_seconds = 0;
      continue;
    }
    // A healthy resource stage can be quiet for a while: 25 s produced four
    // dumps on a run that then carried on normally and left none for the
    // actual stall. Wait longer, and keep dumping for as long as it is frozen.
    if (++quiet_seconds < 90) continue;
    quiet_seconds = 60;  // re-dump every 30 s while still frozen
    ++dumps;
    char banner[160];
    std::snprintf(banner, sizeof(banner),
                  "[PS4 stall] no world-step progress for 90s at event %llu; "
                  "stack dump %d follows\n",
                  static_cast<unsigned long long>(now), dumps);
    if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
      std::fputs(banner, log);
      std::fflush(log);
      std::fclose(log);
    }
    sceKernelDebugOutText(0, banner);
    rex::ps5diag::SetTraceSink(&Gta4Ps4TraceLine);
    rex::ps5diag::SampleAllThreads();
    Gta4Ps4DumpContendedLocks();
  }
  return nullptr;
}

// X_RTL_CRITICAL_SECTION: lock_count @0x10 (host endian), recursion_count
// @0x14 and owning_thread @0x18, both big-endian guest words.
void Gta4Ps4DumpContendedLocks() {
  char line[256];
  int reported = 0;
  for (int i = 0; i < rex::ps5diag::kWaitSlots; ++i) {
    rex::ps5diag::WaitSlot& w = rex::ps5diag::wait_slots[i];
    const void* cs = w.critical_section;
    if (!cs) continue;
    const uint8_t* raw = static_cast<const uint8_t*>(cs);
    uint32_t be_recursion = 0, be_owner = 0;
    int32_t lock_count = 0;
    std::memcpy(&lock_count, raw + 0x10, 4);
    std::memcpy(&be_recursion, raw + 0x14, 4);
    std::memcpy(&be_owner, raw + 0x18, 4);
    const uint32_t recursion = __builtin_bswap32(be_recursion);
    const uint32_t owner = __builtin_bswap32(be_owner);
    std::snprintf(line, sizeof(line),
                  "[PS4 lock] waiter tid=%08X lr=%08X waiting on cs guest=%08X "
                  "entered-at=%08X | owner tid=%08X lock_count=%d recursion=%u",
                  w.thread_id, w.guest_lr, w.critical_section_guest,
                  w.critical_section_lr, owner, lock_count, recursion);
    Gta4Ps4TraceLine(line);
    ++reported;
  }
  if (!reported) Gta4Ps4TraceLine("[PS4 lock] no contended critical section recorded");
}

void Gta4Ps4NoteWorldStepProgress() {
  g_world_step_progress.fetch_add(1, std::memory_order_relaxed);
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    // UnhandledHostFault() appends its crash line to ps5diag's log path, which
    // only gets set on PS5; its fallback is /app0/logs/crash.log and /app0 is
    // the read-only package mount, so on PS4 the report would go nowhere.
    // Send it to the append-only guest trace we actually collect.
    rex::ps5diag::SetLogPath("/data/LibertyRecomp/logs/gta4-guest-trace.txt");
    rex::ps5diag::SetTraceSink(&Gta4Ps4TraceLine);
    pthread_t t{};
    if (pthread_create(&t, nullptr, Gta4Ps4StallWatchdog, nullptr) == 0) {
      pthread_detach(t);
    }
  }
}

}  // namespace
#endif

void GTA4_Ps4CrashTrace(const char* function, const PPCContext& ctx,
                        const uint8_t* base) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<unsigned> count{0};
  const unsigned n = count.fetch_add(1, std::memory_order_relaxed);
  const bool stack_target = std::strcmp(function, "sub_828CACA0-entry") == 0 ||
                            std::strcmp(function, "sub_828CACE8-entry") == 0;
  const bool worker_target = std::strncmp(function, "worker-thread-", 14) == 0;
  const bool graphics_target = std::strncmp(function, "sub_82167440-", 13) == 0;
  const bool lookup_fault_target =
      std::strncmp(function, "sub_82579A10-", 13) == 0;
  const bool property_fault_target =
      std::strncmp(function, "sub_8257AD68-", 13) == 0;
  const bool getter_fault_target =
      std::strncmp(function, "sub_82579910-", 13) == 0;
  const bool route_fault_target =
      std::strncmp(function, "sub_82577A58-", 13) == 0 ||
      std::strncmp(function, "sub_825789B8-", 13) == 0;
  const bool metric_fault_target =
      std::strncmp(function, "sub_825FA050-", 13) == 0;
  const bool parse_fault_target =
      std::strncmp(function, "sub_82512EE0-", 13) == 0;
  const bool allocator_free_target =
      std::strncmp(function, "sub_82848B68-", 13) == 0;
  const bool allocator_low_target =
      std::strcmp(function, "sub_82848B68-low-entry") == 0 ||
      std::strcmp(function, "sub_82848B68-low-memset") == 0;
  const bool dirty_disc_target = std::strncmp(function, "dirty-disc-", 11) == 0;
  const bool target = stack_target ||
                      std::strcmp(function, "sub_828CA8F8-entry") == 0 ||
                      std::strcmp(function, "sub_828D8440-entry") == 0 ||
                      std::strcmp(function, "sub_828D8440-call1-return") == 0 ||
                      std::strcmp(function, "sub_828D8440-call2-return") == 0 ||
                      std::strcmp(function, "sub_828D85C8-entry") == 0 ||
                      std::strcmp(function, "sub_82849940-entry") == 0 ||
                      std::strcmp(function, "sub_82A18620-entry") == 0 ||
                      std::strcmp(function, "sub_82145420-entry") == 0 ||
                      std::strcmp(function, "sub_82145420-exit") == 0 ||
                      std::strcmp(function, "sub_82142230-entry") == 0 ||
                      std::strcmp(function, "sub_82142230-exit") == 0 ||
                      std::strcmp(function, "sub_82144708-entry") == 0 ||
                      dirty_disc_target ||
                      std::strncmp(function, "sub_82579A10-", 13) == 0 ||
                      std::strncmp(function, "sub_8257AD68-", 13) == 0;
  static std::atomic<unsigned> target_count{0};
  static std::atomic<unsigned> stack_target_count{0};
  static std::atomic<unsigned> worker_target_count{0};
  static std::atomic<unsigned> graphics_target_count{0};
  static std::atomic<unsigned> lookup_fault_target_count{0};
  static std::atomic<unsigned> property_fault_target_count{0};
  static std::atomic<unsigned> getter_fault_target_count{0};
  static std::atomic<unsigned> route_fault_target_count{0};
  static std::atomic<unsigned> metric_fault_target_count{0};
  static std::atomic<unsigned> parse_fault_target_count{0};
  static std::atomic<unsigned> allocator_free_target_count{0};
  if (worker_target) {
    if (worker_target_count.fetch_add(1, std::memory_order_relaxed) >= 96) return;
  } else if (graphics_target) {
    if (graphics_target_count.fetch_add(1, std::memory_order_relaxed) >= 96) return;
  } else if (stack_target) {
    if (stack_target_count.fetch_add(1, std::memory_order_relaxed) >= 32) return;
  } else if (lookup_fault_target) {
    if (lookup_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 1024) return;
  } else if (property_fault_target) {
    if (property_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 512) return;
  } else if (getter_fault_target) {
    if (getter_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 1024) return;
  } else if (route_fault_target) {
    if (route_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 128) return;
  } else if (parse_fault_target) {
    if (parse_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 1024) return;
  } else if (allocator_free_target) {
    if (!allocator_low_target) {
      const unsigned sample = allocator_free_target_count.fetch_add(1, std::memory_order_relaxed);
      if (sample >= 64 && (sample & 0xFFu) != 0) return;
    }
  } else if (metric_fault_target) {
    if (metric_fault_target_count.fetch_add(1, std::memory_order_relaxed) >= 8192) return;
  } else if (target) {
    if (target_count.fetch_add(1, std::memory_order_relaxed) >= 32) return;
  } else if (n >= 16) {
    return;
  }

  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 crash trace] %s n=%u r3=%08x r4=%08x r5=%08x "
                "r1=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x "
                "r11=%08x r12=%08x r13=%08x lr=%08x base=%p\n",
                function, n, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r1.u32, ctx.r6.u32,
                ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                ctx.r11.u32, ctx.r12.u32, ctx.r13.u32,
                static_cast<uint32_t>(ctx.lr), base);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)function;
  (void)ctx;
  (void)base;
#endif
}

void GTA4_Ps4GraphicsCheckpoint(const char* function, const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) >= 128) return;
  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 graphics checkpoint] %s r3=%08x r4=%08x r5=%08x "
                "r1=%08x lr=%08x\n",
                function, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32,
                static_cast<uint32_t>(ctx.lr));
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)function;
  (void)ctx;
#endif
}

void GTA4_Ps4LoadingStateTrace(const char* phase, uint32_t loading_active,
                               uint32_t screen_index, uint32_t ready,
                               uint32_t complete, uint32_t audio_gate,
                               uint32_t frontend_state) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<uint32_t> count{0};
  static std::atomic<uint64_t> last_state{UINT64_MAX};
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
  const bool boundary_event = phase &&
                              (std::strncmp(phase, "world", 5) == 0 ||
                               std::strncmp(phase, "setter", 6) == 0 ||
                               std::strncmp(phase, "workflow", 8) == 0);
  const uint64_t state = uint64_t(loading_active & 1u) |
      (uint64_t(screen_index & 0xffu) << 1) | (uint64_t(ready & 1u) << 9) |
      (uint64_t(complete & 1u) << 10) | (uint64_t(audio_gate & 1u) << 11) |
      (uint64_t(frontend_state) << 12);
  const bool changed = last_state.exchange(state, std::memory_order_relaxed) != state;
  // Per-frame file open/flush/close plus kernel debug output throttled world
  // startup severely. Keep every state transition and an unlimited periodic
  // heartbeat, without doing synchronous logging for every rendered frame.
  if (!changed && !boundary_event && (n & 0x3ffu) != 0)
    return;
  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 loading state] n=%u phase=%s active=%u screen=%u "
                "ready=%u complete=%u audio_gate=%u frontend=%08x\n",
                n, phase, loading_active, screen_index, ready, complete,
                audio_gate, frontend_state);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)phase;
  (void)loading_active;
  (void)screen_index;
  (void)ready;
  (void)complete;
  (void)audio_gate;
  (void)frontend_state;
#endif
}

void GTA4_Ps4TransitionDispatchTrace(const char* phase, uint32_t caller,
                                     uint32_t stored_state,
                                     const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<uint32_t> emitted{0};
  static std::atomic<uint32_t> last_after_state{UINT32_MAX};
  const uint32_t event = emitted.load(std::memory_order_relaxed);
  const bool before_sample = phase && std::strcmp(phase, "before") == 0 && event < 8;
  const bool after_change = phase && std::strcmp(phase, "after") == 0 &&
      last_after_state.exchange(stored_state, std::memory_order_relaxed) != stored_state;
  if (!before_sample && !after_change) return;
  const uint32_t n = emitted.fetch_add(1, std::memory_order_relaxed);
  if (n >= 64) return;
  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 transition dispatch] n=%u phase=%s caller=%08x state=%08x r3=%08x r4=%08x r5=%08x\n",
                n, phase, caller, stored_state, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)phase;
  (void)caller;
  (void)stored_state;
  (void)ctx;
#endif
}

void GTA4_Ps4WorldStepTrace(const char* step, const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<uint32_t> count{0};
  static std::atomic<uint32_t> critical_count{0};
  static std::atomic<uint32_t> parser_count{0};
  static std::atomic<uint32_t> activation_count{0};
  static std::atomic<uint32_t> setup_count{0};
  static std::atomic<uint32_t> resource_count{0};
  // Keep categories separately numbered for easier reading, but do not cap
  // any category: startup can take far more steps than a short diagnostic run.
  const bool critical = step &&
      (std::strstr(step, "sub_8256FDC0") || std::strstr(step, "sub_82145770"));
  const bool parser = step && std::strstr(step, "parser-loop-progress");
  // Preserve the outer sub_82141F00 call sequence independently of nested
  // parser calls, which otherwise exhaust the generic budget before it shows
  // how far world activation gets.
  const bool activation = step && std::strncmp(step, "begin:0x8214", 12) == 0;
  const bool activation_end = step && std::strncmp(step, "end:0x8214", 10) == 0;
  const bool activation_step = activation || activation_end;
  const bool setup_begin = step && std::strncmp(step, "begin-inner:0x821FD", 19) == 0;
  const bool setup_end = step && std::strncmp(step, "end-inner:0x821FD", 17) == 0;
  const bool setup_container = step &&
      (std::strstr(step, "sub_8221B2A8") ||
       std::strstr(step, "setup-register-") ||
       std::strstr(step, "sub_82570BC0") ||
       std::strstr(step, "sub_8256FFD8"));
  const bool resource_step = step && std::strstr(step, "world-resource-");
  const bool world_init_child = step &&
      (std::strncmp(step, "begin-child:0x82570", 19) == 0 ||
       std::strncmp(step, "end-child:0x82570", 17) == 0);
  const bool setup_step = setup_begin || setup_end || setup_container || world_init_child;
  const uint32_t n = resource_step
      ? resource_count.fetch_add(1, std::memory_order_relaxed)
      : parser
      ? parser_count.fetch_add(1, std::memory_order_relaxed)
      : critical
          ? critical_count.fetch_add(1, std::memory_order_relaxed)
          : activation_step
              ? activation_count.fetch_add(1, std::memory_order_relaxed)
              : setup_step
                  ? setup_count.fetch_add(1, std::memory_order_relaxed)
                  : count.fetch_add(1, std::memory_order_relaxed);
  // These traces are the only useful visibility into guest startup here.
  // Keep emitting until setup advances, even when it takes thousands of steps.
  char line[224];
  std::snprintf(line, sizeof(line),
                "[PS4 world step] %s n=%u %s lr=%08x r1=%08x r3=%08x r4=%08x r5=%08x\n",
                resource_step ? "resource" : parser ? "parser" : critical ? "critical" : activation_step ? "activation" : setup_step ? "setup" : "generic",
                n, step, static_cast<uint32_t>(ctx.lr), ctx.r1.u32,
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
  Gta4Ps4NoteWorldStepProgress();
#else
  (void)step;
  (void)ctx;
#endif
}

void GTA4_Ps4WorldPathTrace(const char* phase, uint32_t guest_path,
                            const uint8_t* base, const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  char path[192];
  size_t length = 0;
  if (base && guest_path >= 0x80000000u && guest_path < 0xF0000000u) {
    const uint8_t* source = base + guest_path;
    while (length + 1 < sizeof(path)) {
      const uint8_t ch = source[length];
      if (ch == 0) break;
      path[length] = ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?';
      ++length;
    }
  }
  path[length] = '\0';
  char line[384];
  std::snprintf(line, sizeof(line),
                "[PS4 world path] phase=%s ptr=%08x path=\"%s\" r3=%08x r4=%08x r5=%08x lr=%08x\n",
                phase ? phase : "?", guest_path, path, ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)phase;
  (void)guest_path;
  (void)base;
  (void)ctx;
#endif
}

void GTA4_Ps4TimebaseSpinTrace(const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<uint32_t> count{0};
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
  if (n >= 32) return;
  char line[192];
  std::snprintf(line, sizeof(line),
                "[PS4 timebase spin] n=%u guest=%016llx host=%016llx lr=%08x r1=%08x\n",
                n, static_cast<unsigned long long>(ctx.r11.u64),
                static_cast<unsigned long long>(rex::chrono::Clock::QueryHostTickCount()),
                static_cast<uint32_t>(ctx.lr), ctx.r1.u32);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)ctx;
#endif
}

bool GTA4_Ps4GuestStreamReadAllowed(uint32_t address, const char* site,
                                    const PPCContext& ctx) {
#if REX_PLATFORM_PS4
  // The stream buffer is expected to remain in the page containing the guest
  // stack. The previous crash read at the first address in the next page.
  const uint32_t stack_page_end = (ctx.r1.u32 + 0xffffu) & 0xffff0000u;
  const uint64_t next_page_end = static_cast<uint64_t>(stack_page_end) + 0x10000u;
  if (address < stack_page_end || static_cast<uint64_t>(address) >= next_page_end)
    return true;
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) < 32) {
    char line[224];
    std::snprintf(line, sizeof(line),
                  "[PS4 stream pointer guard] site=%s address=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x lr=%08x\n",
                  site, address, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.lr);
    if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
      std::fputs(line, log);
      std::fflush(log);
      std::fclose(log);
    }
    sceKernelDebugOutText(0, line);
  }
  return false;
#else
  (void)address;
  (void)site;
  (void)ctx;
  return true;
#endif
}

static void GTA4_Ps4GuestMemoryTraceImpl(uint32_t address, const char* operation,
                                        uint32_t r1, uint32_t r3, uint32_t lr,
                                        bool include_context) {
#if REX_PLATFORM_PS4
  static std::atomic<unsigned> count{0};
  // Focus on the recurring protected-page fault. The failing access can be
  // made through REX_RAW_ADDR/memcpy, so include raw and read operations here,
  // not only generated scalar stores.
  const bool target_access =
      (address >= 0x8285e900u && address < 0x8285e960u) ||
      (address >= 0x82513000u && address < 0x82514000u) ||
      (address >= 0x64617000u && address < 0x64618000u);
  const bool crash_page_access =
      (address >= 0x10a96000u && address < 0x10a97000u) ||
      (address >= 0x10ac1000u && address < 0x10ac2000u);
  const bool low_fault_page = address >= 0x3a000u && address < 0x3b000u;
  if (crash_page_access) {
    static std::atomic<unsigned> crash_page_count{0};
    if (crash_page_count.fetch_add(1, std::memory_order_relaxed) >= 128) return;
  } else if (low_fault_page) {
    static std::atomic<unsigned> low_fault_page_count{0};
    if (low_fault_page_count.fetch_add(1, std::memory_order_relaxed) >= 64) return;
  } else if (!target_access && count.fetch_add(1, std::memory_order_relaxed) >= 64) {
    return;
  }
  const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  char line[192];
  if (crash_page_access) {
    std::snprintf(line, sizeof(line),
                  "[PS4 crash-page access] op=%s address=%08x r1=%08x r3=%08x lr=%08x caller=%p\n",
                  operation, address, r1, r3, lr,
                  reinterpret_cast<void*>(caller));
  } else if (address >= 0x64617000u && address < 0x64618000u) {
    std::snprintf(line, sizeof(line),
                  "[PS4 invalid guest pointer] op=%s address=%08x caller=%p\n",
                  operation, address, reinterpret_cast<void*>(caller));
  } else if (include_context) {
    std::snprintf(line, sizeof(line),
                  "[PS4 low guest access] op=%s address=%08x r1=%08x r3=%08x lr=%08x caller=%p\n",
                  operation, address, r1, r3, lr,
                  reinterpret_cast<void*>(caller));
  } else {
    std::snprintf(line, sizeof(line),
                  "[PS4 low guest access] op=%s address=%08x caller=%p\n",
                  operation, address, reinterpret_cast<void*>(caller));
  }
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)address;
  (void)operation;
  (void)r1;
  (void)r3;
  (void)lr;
  (void)include_context;
#endif
}

void GTA4_Ps4GuestMemoryTrace(uint32_t address, const char* operation) {
  GTA4_Ps4GuestMemoryTraceImpl(address, operation, 0, 0, 0, false);
}

void GTA4_Ps4GuestMemoryTraceContext(uint32_t address, const char* operation,
                                    uint32_t r1, uint32_t r3, uint32_t lr) {
  GTA4_Ps4GuestMemoryTraceImpl(address, operation, r1, r3, lr, true);
}

__attribute__((noinline)) void GTA4_Ps4GuestMemoryTraceContextFull(
    uint32_t address, const char* operation, const PPCContext& ctx) {
#if REX_PLATFORM_PS4
  // The recurring fatal write is to guest VA 0x1010. Capture the complete
  // translated register file and native thread at that exact scalar access so
  // the PPC effective address can be reconstructed without guessing from LR.
  static std::atomic<unsigned> low_access_count{0};
  const unsigned sample = low_access_count.fetch_add(1, std::memory_order_relaxed);
  if (address != 0x1010u && sample >= 32) return;
  const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  char line[768];
  std::snprintf(
      line, sizeof(line),
      "[PS4 low guest full context] tid=%u host=%p op=%s address=%08x lr=%08x "
      "r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x "
      "r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x "
      "r16=%08x r17=%08x r18=%08x r19=%08x r20=%08x r21=%08x r22=%08x r23=%08x "
      "r24=%08x r25=%08x r26=%08x r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x\n",
      static_cast<unsigned>(scePthreadGetthreadid()), reinterpret_cast<void*>(caller),
      operation, address, static_cast<uint32_t>(ctx.lr), ctx.r0.u32, ctx.r1.u32,
      ctx.r2.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
      ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.r11.u32, ctx.r12.u32,
      ctx.r13.u32, ctx.r14.u32, ctx.r15.u32, ctx.r16.u32, ctx.r17.u32,
      ctx.r18.u32, ctx.r19.u32, ctx.r20.u32, ctx.r21.u32, ctx.r22.u32,
      ctx.r23.u32, ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32,
      ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32);
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)address;
  (void)operation;
  (void)ctx;
#endif
}

__attribute__((noinline)) void GTA4_Ps4GuestMemoryTraceContextSite(
    uint32_t address, const char* operation, const PPCContext& ctx,
    const char* file, unsigned line_number, const char* function) {
#if REX_PLATFORM_PS4
  if (address == 0x1010u) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "[PS4 low guest source] tid=%u address=%08x op=%s source=%s:%u function=%s\n",
                  static_cast<unsigned>(scePthreadGetthreadid()), address,
                  operation, file, line_number, function);
    if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
      std::fputs(line, log);
      std::fflush(log);
      std::fclose(log);
    }
    sceKernelDebugOutText(0, line);
  }
#else
  (void)file;
  (void)line_number;
  (void)function;
#endif
  GTA4_Ps4GuestMemoryTraceContextFull(address, operation, ctx);
}

void GTA4_Ps4GuestCallbackReadTrace(uint32_t address, uint32_t object,
                                    uint32_t target, const PPCContext& ctx) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  // Keep context for both unmapped pages observed in the startup callback
  // path. The caller logs immediately before dereferencing r30 + 32.
  const bool observed_page =
      (address >= 0x10a96000u && address < 0x10a97000u) ||
      (address >= 0x10ac1000u && address < 0x10ac2000u);
  if (!observed_page) return;
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) >= 32) return;
  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 callback-page context] read=0x%08x object=0x%08x "
                "target=0x%08x r1=0x%08x r3=0x%08x r30=0x%08x "
                "r31=0x%08x lr=0x%08x\n",
                address, object, target, ctx.r1.u32, ctx.r3.u32,
                ctx.r30.u32, ctx.r31.u32, static_cast<uint32_t>(ctx.lr));
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)address;
  (void)object;
  (void)target;
  (void)ctx;
#endif
}

bool GTA4_Ps4GuestReadGuardContext(uint32_t address, const char* operation,
                                   const PPCContext& ctx) {
#if REX_PLATFORM_PS4
  // This page contains the repeated startup read fault. Trace and suppress
  // generated reads from it across all widths to identify the next boundary.
  if (address < 0x10ac1000u || address >= 0x10ac2000u) return true;
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) < 64) {
    char line[224];
    std::snprintf(line, sizeof(line),
                  "[PS4 guest page guard] op=%s address=%08x r1=%08x r3=%08x "
                  "r30=%08x r31=%08x lr=%08x value=0\n",
                  operation, address, ctx.r1.u32, ctx.r3.u32, ctx.r30.u32,
                  ctx.r31.u32, static_cast<uint32_t>(ctx.lr));
    if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
      std::fputs(line, log);
      std::fflush(log);
      std::fclose(log);
    }
    sceKernelDebugOutText(0, line);
  }
  return false;
#else
  (void)address;
  (void)operation;
  (void)ctx;
  return true;
#endif
}

bool GTA4_Ps4GuestMemoryWriteAllowed(uint32_t address, const char* operation) {
#if REX_PLATFORM_PS4
  // Guest executable text is mapped read-only on PS4. A bad guest resource
  // handle previously attempted to decrement a refcount inside this region,
  // taking down the whole app before we could capture its caller context.
  if ((address >= 0x8285e000u && address < 0x82860000u) ||
      (address >= 0x82513000u && address < 0x82514000u)) {
    static std::atomic<unsigned> site_count{0};
    if (site_count.fetch_add(1, std::memory_order_relaxed) < 64) {
      const auto site = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
      uintptr_t host_stack_marker = 0;
      char line[160];
      std::snprintf(line, sizeof(line),
                    "[PS4 guest write site] op=%s address=%08x host=%p stack=%p\n",
                    operation, address, reinterpret_cast<void*>(site),
                    static_cast<void*>(&host_stack_marker));
      if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
        std::fputs(line, log);
        std::fflush(log);
        std::fclose(log);
      }
      sceKernelDebugOutText(0, line);
    }
    GTA4_Ps4GuestMemoryTraceContext(address, operation, 0, 0, 0);
    sceKernelDebugOutText(0, "[PS4 guest write guard] blocked write to executable page\n");
    return false;
  }
#else
  (void)address;
  (void)operation;
#endif
  return true;
}

bool GTA4_Ps4GuestMemoryWriteAllowedContext(uint32_t address,
                                            const char* operation,
                                            const PPCContext& ctx,
                                            uint32_t value) {
#if REX_PLATFORM_PS4
  if (address == 0x8285e930u) {
    static std::atomic<unsigned> context_count{0};
    if (context_count.fetch_add(1, std::memory_order_relaxed) < 16) {
      GTA4_Ps4GuestStoreContextTrace("code-page-write-guard-context", address,
                                     ctx, value);
    }
  }
  if (address >= 0x82513000u && address < 0x82514000u) {
    static std::atomic<unsigned> context_count{0};
    if (context_count.fetch_add(1, std::memory_order_relaxed) < 16) {
      GTA4_Ps4GuestStoreContextTrace("code-page-write-guard-context", address,
                                     ctx, value);
    }
  }
#else
  (void)ctx;
  (void)value;
#endif
  return GTA4_Ps4GuestMemoryWriteAllowed(address, operation);
}

void GTA4_Ps4GuestStoreTrace(const char* site, uint32_t address, uint32_t base,
                             uint32_t index, uint32_t value, uint32_t lr) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) >= 64) return;
  const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  char line[256];
  std::snprintf(line, sizeof(line),
                "[PS4 guest store] site=%s address=%08x base=%08x index=%08x "
                "value=%08x lr=%08x host=%p\n",
                site, address, base, index, value, lr, reinterpret_cast<void*>(caller));
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)site;
  (void)address;
  (void)base;
  (void)index;
  (void)value;
  (void)lr;
#endif
}

void GTA4_Ps4GuestStoreContextTrace(const char* site, uint32_t address,
                                   const PPCContext& ctx, uint32_t value) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  static std::atomic<unsigned> count{0};
  const unsigned n = count.fetch_add(1, std::memory_order_relaxed);
  const bool target = site &&
                      (std::strncmp(site, "sub_828B36D0-", 13) == 0 ||
                       std::strncmp(site, "sub_8284CB80-vcall", 17) == 0 ||
                       std::strncmp(site, "sub_8285E", 9) == 0 ||
                       std::strcmp(site, "sub_82579990-entry") == 0 ||
                       std::strcmp(site, "code-page-write-guard-context") == 0 ||
                       std::strcmp(site, "code-page-write-8285e930") == 0);
  // Keep enough early context to diagnose a bad store, but don't log every
  // stack update from the hot guest routines. That made a short loading run
  // produce tens of megabytes of duplicate register dumps.
  if ((target && n >= 64) || (!target && n >= 32)) return;
  const uint32_t regs[32] = {
      ctx.r0.u32,  ctx.r1.u32,  ctx.r2.u32,  ctx.r3.u32,  ctx.r4.u32,  ctx.r5.u32,
      ctx.r6.u32,  ctx.r7.u32,  ctx.r8.u32,  ctx.r9.u32,  ctx.r10.u32, ctx.r11.u32,
      ctx.r12.u32, ctx.r13.u32, ctx.r14.u32, ctx.r15.u32, ctx.r16.u32, ctx.r17.u32,
      ctx.r18.u32, ctx.r19.u32, ctx.r20.u32, ctx.r21.u32, ctx.r22.u32, ctx.r23.u32,
      ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32,
      ctx.r30.u32, ctx.r31.u32};
  char line[640];
  int length = std::snprintf(line, sizeof(line),
                             "[PS4 guest store context] site=%s address=%08x value=%08x lr=%08x",
                             site, address, value, static_cast<uint32_t>(ctx.lr));
  for (unsigned i = 0; i < 32 && length > 0 &&
                       static_cast<size_t>(length) < sizeof(line); ++i) {
    length += std::snprintf(line + length, sizeof(line) - static_cast<size_t>(length),
                            " r%u=%08x", i, regs[i]);
  }
  if (length > 0 && static_cast<size_t>(length) < sizeof(line) - 1) {
    uintptr_t host_stack_marker = 0;
    length += std::snprintf(line + length, sizeof(line) - static_cast<size_t>(length),
                            " host_stack=%p", static_cast<void*>(&host_stack_marker));
  }
  if (length > 0 && static_cast<size_t>(length) < sizeof(line) - 1) {
    line[length++] = '\n';
    line[length] = '\0';
  }
  if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
    std::fputs(line, log);
    std::fflush(log);
    std::fclose(log);
  }
  sceKernelDebugOutText(0, line);
#else
  (void)site;
  (void)address;
  (void)ctx;
  (void)value;
#endif
}

void GTA4_Ps4GuestCodePageWriteContextTrace(uint32_t address,
                                           const PPCContext& ctx,
                                           uint32_t value) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  if (address != 0x8285e930u) {
    return;
  }
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) < 8) {
    GTA4_Ps4GuestStoreContextTrace("code-page-write-8285e930", address, ctx,
                                   value);
  }
#else
  (void)address;
  (void)ctx;
  (void)value;
#endif
}

void GTA4_Ps4DeviceCreateTrace(const char* phase, uint32_t mode, uint32_t behavior,
                              uint32_t presentation, uint32_t output, uint32_t device,
                              uint32_t status, uint32_t lr) {
  if (!REXCVAR_GET(gta4_ps4_trace)) return;
#if REX_PLATFORM_PS4
  char line[224];
  std::snprintf(line, sizeof(line),
                "[PS4 device create] %s mode=%u behavior=%08x presentation=%08x "
                "output=%08x device=%08x status=%08x lr=%08x\n",
                phase, mode, behavior, presentation, output, device, status, lr);
  sceKernelDebugOutText(0, line);
#else
  (void)phase;
  (void)mode;
  (void)behavior;
  (void)presentation;
  (void)output;
  (void)device;
  (void)status;
  (void)lr;
#endif
}
