/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdio>
#include <vector>

#if REX_PLATFORM_PS4
#include <orbis/libkernel.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#if REX_PLATFORM_PS5 || REX_PLATFORM_PS4
#include <pthread.h>
#include <atomic>
#include <string>
#include <rex/frame_stats.h>
#if REX_PLATFORM_PS4
void Ps4RaiseAudioThreadPriority(const char* what);  // (audio_system.cpp)
#endif
#include <rex/logging.h>
#include <rex/ps5_diag.h>
#include <rex/system/thread_state.h>
#endif

namespace {
#if REX_PLATFORM_PS5
// [ps5 port] records this thread's kernel wait for the 10 s monitor
// (rex/ps5_diag.h); clears it when the wait ends.
struct Ps5WaitScope {
  rex::ps5diag::WaitSlot* slot;
  Ps5WaitScope(rex::system::XObject* first, uint32_t count, uint64_t* opt_timeout) {
    slot = rex::ps5diag::CurrentWaitSlot();
    if (!slot) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    slot->thread_id = thread ? thread->handle() : 0;  // (as the "thread ... (F80000xx)" log lines)
    slot->guest_lr = 0;
    slot->guest_sp = slot->guest_stack_lo = slot->guest_stack_hi = 0;
    if (thread && thread->thread_state() && thread->thread_state()->context()) {
      auto* ctx = thread->thread_state()->context();
      slot->guest_lr = uint32_t(ctx->lr);
      slot->guest_sp = uint32_t(ctx->r1.u32);
      slot->guest_stack_lo = thread->guest_stack_limit();
      slot->guest_stack_hi = thread->guest_stack_base();
    }
    slot->object_type = first ? uint32_t(first->type()) : 0;
    slot->handle = first ? first->handle() : 0;
    slot->count = count;
    slot->timeout_ms = opt_timeout ? 0 : 0xFFFFFFFFu;
    slot->since_ms.store(rex::ps5diag::NowMs(), std::memory_order_release);
  }
  ~Ps5WaitScope() {
    if (!slot) return;
    const uint64_t since = slot->since_ms.load(std::memory_order_relaxed);
    slot->since_ms.store(0, std::memory_order_release);
    // A long wait on the game's main thread (several frames' worth): what it
    // waited on and from where, to tell the hitches apart. A line at most
    // every 5 s, 100 in all (loading screens wait this long all the time).
    const uint64_t now = rex::ps5diag::NowMs();
    if (!since || now - since < 60 ||
        reinterpret_cast<uintptr_t>(pthread_self()) !=
            rex::frame_stats::g_game_main_thread.load(std::memory_order_relaxed)) {
      return;
    }
    static std::atomic<uint64_t> last_ms{0};
    static std::atomic<uint32_t> lines{0};
    uint64_t last = last_ms.load(std::memory_order_relaxed);
    if ((last && now - last < 5000) || !last_ms.compare_exchange_strong(last, now) ||
        lines.fetch_add(1, std::memory_order_relaxed) >= 100) {
      return;
    }
    REXLOG_INFO("ps5 hitch: the game's main thread waited {} ms on {} {} handle {:08X} from game "
                "code {:08X}{}",
                now - since, slot->count > 1 ? slot->count : 1,
                rex::ps5diag::ObjectTypeName(slot->object_type), slot->handle, slot->guest_lr,
                slot->critical_section
                    ? fmt::format(" (a critical section entered from {:08X})", slot->critical_section_lr)
                    : std::string());
  }
};
#elif REX_PLATFORM_PS4
// [ps4 port] Where the game's render thread and main thread wait, and for how
// long, by the guest code that waits (its LR) and the object's type: summed
// here, reported every 10 s (Ps4ReportGuestWaits, from the profiler thread).
// LXXXIX: every thread half idle, frames 85 ms, 720p no faster: the two
// threads take turns, and this names what each one waits for.
struct Ps4WaitSite {
  std::atomic<uint64_t> key{0};  // lr << 8 | object type (0: empty)
  std::atomic<uint64_t> us{0};
  std::atomic<uint32_t> count{0};
};
constexpr uint32_t kPs4WaitSites = 128;
// Roles: the game's render thread, its main thread, the audio worker (the
// game's mixer callback runs on it; XCVI: 15-18 ms a callback, two thirds of
// it blocked).
// C: the callback spends 99% of its time waiting for the game's mixing thread
// (sub_821909D0, idle wait returning to 82190A28): a fourth role.
constexpr uint32_t kPs4WaitRoles = 4;
constexpr uint32_t kAudioMixIdleWaitLr = 0x82190A28;
Ps4WaitSite g_ps4_wait_sites[kPs4WaitRoles][kPs4WaitSites];
std::atomic<uint64_t> g_ps4_wait_dropped{0};

void NotePs4Wait(uint32_t role, uint64_t key, uint64_t us) {
  uint32_t i = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 57) & (kPs4WaitSites - 1);
  for (uint32_t probe = 0; probe < kPs4WaitSites; ++probe, i = (i + 1) & (kPs4WaitSites - 1)) {
    Ps4WaitSite& site = g_ps4_wait_sites[role][i];
    uint64_t current = site.key.load(std::memory_order_relaxed);
    if (current == 0 && site.key.compare_exchange_strong(current, key)) current = key;
    if (current == key) {
      site.us.fetch_add(us, std::memory_order_relaxed);
      site.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  g_ps4_wait_dropped.fetch_add(1, std::memory_order_relaxed);
}

struct Ps5WaitScope {
  int role = -1;
  uint64_t key = 0;
  int64_t begin_ns = 0;
  Ps5WaitScope(rex::system::XObject* first, uint32_t, uint64_t*) {
    const uintptr_t self = reinterpret_cast<uintptr_t>(pthread_self());
    if (self == rex::frame_stats::g_title_thread.load(std::memory_order_relaxed)) {
      role = 0;
    } else if (self == rex::frame_stats::g_game_main_thread.load(std::memory_order_relaxed)) {
      role = 1;
    } else if (self == rex::frame_stats::g_audio_worker_thread.load(std::memory_order_relaxed)) {
      role = 2;
    } else if (self == rex::frame_stats::g_audio_mix_thread.load(std::memory_order_relaxed)) {
      role = 3;
    }
    uint32_t lr = 0;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (thread && thread->thread_state() && thread->thread_state()->context())
      lr = uint32_t(thread->thread_state()->context()->lr);
    if (role < 0) {
      if (lr != kAudioMixIdleWaitLr) return;
      rex::frame_stats::g_audio_mix_thread.store(self, std::memory_order_relaxed);
      ::Ps4RaiseAudioThreadPriority("game mixing");
      rex::ps5diag::MarkAudioThread();
      role = 3;
    }
    key = (uint64_t(lr) << 8) | (first ? uint32_t(first->type()) & 0xFF : 0) | (uint64_t(1) << 63);
    begin_ns = rex::frame_stats::NowNs();
  }
  ~Ps5WaitScope() {
    if (role < 0) return;
    NotePs4Wait(uint32_t(role), key, uint64_t(rex::frame_stats::NowNs() - begin_ns) / 1000);
  }
};
#else
struct Ps5WaitScope {
  Ps5WaitScope(rex::system::XObject*, uint32_t, uint64_t*) {}
};
#endif
}  // namespace

#if REX_PLATFORM_PS4
namespace rex::system {
// The audio threads' waits outside kernel objects, by the guest code's LR:
// critical-section spins (kind 0xF0), the guest's timed sleeps (0xF1), XMA
// context locks (0xF2 enable, 0xF3 disable, 0xF4 block, 0xF5 clear, 0xF6
// release). Other threads' are ignored.
void Ps4NoteAudioWait(uint32_t lr, uint32_t kind, uint64_t us) {
  const uintptr_t self = reinterpret_cast<uintptr_t>(pthread_self());
  uint32_t role;
  if (self == rex::frame_stats::g_audio_worker_thread.load(std::memory_order_relaxed)) role = 2;
  else if (self == rex::frame_stats::g_audio_mix_thread.load(std::memory_order_relaxed)) role = 3;
  else return;
  if (!lr) {
    auto* thread = XThread::GetCurrentThread();
    if (thread && thread->thread_state() && thread->thread_state()->context())
      lr = uint32_t(thread->thread_state()->context()->lr);
  }
  NotePs4Wait(role, (uint64_t(lr) << 8) | (kind & 0xFF) | (uint64_t(1) << 63), us);
}
bool Ps4IsAudioThread() {
  const uintptr_t self = reinterpret_cast<uintptr_t>(pthread_self());
  return self == rex::frame_stats::g_audio_worker_thread.load(std::memory_order_relaxed) ||
         self == rex::frame_stats::g_audio_mix_thread.load(std::memory_order_relaxed);
}
// The top waits of the last period, per thread, then cleared.
void Ps4ReportGuestWaits(double seconds) {
  static const char* const kRoles[kPs4WaitRoles] = {"render", "main", "audio", "audio-mix"};
  for (uint32_t role = 0; role < kPs4WaitRoles; ++role) {
    struct Entry {
      uint64_t key, us;
      uint32_t count;
    };
    Entry top[8]{};
    uint64_t total_us = 0;
    for (Ps4WaitSite& site : g_ps4_wait_sites[role]) {
      const uint64_t key = site.key.load(std::memory_order_relaxed);
      if (!key) continue;
      const uint64_t us = site.us.exchange(0, std::memory_order_relaxed);
      const uint32_t count = site.count.exchange(0, std::memory_order_relaxed);
      total_us += us;
      Entry entry{key, us, count};
      for (Entry& slot : top) {
        if (entry.us > slot.us) std::swap(entry, slot);
      }
    }
    std::string text;
    for (const Entry& entry : top) {
      if (!entry.us) break;
      const uint32_t kind = uint32_t(entry.key & 0xFF);
      text += fmt::format(" {:08X}/{}:{:.0f}ms/{}", uint32_t(entry.key >> 8),
                          kind == 0xF0   ? "cs-spin"
                          : kind == 0xF1 ? "sleep"
                          : kind == 0xF2 ? "xma-enable"
                          : kind == 0xF3 ? "xma-disable"
                          : kind == 0xF4 ? "xma-block"
                          : kind == 0xF5 ? "xma-clear"
                          : kind == 0xF6 ? "xma-release"
                                         : rex::ps5diag::ObjectTypeName(kind),
                          entry.us / 1000.0, entry.count);
    }
    REXLOG_WARN("ps4 waits: {} thread waited {:.0f} ms of {:.0f} s in guest waits; by game code/object "
                "(ms, count):{}",
                kRoles[role], total_us / 1000.0, seconds, text);
  }
}
}  // namespace rex::system
#endif

namespace rex::system {

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

void XObject::Retain() {
  ++pointer_ref_count_;
}

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  Ps5WaitScope ps5_wait(this, 1, opt_timeout);
  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  XThread::CheckTitleTermination();
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  Ps5WaitScope ps5_wait(wait_object, 1, opt_timeout);
  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      return X_STATUS_SUCCESS;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      return X_STATUS_USER_APC;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  std::vector<rex::thread::WaitHandle*> wait_handles(count);
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  XThread::CheckTitleTermination();
  Ps5WaitScope ps5_wait(count ? objects[0] : nullptr, count, opt_timeout);
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        return X_STATUS(result.second);
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
    }
  } else {
    auto result =
        rex::thread::WaitAll(std::move(wait_handles), alertable ? true : false, timeout_ms);
    XThread::CheckTitleTermination();
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        return X_STATUS_SUCCESS;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        return X_STATUS_USER_APC;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        return X_STATUS_TIMEOUT;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        return X_STATUS_ABANDONED_WAIT_0;
    }
  }
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
