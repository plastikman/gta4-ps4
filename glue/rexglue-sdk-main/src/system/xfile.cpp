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

#include <rex/platform.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/thread/mutex.h>

#include <algorithm>
#include <limits>
#include <new>
#include <span>

namespace rex::system {

XFile::XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous)
    : XObject(kernel_state, kObjectType), file_(file), is_synchronous_(synchronous) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
  if (IsIoTraceEnabled()) {
    REXSYS_DEBUG("[PortableSave] Open file '{}'", entry()->absolute_path());
  }
}

XFile::XFile() : XObject(kObjectType) {
  async_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  if (IsIoTraceEnabled()) {
    REXSYS_DEBUG("[PortableSave] Close file '{}'", entry()->absolute_path());
  }
  async_event_->Set();
  file_->Destroy();
}

bool XFile::IsIoTraceEnabled() const {
  if (!file_) {
    return false;
  }
  const auto* host_device = dynamic_cast<const rex::filesystem::HostPathDevice*>(device());
  return host_device && host_device->trace_io();
}

uint64_t XFile::position() const {
  std::lock_guard<std::mutex> lock(file_lock_);
  return position_;
}

void XFile::set_position(uint64_t value) {
  std::lock_guard<std::mutex> lock(file_lock_);
  position_ = value;
}

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                               const std::string_view file_name, bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  rex::filesystem::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(rex::string::utf8_find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() > end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                     uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion) {
  const uint64_t effective_offset =
      file_io::UsesCurrentPosition(byte_offset) ? position() : byte_offset;
  if (notify_completion) {
    BeginIO();
  }
  uint32_t traced_bytes_read = 0;
  uint32_t* bytes_read = out_bytes_read ? out_bytes_read : &traced_bytes_read;
  const X_STATUS result =
      ReadTransfer(buffer_guest_address, buffer_length, byte_offset, bytes_read);
  if (notify_completion) {
    CompleteIO(result, *bytes_read, apc_context);
  }
  if (IsIoTraceEnabled()) {
    REXSYS_TRACE(
        "[PortableSave] Read file='{}' offset={} requested={} transferred={} status={:08X}",
        entry()->absolute_path(), effective_offset, buffer_length, *bytes_read, result);
  }
  return result;
}

bool XFile::ValidateGuestRange(uint32_t guest_address, uint32_t length, bool require_write) const {
  if (!length) {
    return true;
  }
  if (guest_address > std::numeric_limits<uint32_t>::max() - (length - 1)) {
    return false;
  }
  const uint32_t high_address = guest_address + length - 1;
  rex::memory::BaseHeap* start_heap = memory()->LookupHeap(guest_address);
  const rex::memory::BaseHeap* end_heap = memory()->LookupHeap(high_address);
  if (!start_heap || start_heap != end_heap) {
    return false;
  }
  const memory::PageAccess access = start_heap->QueryRangeAccess(guest_address, high_address);
  if (require_write) {
    return access == memory::PageAccess::kReadWrite ||
           access == memory::PageAccess::kExecuteReadWrite;
  }
  return access != memory::PageAccess::kNoAccess;
}

