// The MobCam engine behind the C API in mobcam_core.h.
//
// One worker thread owns the phone connection: it resolves the phone (USB via adb, or Wi-Fi
// discovery), receives H.264, decodes it, renders the 1920x1080 output canvas (rotation +
// fit/fill) and publishes it to the virtual camera. The UI polls status and preview frames.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "../common/shared_frame.h"
#include "internal.h"
#include "mobcam_core.h"

namespace mobcam {
namespace {

void Copy(wchar_t* dst, size_t cap, const std::wstring& s) { wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Utf8(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

class Engine {
 public:
  static Engine& Get() {
    static Engine engine;
    return engine;
  }

  // ------------------------------------------------------------ virtual camera
  int VcamState() {
    std::lock_guard<std::mutex> g(vcamMu_);
    if (vcam_.running()) return MC_VCAM_RUNNING;
    if (vcamError_) return MC_VCAM_FAILED;
    if (!IsSourceRegistered()) return MC_VCAM_NOT_INSTALLED;
    return IsSourceOutdated() ? MC_VCAM_OUTDATED : MC_VCAM_INSTALLED;
  }

  HRESULT VcamStart() {
    std::lock_guard<std::mutex> g(vcamMu_);
    if (vcam_.running()) return S_OK;
    HRESULT hr = E_FAIL;
    // MFCreateVirtualCamera wants an MTA; the caller may be the UI's STA thread.
    std::thread([&] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      hr = vcam_.Start();
      CoUninitialize();
    }).join();
    vcamError_ = FAILED(hr) ? hr : 0;
    return hr;
  }

  void VcamStop() {
    std::lock_guard<std::mutex> g(vcamMu_);
    std::thread([&] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      vcam_.Remove();
      CoUninitialize();
    }).join();
  }

  int VcamInstall() {
    // The elevated helper is the CLI sitting next to this module.
    const std::wstring cli = ModuleDir() + L"\\mobcam-cli.exe";
    const bool wasRunning = vcam_.running();
    if (wasRunning) VcamStop();
    const int rc = RunElevated(cli, L"install");
    {
      std::lock_guard<std::mutex> g(vcamMu_);
      vcamError_ = 0;
    }
    if (wasRunning) VcamStart();
    return rc;
  }

  // ------------------------------------------------------------ connection
  void Connect(int link, const std::wstring& host, int port) {
    Disconnect();
    stop_ = false;
    {
      std::lock_guard<std::mutex> g(statusMu_);
      ResetStreamStatus();
      status_.state = MC_STATE_SEARCHING;
      Copy(status_.message, 256, L"Looking for your phone…");
    }
    std::string h = Utf8(host);
    worker_ =std::thread([this, link, h, port] { Worker(link, h, uint16_t(port > 0 ? port : kStreamPort)); });
  }

  void Disconnect() {
    stop_ = true;
    CloseSocket();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> g(statusMu_);
    ResetStreamStatus();
    status_.state = MC_STATE_IDLE;
    Copy(status_.message, 256, L"Disconnected");
  }

  void GetStatus(mc_status* out) {
    {
      std::lock_guard<std::mutex> g(statusMu_);
      *out = status_;
    }
    out->vcam = VcamState();
    {
      std::lock_guard<std::mutex> g(vcamMu_);
      out->vcam_error = vcamError_;
    }
    {
      std::lock_guard<std::mutex> g(canvasMu_);
      if (!writer_.IsOpen()) writer_.TryOpen();  // so "in use" works before the phone connects
      out->camera_in_use = writer_.ConsumerActive() ? 1 : 0;
    }
  }

  int GetLenses(mc_lens* out, int max) {
    std::lock_guard<std::mutex> g(statusMu_);
    int n = 0;
    for (const auto& lens : lenses_) {
      if (n >= max) break;
      mc_lens& l = out[n++];
      l = {};
      l.front = lens.front ? 1 : 0;
      l.zoom = lens.zoom;
      Copy(l.label, 32, lens.label);
    }
    return n;
  }

  bool SendCommand(const std::string& command) {
    SOCKET s = socket_.load();
    if (s == INVALID_SOCKET) return false;
    const std::string line = command + "\n";
    return send(s, line.data(), int(line.size()), 0) == int(line.size());
  }

  // ------------------------------------------------------------ picture
  void SetRotation(int degrees) { rotationOverride_ = degrees < 0 ? -1 : ((degrees % 360) + 360) % 360 / 90 * 90; }
  void SetFill(bool fill) { fill_ = fill; }

  int GetPreview(uint8_t* bgra, int width, int height, int stride, int64_t* seq) {
    std::lock_guard<std::mutex> g(canvasMu_);
    if (canvasSeq_ == 0 || (seq && *seq == canvasSeq_)) return 0;
    if (seq) *seq = canvasSeq_;

    // Nearest-neighbour downscale + BT.601 video-range NV12 -> BGRA.
    const int cw = int(kCanvasWidth), ch = int(kCanvasHeight);
    const uint8_t* y = canvas_.data();
    const uint8_t* uv = y + size_t(cw) * ch;
    xmap_.resize(width);
    for (int x = 0; x < width; ++x) xmap_[x] = std::min(cw - 1, int((x + 0.5) * cw / width));
    for (int row = 0; row < height; ++row) {
      const int sy = std::min(ch - 1, int((row + 0.5) * ch / height));
      const uint8_t* yr = y + size_t(sy) * cw;
      const uint8_t* uvr = uv + size_t(sy / 2) * cw;
      uint8_t* out = bgra + size_t(row) * stride;
      for (int x = 0; x < width; ++x) {
        const int sx = xmap_[x];
        const int c = 298 * (yr[sx] - 16);
        const int d = uvr[sx & ~1] - 128;
        const int e = uvr[(sx & ~1) + 1] - 128;
        out[4 * x + 0] = uint8_t(std::clamp((c + 516 * d + 128) >> 8, 0, 255));
        out[4 * x + 1] = uint8_t(std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255));
        out[4 * x + 2] = uint8_t(std::clamp((c + 409 * e + 128) >> 8, 0, 255));
        out[4 * x + 3] = 255;
      }
    }
    return 1;
  }

