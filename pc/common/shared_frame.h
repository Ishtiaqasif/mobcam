// Shared-memory contract between mobcam.exe (writer) and mobcam-source.dll (reader).
//
// The DLL runs inside the Windows Frame Server service (session 0), the client runs in the
// user's session. Only session-0 code (or an elevated process) may *create* a Global\ object,
// so the DLL creates the mapping and the client opens it.
//
// The client always writes a fixed 1920x1080 NV12 "canvas" (already rotated and letterboxed).
// The DLL scales that canvas to whatever size the consuming app picked.
#pragma once

#include <windows.h>
#include <cstdint>

namespace mobcam {

constexpr wchar_t kMappingName[] = L"Global\\MobCamSharedFrame_v1";
constexpr uint32_t kMagic = 0x4D43414D;  // 'MCAM'
constexpr uint32_t kVersion = 1;

constexpr uint32_t kCanvasWidth = 1920;
constexpr uint32_t kCanvasHeight = 1080;
constexpr uint32_t kCanvasBytes = kCanvasWidth * kCanvasHeight * 3 / 2;  // NV12
constexpr uint32_t kSlotCount = 3;  // triple buffering: writer never touches the slot being read
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint64_t kMappingBytes = kHeaderBytes + uint64_t(kSlotCount) * kCanvasBytes;

// A frame older than this is treated as "no signal".
constexpr uint64_t kStaleMs = 1500;

// Everyone in the interactive session (and services) may read/write; low-IL allowed too.
constexpr wchar_t kMappingSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;AU)S:(ML;;NW;;;LW)";

struct SharedHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t slotCount;
  uint32_t frameBytes;
  volatile LONG latestSlot;  // index of the newest complete frame, -1 if none yet
  volatile LONG reserved0;
  volatile LONG64 sequence;       // increments once per published frame
  volatile LONG64 lastWriteTick;  // GetTickCount64() of the last publish (system-wide clock)
  volatile LONG64 lastReadTick;   // GetTickCount64() of the last frame an app pulled (set by the DLL)
};
static_assert(sizeof(SharedHeader) <= kHeaderBytes, "header too large");

inline uint8_t* SlotPtr(void* base, uint32_t slot) {
  return static_cast<uint8_t*>(base) + kHeaderBytes + size_t(slot) * kCanvasBytes;
}

inline void InitHeader(SharedHeader* h) {
  h->magic = kMagic;
  h->version = kVersion;
  h->width = kCanvasWidth;
  h->height = kCanvasHeight;
  h->slotCount = kSlotCount;
  h->frameBytes = kCanvasBytes;
  h->latestSlot = -1;
  h->sequence = 0;
  h->lastWriteTick = 0;
  h->lastReadTick = 0;
}

inline bool HeaderValid(const SharedHeader* h) {
  return h->magic == kMagic && h->version == kVersion && h->width == kCanvasWidth && h->height == kCanvasHeight &&
         h->slotCount == kSlotCount;
}

}  // namespace mobcam