X_STATUS XFile::SnapshotReadScatter(uint32_t segments_guest_address, uint32_t length,
                                    std::vector<uint32_t>* out_segments) const {
  if (!out_segments) {
    return X_STATUS_INVALID_PARAMETER;
  }
  out_segments->clear();
  const size_t segment_count = file_io::ScatterSegmentCount(length);
  if (!segment_count) {
    return X_STATUS_SUCCESS;
  }
  const size_t descriptor_bytes = segment_count * sizeof(rex::be<uint32_t>);
  if (descriptor_bytes > std::numeric_limits<uint32_t>::max() ||
      !ValidateGuestRange(segments_guest_address, static_cast<uint32_t>(descriptor_bytes), false)) {
    return X_STATUS_ACCESS_VIOLATION;
  }

  const auto* descriptors =
      memory()->TranslateVirtual<const rex::be<uint32_t>*>(segments_guest_address);
  try {
    out_segments->reserve(segment_count);
    uint32_t remaining = length;
    for (size_t i = 0; i < segment_count; ++i) {
      const uint32_t destination = static_cast<uint32_t>(descriptors[i]);
      const uint32_t transfer_length = file_io::ScatterSegmentLength(remaining);
      if (!ValidateGuestRange(destination, transfer_length, true)) {
        out_segments->clear();
        return X_STATUS_ACCESS_VIOLATION;
      }
      out_segments->push_back(destination);
      remaining -= transfer_length;
    }
  } catch (const std::bad_alloc&) {
    out_segments->clear();
    return X_STATUS_NO_MEMORY;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XFile::ReadTransfer(uint32_t buffer_guest_address, uint32_t buffer_length,
                             uint64_t byte_offset, uint32_t* out_bytes_read) {
  std::lock_guard<std::mutex> lock(file_lock_);
  return ReadInternalLocked(buffer_guest_address, buffer_length, byte_offset, out_bytes_read);
}

X_STATUS XFile::ReadInternalLocked(uint32_t buffer_guest_address, uint32_t buffer_length,
                                   uint64_t byte_offset, uint32_t* out_bytes_read) {
  if (out_bytes_read) {
    *out_bytes_read = 0;
  }

  const bool use_current_position = file_io::UsesCurrentPosition(byte_offset);
  const uint64_t effective_offset = use_current_position ? position_ : byte_offset;
  if (effective_offset > std::numeric_limits<size_t>::max()) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (file_io::AdvancesPosition(is_synchronous_, byte_offset) &&
      file_io::PositionAdvanceOverflows(effective_offset, buffer_length)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (!ValidateGuestRange(buffer_guest_address, buffer_length, true)) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      rex::memory::BaseHeap* buffer_start_heap = memory()->LookupHeap(buffer_guest_address);
      rex::memory::PhysicalHeap* buffer_physical_heap =
          buffer_start_heap->heap_type() == memory::HeapType::kGuestPhysical
              ? static_cast<rex::memory::PhysicalHeap*>(buffer_start_heap)
              : nullptr;
      uint8_t* const host_destination =
          buffer_physical_heap
              ? memory()->TranslatePhysical(
                    buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))
              : memory()->TranslateVirtual(buffer_guest_address);
      result = file_->ReadSync(std::span<uint8_t>(host_destination, buffer_length),
                               static_cast<size_t>(effective_offset), &bytes_read);
#if REX_PLATFORM_PS4
      // The streamed resource records that later overflow `new T*[count]` all
      // hold the same 24 bytes (2ccd727b e3a0f205 ...), which exist in neither
      // the ISO nor TU8. Log (a) any read landing in the window those records
      // live in and (b) any read anywhere that returns that pattern, with the
      // requested offset, so it is visible whether the game asks for the same
      // thing repeatedly. Cheap range/byte checks; bounded output.
      {
        constexpr uint32_t kWindowLo = 0xCAD00000u, kWindowHi = 0xCAE00000u;
        static const uint8_t kPattern[8] = {0x2c, 0xcd, 0x72, 0x7b, 0xe3, 0xa0, 0xf2, 0x05};
        const bool in_window = buffer_guest_address < kWindowHi &&
                               buffer_guest_address + buffer_length > kWindowLo;
        const bool has_pattern =
            bytes_read >= 8 && std::memcmp(host_destination, kPattern, 8) == 0;
        // Separate budgets so one kind of hit cannot crowd out the other.
        static std::atomic<uint32_t> window_logged{0};
        static std::atomic<uint32_t> pattern_logged{0};
        const bool log_it =
            (in_window && window_logged.fetch_add(1, std::memory_order_relaxed) < 1500) ||
            (has_pattern && pattern_logged.fetch_add(1, std::memory_order_relaxed) < 200);
        if (log_it) {
          char first[16 * 3 + 1] = {};
          for (size_t k = 0; k < 16 && k < bytes_read; ++k) {
            std::snprintf(first + k * 3, 4, "%02x ", host_destination[k]);
          }
          char line[512];
          std::snprintf(line, sizeof(line),
                        "[PS4 read] %s%s path=%s off=0x%llx len=0x%x dst=%08x got=0x%zx res=%08x "
                        "first=%s",
                        in_window ? "WINDOW" : "", has_pattern ? " PATTERN" : "", path().c_str(),
                        static_cast<unsigned long long>(effective_offset), buffer_length,
                        buffer_guest_address, bytes_read, static_cast<uint32_t>(result), first);
          if (FILE* log = std::fopen("/data/LibertyRecomp/logs/gta4-guest-trace.txt", "a")) {
            std::fputs(line, log);
            std::fputc('\n', log);
            std::fclose(log);
          }
        }
      }
#endif
      if (XSUCCEEDED(result)) {
        if (buffer_physical_heap) {
          buffer_physical_heap->TriggerCallbacks(
              rex::thread::global_critical_region::AcquireDirect(), buffer_guest_address,
              buffer_length, true, true);
        }
      }
    }
  }

  // Synchronous file objects follow every successful transfer's effective
  // offset, including explicit offset zero. Asynchronous explicit-offset reads
  // don't mutate the shared position; current-position reads are serialized by
  // file_lock_ and advance it here.
  if (XSUCCEEDED(result) && file_io::AdvancesPosition(is_synchronous_, byte_offset)) {
    position_ = effective_offset + bytes_read;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  return result;
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                            uint32_t* out_bytes_read, uint32_t apc_context) {
  const uint64_t effective_offset =
      file_io::UsesCurrentPosition(byte_offset) ? position() : byte_offset;
  BeginIO();
  std::vector<uint32_t> segments;
  X_STATUS result = SnapshotReadScatter(segments_guest_address, length, &segments);
  uint32_t read_total = 0;
  if (XSUCCEEDED(result)) {
    result = ReadScatterTransfer(segments, length, byte_offset, &read_total);
  }
  if (out_bytes_read) {
    *out_bytes_read = read_total;
  }
  CompleteIO(result, read_total, apc_context);

  if (IsIoTraceEnabled()) {
    REXSYS_TRACE(
        "[PortableSave] ReadScatter file='{}' offset={} requested={} transferred={} status={:08X}",
        entry()->absolute_path(), effective_offset, length, read_total, result);
  }

  return result;
}

X_STATUS XFile::ReadScatterTransfer(std::span<const uint32_t> segments, uint32_t length,
                                    uint64_t byte_offset, uint32_t* out_bytes_read) {
  if (out_bytes_read) {
    *out_bytes_read = 0;
  }
  if (segments.size() != file_io::ScatterSegmentCount(length)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;
  uint32_t read_total = 0;
  uint32_t remaining = length;
  for (uint32_t destination : segments) {
    const uint32_t read_length = file_io::ScatterSegmentLength(remaining);
    uint64_t transfer_offset = byte_offset;
    if (!file_io::UsesCurrentPosition(byte_offset)) {
      if (file_io::ScatterOffsetOverflows(byte_offset, read_total)) {
        result = X_STATUS_INVALID_PARAMETER;
        break;
      }
      transfer_offset = file_io::ScatterExplicitOffset(byte_offset, read_total);
    }

    uint32_t bytes_read = 0;
    result = ReadInternalLocked(destination, read_length, transfer_offset, &bytes_read);
    read_total += bytes_read;
    if (!file_io::ContinueScatter(result, read_length, bytes_read)) {
      break;
    }
    remaining -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = read_total;
  }
  return result;
}

void XFile::BeginIO() {
  ResetWaitEvent();
}

void XFile::CompleteIO(X_STATUS status, uint32_t num_bytes, uint32_t apc_context) {
  NotifyIOCompletion(status, num_bytes, apc_context);
  SignalWaitEvent();
}

void XFile::ResetWaitEvent() {
  async_event_->Reset();
}

void XFile::SignalWaitEvent() {
  async_event_->Set();
}

void XFile::NotifyIOCompletion(X_STATUS status, uint32_t num_bytes, uint32_t apc_context) {
  XIOCompletion::IONotification notification;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = status;
  NotifyIOCompletionPorts(notification);
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                      uint32_t* out_bytes_written, uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_;
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(
      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),
      size_t(byte_offset), &bytes_written);
  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }

  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = uint32_t(bytes_written);
  notify.status = result;

  NotifyIOCompletionPorts(notify);

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  async_event_->Set();
  if (IsIoTraceEnabled()) {
    REXSYS_TRACE(
        "[PortableSave] Write file='{}' offset={} requested={} transferred={} status={:08X}",
        entry()->absolute_path(), byte_offset, buffer_length, bytes_written, result);
  }
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  std::lock_guard<std::mutex> lock(file_lock_);
  const X_STATUS result = file_->SetLength(length);
  if (IsIoTraceEnabled()) {
    REXSYS_TRACE("[PortableSave] Resize file='{}' length={} status={:08X}",
                 entry()->absolute_path(), length, result);
  }
  return result;
}