  // ------------------------------------------------------------ discovery
  int Discover(mc_device* out, int max, int timeoutMs) {
    int n = 0;
    const std::wstring adb = FindAdb();
    std::string model;
    if (!adb.empty() && n < max && AdbHasDevice(adb, &model)) {
      mc_device& d = out[n++];
      d = {};
      d.link = MC_LINK_USB;
      d.port = kStreamPort;
      Copy(d.name, 128, Widen(model.empty() ? "Phone" : model));
      Copy(d.host, 64, L"USB");
    }
    for (const auto& p : DiscoverPhones(timeoutMs, false)) {
      if (n >= max) break;
      mc_device& d = out[n++];
      d = {};
      d.link = MC_LINK_WIFI;
      d.port = p.port;
      Copy(d.name, 128, Widen(p.name.empty() ? p.host : p.name));
      Copy(d.host, 64, Widen(p.host));
    }
    return n;
  }

 private:
  ~Engine() {
    // Process exit without mc_shutdown: don't std::terminate on a joinable thread.
    stop_ = true;
    CloseSocket();
    if (worker_.joinable()) worker_.detach();
  }

  Engine() : canvas_(kCanvasBytes) {
    Nv12View v{canvas_.data(), int(kCanvasWidth), canvas_.data() + size_t(kCanvasWidth) * kCanvasHeight,
               int(kCanvasWidth), int(kCanvasWidth), int(kCanvasHeight)};
    FillNv12(v, kBlackY, kNeutralUV, kNeutralUV);
    status_.phone_rotation = -1;
    status_.phone_facing = -1;
    status_.phone_lens = -1;
    Copy(status_.message, 256, L"Not connected");
  }

  void ResetStreamStatus() {
    status_.link = 0;
    status_.width = status_.height = status_.fps = status_.kbps = 0;
    status_.phone_rotation = -1;
    status_.phone_facing = -1;
    status_.phone_target_height = 0;
    status_.phone_lens = -1;
    status_.lens_count = 0;
    lenses_.clear();
    status_.peer[0] = 0;
  }

  void SetState(int state, const std::wstring& message) {
    std::lock_guard<std::mutex> g(statusMu_);
    status_.state = state;
    Copy(status_.message, 256, message);
  }

  void CloseSocket() {
    SOCKET s = socket_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
  }

  bool Wait(int ms) {
    for (int i = 0; i < ms / 100 && !stop_; ++i) Sleep(100);
    return !stop_;
  }

