#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../common/nv12.h"

namespace mobcam {

using Microsoft::WRL::ComPtr;

constexpr uint16_t kStreamPort = 5000;
constexpr uint16_t kDiscoveryPort = 5001;

// ---------------------------------------------------------------- net.cpp
struct PhoneAddress {
  std::string host;
  uint16_t port = kStreamPort;
  std::string name;
};

bool InitWinsock();
std::vector<PhoneAddress> DiscoverPhones(int timeoutMs, bool stopAtFirst);
SOCKET ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string* error);

std::wstring FindAdb();
bool AdbHasDevice(const std::wstring& adb, std::string* deviceName);
bool AdbForward(const std::wstring& adb, uint16_t port, std::string* error);

// ---------------------------------------------------------------- decoder.cpp

struct PhoneLens {
  std::wstring label;  // "Ultra-wide", "Wide", "Telephoto", "Front"...
  float zoom = 1.0f;
  bool front = false;
};

// Phone metadata carried in MobCam's SEI.
struct PhoneInfo {
  int rotation = -1;     // clockwise degrees to make the picture upright
  int facing = -1;       // 0 back, 1 front
  int targetHeight = 0;  // 480 / 720 / 1080
  int lensIndex = -1;    // index into lenses
  std::vector<PhoneLens> lenses;
};

// Splits a raw H.264 Annex-B byte stream into access units and extracts MobCam's SEI.
class AccessUnitAssembler {
 public:
  std::function<void(const std::vector<uint8_t>& au, bool keyFrame)> onAccessUnit;

  void Push(const uint8_t* data, size_t length);
  void Reset();
  const PhoneInfo& phone() const { return phone_; }

 private:
  void OnNal(const uint8_t* nal, size_t length);
  void Flush();

  std::vector<uint8_t> pending_;
  size_t searched_ = 0;
  std::vector<uint8_t> au_;
  bool auHasSlice_ = false;
  bool auKey_ = false;
  PhoneInfo phone_;
};

// Media Foundation H.264 decoder (software MFT, low-latency mode) producing NV12.
class H264Decoder {
 public:
  using FrameCallback = std::function<void(const Nv12ConstView&)>;

  HRESULT Init();
  void Close();
  HRESULT Decode(const uint8_t* data, size_t length, LONGLONG timestamp, const FrameCallback& onFrame);

 private:
  HRESULT ConfigureOutput();
  HRESULT Drain(const FrameCallback& onFrame);

  ComPtr<IMFTransform> mft_;
  bool outputReady_ = false;
  bool providesSamples_ = false;
  DWORD outputBufferSize_ = 0;
  ComPtr<IMFSample> outSample_;
  UINT32 codedWidth_ = 0, codedHeight_ = 0, stride_ = 0;
  RECT visible_{};
};

// ---------------------------------------------------------------- frame_writer.cpp

// Publishes finished 1920x1080 NV12 canvases to mobcam-source.dll via shared memory.
class FrameWriter {
 public:
  ~FrameWriter();
  bool TryOpen();
  bool IsOpen() const { return view_ != nullptr; }
  void Publish(const uint8_t* canvasNv12);
  // True if an app pulled a frame from the virtual camera recently.
  bool ConsumerActive() const;

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  ULONGLONG lastAttempt_ = 0;
};

// ---------------------------------------------------------------- install.cpp
bool IsElevated();
bool IsSourceRegistered();
bool IsSourceOutdated();  // installed DLL differs from the one next to this module
int InstallSource();      // needs admin
int UninstallSource();    // needs admin
int RunElevated(const std::wstring& exe, const std::wstring& args);  // UAC, waits, returns exit code
std::wstring ModuleDir();                                            // folder of the binary containing the MobCam core

class VirtualCamera {
 public:
  HRESULT Start();
  void Remove();
  bool running() const { return camera_ != nullptr; }

 private:
  ComPtr<IMFVirtualCamera> camera_;
};

std::string HrToString(HRESULT hr);

}  // namespace mobcam