X_STATUS XFile::Rename(const std::filesystem::path& file_path) {
  std::lock_guard<std::mutex> lock(file_lock_);
  const std::string old_path = entry()->absolute_path();
  const X_STATUS result = entry()->Rename(file_path);
  if (IsIoTraceEnabled()) {
    REXSYS_TRACE("[PortableSave] Rename file='{}' destination='{}' status={:08X}", old_path,
                 file_path.string(), result);
  }
  return result;
}

void XFile::RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  completion_ports_.push_back({key, port});
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end(); it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::HasIOCompletionPorts() {
  std::lock_guard<std::mutex> lock(completion_port_lock_);
  return !completion_ports_.empty();
}

bool XFile::Save(stream::ByteStream* stream) {
  REXSYS_DEBUG("XFile {:08X} ({})", handle(), file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(file_->entry()->absolute_path());
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>((file_->entry()->attributes() & rex::filesystem::kFileAttributeDirectory) !=
                      0);
  stream->Write<bool>(is_synchronous_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();

  REXSYS_DEBUG("XFile {:08X} ({})", file->handle(), abs_path);

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(nullptr, abs_path,
                                                   rex::filesystem::FileDisposition::kOpen, access,
                                                   is_directory, false, &vfs_file, &action);
  if (XFAILED(res)) {
    REXSYS_ERROR("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;

  return object_ref<XFile>(file);
}

void XFile::NotifyIOCompletionPorts(XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace rex::system