  // Figures out where the phone is. Returns false (with a status message) if not found yet.
  bool Resolve(int link, const std::string& wantedHost, uint16_t wantedPort, std::string& host, uint16_t& port,
               int& usedLink, std::wstring& peer) {
    if (link == MC_LINK_AUTO || link == MC_LINK_USB) {
      const std::wstring adb = FindAdb();
      std::string model;
      if (!adb.empty() && AdbHasDevice(adb, &model)) {
        std::string err;
        if (!AdbForward(adb, kStreamPort, &err)) {
          SetState(MC_STATE_SEARCHING, L"USB: adb forward failed: " + Widen(err));
          return false;
        }
        host = "127.0.0.1";
        port = kStreamPort;
        usedLink = MC_LINK_USB;
        peer = Widen(model.empty() ? "USB phone" : model);
        return true;
      }
      if (link == MC_LINK_USB) {
        SetState(MC_STATE_SEARCHING, adb.empty() ? L"adb not found (install Android platform-tools)"
                                                 : L"No phone on USB. Plug it in and allow USB debugging.");
        return false;
      }
    }

    if (!wantedHost.empty()) {
      host = wantedHost;
      port = wantedPort;
      usedLink = MC_LINK_WIFI;
      peer = Widen(wantedHost);
      return true;
    }

    auto phones = DiscoverPhones(2000, true);
    if (phones.empty()) {
      SetState(MC_STATE_SEARCHING, L"No phone found. Open the MobCam app on the same Wi-Fi.");
      return false;
    }
    host = phones[0].host;
    port = phones[0].port;
    usedLink = MC_LINK_WIFI;
    peer = Widen(phones[0].name.empty() ? phones[0].host : phones[0].name);
    return true;
  }

  void Worker(int link, std::string wantedHost, uint16_t wantedPort) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    H264Decoder decoder;
    AccessUnitAssembler assembler;
    std::vector<char> buffer(256 * 1024);

    while (!stop_) {
      SetState(MC_STATE_SEARCHING, L"Looking for your phone…");
      std::string host;
      uint16_t port = 0;
      int usedLink = 0;
      std::wstring peer;
      if (!Resolve(link, wantedHost, wantedPort, host, port, usedLink, peer)) {
        Wait(2000);
        continue;
      }

      SetState(MC_STATE_CONNECTING, L"Connecting to " + peer + L"…");
      std::string err;
      SOCKET s = ConnectTcp(host, port, 3000, &err);
      if (s == INVALID_SOCKET) {
        SetState(MC_STATE_SEARCHING, Widen(err) + L". Is the MobCam app open?");
        Wait(2000);
        continue;
      }
      socket_ = s;
      if (stop_) break;

      if (FAILED(decoder.Init())) {
        SetState(MC_STATE_SEARCHING, L"H.264 decoder unavailable");
        CloseSocket();
        Wait(2000);
        continue;
      }
      assembler.Reset();
      {
        std::lock_guard<std::mutex> g(statusMu_);
        ResetStreamStatus();
        status_.state = MC_STATE_STREAMING;
        status_.link = usedLink;
        Copy(status_.peer, 128, peer);
        Copy(status_.message, 256, L"Waiting for video…");
      }

      LONGLONG timestamp = 0;
      int frames = 0, width = 0, height = 0;
      uint64_t bytes = 0;
      ULONGLONG lastReport = GetTickCount64();

      auto onFrame = [&](const Nv12ConstView& frame) {
        width = frame.width;
        height = frame.height;
        ++frames;
        Render(frame, assembler.phone().rotation);
      };
      assembler.onAccessUnit = [&](const std::vector<uint8_t>& au, bool) {
        decoder.Decode(au.data(), au.size(), timestamp, onFrame);
        timestamp += 333333;
      };

      while (!stop_) {
        const int n = recv(s, buffer.data(), int(buffer.size()), 0);
        if (n <= 0) break;
        bytes += n;
        assembler.Push(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(n));

        const ULONGLONG now = GetTickCount64();
        if (now - lastReport >= 1000) {
          const auto elapsed = now - lastReport;
          const PhoneInfo info = assembler.phone();
          std::lock_guard<std::mutex> g(statusMu_);
          status_.fps = int(frames * 1000 / elapsed);
          status_.kbps = int(bytes * 8 / elapsed);
          status_.width = width;
          status_.height = height;
          status_.rotation = appliedRotation_;
          status_.phone_rotation = info.rotation;
          status_.phone_facing = info.facing;
          status_.phone_target_height = info.targetHeight;
          status_.phone_lens = info.lensIndex;
          if (!info.lenses.empty()) lenses_ = info.lenses;
          status_.lens_count = int32_t(lenses_.size());
          Copy(status_.message, 256, frames ? L"Live" : L"Waiting for video…");
          frames = 0;
          bytes = 0;
          lastReport = now;
        }
      }

      CloseSocket();
      decoder.Close();
      if (!stop_) {
        {
          std::lock_guard<std::mutex> g(statusMu_);
          ResetStreamStatus();
        }
        SetState(MC_STATE_SEARCHING, L"Phone disconnected. Reconnecting…");
        Wait(1000);
      }
    }
    CoUninitialize();
  }

