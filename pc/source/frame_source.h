#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "../common/nv12.h"

namespace mobcam {

// Reads the latest canvas published by mobcam.exe through shared memory and produces frames
// at the size the consuming app asked for. Falls back to a "waiting for phone" card.
class FrameSource {
 public:
  ~FrameSource();

  void Fill(const Nv12View& dst);

 private:
  bool EnsureMapping();
  void RenderNoSignal(int width, int height);

  std::mutex lock_;
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  unsigned long long lastAttemptTick_ = 0;
  bool loggedState_ = false;
  bool hadSignal_ = false;

  Nv12Blitter blitter_;
  std::vector<uint8_t> noSignal_;
  int noSignalW_ = 0, noSignalH_ = 0;
};

}  // namespace mobcam
