#include "internal.h"

#include <sddl.h>

#include "../common/shared_frame.h"

namespace mobcam {

FrameWriter::~FrameWriter() {
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(mapping_);
}

bool FrameWriter::TryOpen() {
  if (view_) return true;
  const ULONGLONG now = GetTickCount64();
  if (lastAttempt_ && now - lastAttempt_ < 1000) return false;
  lastAttempt_ = now;

  // Normally created by mobcam-source.dll inside Frame Server the first time an app opens the
  // camera. When we run elevated we may create it ourselves.
  bool created = false;
  HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
  if (!h) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kMappingSddl, SDDL_REVISION_1, &sd, nullptr)) {
      sa.lpSecurityDescriptor = sd;
    }
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(kMappingBytes >> 32),
                           DWORD(kMappingBytes & 0xFFFFFFFF), kMappingName);
    created = h && GetLastError() != ERROR_ALREADY_EXISTS;
    if (sd) LocalFree(sd);
  }
  if (!h) return false;

  void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingBytes);
  if (!view) {
    CloseHandle(h);
    return false;
  }
  auto* header = static_cast<SharedHeader*>(view);
  if (created || header->magic == 0) InitHeader(header);
  if (!HeaderValid(header)) {
    UnmapViewOfFile(view);
    CloseHandle(h);
    return false;
  }
  mapping_ = h;
  view_ = view;
  return true;
}

void FrameWriter::Publish(const uint8_t* canvas) {
  if (!view_) return;
  auto* header = static_cast<SharedHeader*>(view_);
  const LONG latest = header->latestSlot;
  const uint32_t slot = latest < 0 ? 0 : uint32_t((latest + 1) % LONG(kSlotCount));
  memcpy(SlotPtr(view_, slot), canvas, kCanvasBytes);

  MemoryBarrier();
  InterlockedExchange(&header->latestSlot, LONG(slot));
  InterlockedIncrement64(&header->sequence);
  InterlockedExchange64(&header->lastWriteTick, LONG64(GetTickCount64()));
}

bool FrameWriter::ConsumerActive() const {
  if (!view_) return false;
  const auto* header = static_cast<const SharedHeader*>(view_);
  const auto last = static_cast<ULONGLONG>(header->lastReadTick);
  return last != 0 && GetTickCount64() - last < 2000;
}

}  // namespace mobcam