  void Render(const Nv12ConstView& frame, int phoneRotation) {
    const int override = rotationOverride_.load();
    const int rotation = override >= 0 ? override : (phoneRotation >= 0 ? phoneRotation : 0);
    appliedRotation_ = rotation;

    std::lock_guard<std::mutex> g(canvasMu_);
    Nv12View canvas{canvas_.data(), int(kCanvasWidth), canvas_.data() + size_t(kCanvasWidth) * kCanvasHeight,
                    int(kCanvasWidth), int(kCanvasWidth), int(kCanvasHeight)};
    if (rotation == 0 && frame.width == int(kCanvasWidth) && frame.height == int(kCanvasHeight)) {
      CopyNv12(frame, canvas);
    } else {
      blitter_.Blit(frame, canvas, rotation, fill_.load());
    }
    ++canvasSeq_;
    if (writer_.IsOpen() || writer_.TryOpen()) writer_.Publish(canvas_.data());
  }

  // Connection
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<SOCKET> socket_{INVALID_SOCKET};
  std::mutex statusMu_;
  mc_status status_{};
  std::vector<PhoneLens> lenses_;  // guarded by statusMu_

  // Picture
  std::atomic<int> rotationOverride_{-1};
  std::atomic<bool> fill_{false};
  std::atomic<int> appliedRotation_{0};
  std::mutex canvasMu_;
  std::vector<uint8_t> canvas_;
  int64_t canvasSeq_ = 0;
  Nv12Blitter blitter_;
  FrameWriter writer_;
  std::vector<int> xmap_;

  // Virtual camera
  std::mutex vcamMu_;
  VirtualCamera vcam_;
  HRESULT vcamError_ = 0;
};

std::atomic<int> g_initCount{0};
CO_MTA_USAGE_COOKIE g_mtaCookie = nullptr;

}  // namespace
}  // namespace mobcam

using mobcam::Engine;

extern "C" {

MC_API int32_t mc_init(void) {
  if (mobcam::g_initCount++ > 0) return 0;
  CoIncrementMTAUsage(&mobcam::g_mtaCookie);  // keeps COM objects created on worker threads alive
  if (FAILED(MFStartup(MF_VERSION))) return -1;
  if (!mobcam::InitWinsock()) return -2;
  return 0;
}

MC_API void mc_shutdown(void) {
  if (--mobcam::g_initCount > 0) return;
  Engine::Get().Disconnect();
  Engine::Get().VcamStop();
  WSACleanup();
  MFShutdown();
  if (mobcam::g_mtaCookie) CoDecrementMTAUsage(mobcam::g_mtaCookie);
  mobcam::g_mtaCookie = nullptr;
}

MC_API int32_t mc_vcam_state(void) { return Engine::Get().VcamState(); }
MC_API int32_t mc_vcam_install(void) { return Engine::Get().VcamInstall(); }
MC_API int32_t mc_vcam_start(void) { return Engine::Get().VcamStart(); }
MC_API void mc_vcam_stop(void) { Engine::Get().VcamStop(); }

MC_API void mc_connect(int32_t link, const wchar_t* host, int32_t port) {
  Engine::Get().Connect(link, host ? host : L"", port);
}
MC_API void mc_disconnect(void) { Engine::Get().Disconnect(); }
MC_API void mc_get_status(mc_status* status) {
  if (status) Engine::Get().GetStatus(status);
}
MC_API int32_t mc_discover(mc_device* devices, int32_t max_devices, int32_t timeout_ms) {
  if (!devices || max_devices <= 0) return 0;
  return Engine::Get().Discover(devices, max_devices, timeout_ms);
}

MC_API void mc_set_rotation(int32_t degrees) { Engine::Get().SetRotation(degrees); }
MC_API void mc_set_fill(int32_t fill) { Engine::Get().SetFill(fill != 0); }
MC_API int32_t mc_get_preview(uint8_t* bgra, int32_t width, int32_t height, int32_t stride, int64_t* seq) {
  if (!bgra || width <= 0 || height <= 0 || stride < width * 4) return 0;
  return Engine::Get().GetPreview(bgra, width, height, stride, seq);
}

MC_API int32_t mc_get_lenses(mc_lens* lenses, int32_t max_lenses) {
  if (!lenses || max_lenses <= 0) return 0;
  return Engine::Get().GetLenses(lenses, max_lenses);
}

MC_API int32_t mc_phone_command(const char* command) {
  return command && Engine::Get().SendCommand(command) ? 1 : 0;
}

}  // extern "C"
