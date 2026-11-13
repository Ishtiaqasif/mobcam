// The MobCam engine behind the C API in mobcam_core.h.
//
// One worker thread owns the phone connection: it resolves the phone (USB via adb, or Wi-Fi
// discovery), receives H.264, decodes it, renders the 1920x1080 output canvas (rotation +
// fit/fill) and publishes it to the virtual camera. The UI polls status and preview frames.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../common/shared_frame.h"
#include "internal.h"
#include "mobcam_core.h"
#include "secure.h"

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
      status_.problem = MC_PROBLEM_NONE;
      status_.state = MC_STATE_SEARCHING;
      Copy(status_.message, 256, L"Looking for your phone…");
    }
    std::string h = Utf8(host);
    worker_ = std::thread([this, link, h, port] { Worker(link, h, uint16_t(port > 0 ? port : kStreamPort)); });
  }

  void Disconnect() {
    stop_ = true;
    pairCv_.notify_all();  // a handshake waiting for the user's pairing answer gives up
    CloseSocket();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> g(statusMu_);
    ResetStreamStatus();
    status_.state = MC_STATE_IDLE;
    Copy(status_.message, 256, L"Disconnected");
  }

  // This PC's user answered the pairing question (see PairingHooks).
  void ConfirmPairing(bool accept) {
    {
      std::lock_guard<std::mutex> g(statusMu_);
      if (status_.state != MC_STATE_PAIRING) return;
      if (accept && status_.pairing_stage == MC_PAIRING_CONFIRM) {
        status_.pairing_stage = MC_PAIRING_WAIT_PHONE;
        Copy(status_.message, 256, L"Now tap Allow on the phone to finish pairing.");
      }
    }
    {
      std::lock_guard<std::mutex> g(pairMu_);
      pairDecision_ = accept ? 1 : 0;
    }
    pairCv_.notify_all();
    // Cancel: drop the connection now; the phone closes its prompt when the PC goes away.
    if (!accept) CloseSocket();
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

  bool SendCommand(const std::string& command) { return session_.SendCommand(command); }

  int GetPairedPhones(mc_paired_phone* out, int max) {
    int n = 0;
    for (const auto& p : ListPairedPhones()) {
      if (n >= max) break;
      mc_paired_phone& d = out[n++];
      d = {};
      d.paired_at = p.pairedAt;
      Copy(d.id, 72, p.id);
      Copy(d.name, 128, p.name);
    }
    return n;
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
    Nv12View v{canvas_.data(),    int(kCanvasWidth), canvas_.data() + size_t(kCanvasWidth) * kCanvasHeight,
               int(kCanvasWidth), int(kCanvasWidth), int(kCanvasHeight)};
    FillNv12(v, kBlackY, kNeutralUV, kNeutralUV);
    status_.phone_rotation = -1;
    status_.phone_facing = -1;
    status_.phone_lens = -1;
    status_.pairing_code = -1;
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
    status_.pairing_code = -1;
    status_.pairing_stage = MC_PAIRING_NONE;
  }

  // Like SetState, but keeps the message of an unresolved problem (e.g. "update the phone app")
  // visible while the engine keeps retrying.
  void SetProgress(int state, const std::wstring& message) {
    std::lock_guard<std::mutex> g(statusMu_);
    status_.state = state;
    if (status_.problem == MC_PROBLEM_NONE) Copy(status_.message, 256, message);
  }

  void SetProblem(int state, int problem, const std::wstring& message) {
    std::lock_guard<std::mutex> g(statusMu_);
    status_.state = state;
    status_.problem = problem;
    status_.pairing_code = -1;
    status_.pairing_stage = MC_PAIRING_NONE;
    Copy(status_.message, 256, message);
  }

  // Runs the v2 handshake. Returns true to stream; otherwise sets the status and *retry
  // (false = stop until the user connects again).
  bool Authenticate(SOCKET s, int usedLink, const std::wstring& peer, bool* retry) {
    *retry = true;
    HandshakeInfo info;
    {
      std::lock_guard<std::mutex> g(pairMu_);
      pairDecision_ = -1;
    }
    PairingHooks hooks;
    hooks.onPairing = [&](int code) {
      wchar_t text[256];
      swprintf_s(text, L"Pairing: if %s shows %03d %03d, confirm here and tap Allow on the phone", peer.c_str(),
                 code / 1000, code % 1000);
      std::lock_guard<std::mutex> g(statusMu_);
      status_.state = MC_STATE_PAIRING;
      status_.pairing_code = code;
      status_.pairing_stage = MC_PAIRING_CONFIRM;
      Copy(status_.peer, 128, peer);
      Copy(status_.message, 256, text);
    };
    hooks.onPhoneAllowed = [&] {
      std::lock_guard<std::mutex> g(statusMu_);
      if (status_.pairing_stage == MC_PAIRING_CONFIRM) {
        status_.pairing_stage = MC_PAIRING_WAIT_USER;
        Copy(status_.message, 256, L"The phone allowed the connection. Confirm the code here to finish pairing.");
      }
    };
    hooks.waitForUser = [&](int timeoutMs) {
      std::unique_lock<std::mutex> lock(pairMu_);
      pairCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return pairDecision_ != -1 || stop_; });
      return pairDecision_ == 1 && !stop_;
    };
    const HandshakeResult r = session_.Handshake(s, usedLink == MC_LINK_USB, hooks, &info);
    if (stop_) return false;
    int decision;
    {
      std::lock_guard<std::mutex> g(pairMu_);
      decision = pairDecision_;
    }
    switch (r) {
      case HandshakeResult::Ok:
        return true;
      case HandshakeResult::Cancelled:
        *retry = false;
        SetProblem(MC_STATE_IDLE, MC_PROBLEM_CANCELLED,
                   decision == 0 ? L"Pairing cancelled. Nothing was saved on the PC or the phone."
                                 : L"Pairing wasn't confirmed on this PC in time. Connect again to retry.");
        return false;
      case HandshakeResult::PhoneTooOld:
        SetProblem(MC_STATE_SEARCHING, MC_PROBLEM_PHONE_TOO_OLD,
                   L"The MobCam app on your phone is too old. Update it to connect.");
        return false;
      case HandshakeResult::PhoneTooNew:
        SetProblem(MC_STATE_SEARCHING, MC_PROBLEM_PC_TOO_OLD,
                   L"The MobCam app on your phone is newer than this PC app. Update MobCam on the PC.");
        return false;
      case HandshakeResult::NotPaired:
        *retry = false;
        SetProblem(MC_STATE_IDLE, MC_PROBLEM_NOT_PAIRED,
                   L"This PC forgot the phone. On the phone, open Paired PCs, forget this PC, then connect again.");
        return false;
      case HandshakeResult::Denied:
        switch (info.deniedReason) {
          case kDeniedBusy:
            SetProblem(MC_STATE_SEARCHING, MC_PROBLEM_AUTH_FAILED,
                       L"The phone is showing another pairing request. Answer it first.");
            return false;
          case kDeniedTimeout:
            *retry = false;
            SetProblem(MC_STATE_IDLE, MC_PROBLEM_PAIRING_TIMEOUT,
                       L"Pairing timed out. Connect again and tap Allow on the phone.");
            return false;
          case kDeniedBadPc:
            SetProblem(MC_STATE_SEARCHING, MC_PROBLEM_AUTH_FAILED, L"The phone rejected this PC's identity. Retrying…");
            return false;
          default:
            *retry = false;
            SetProblem(MC_STATE_IDLE, MC_PROBLEM_DENIED, L"Pairing was denied on the phone.");
            return false;
        }
      case HandshakeResult::Failed:
      default: {
        if (decision == 0) {  // the user cancelled while the phone was still asking (socket closed)
          *retry = false;
          SetProblem(MC_STATE_IDLE, MC_PROBLEM_CANCELLED,
                     L"Pairing cancelled. Nothing was saved on the PC or the phone.");
          return false;
        }
        // A timeout while the phone was asking means nobody answered.
        int wasPairing = 0;
        {
          std::lock_guard<std::mutex> g(statusMu_);
          wasPairing = status_.state == MC_STATE_PAIRING;
        }
        if (wasPairing) {
          *retry = false;
          SetProblem(MC_STATE_IDLE, MC_PROBLEM_PAIRING_TIMEOUT, L"Pairing was not completed. Connect again to retry.");
        } else {
          SetProblem(MC_STATE_SEARCHING, MC_PROBLEM_AUTH_FAILED,
                     L"Secure connection failed: " + Widen(info.error) + L". Retrying…");
        }
        return false;
      }
    }
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
      SetProgress(MC_STATE_SEARCHING, L"Looking for your phone…");
      std::string host;
      uint16_t port = 0;
      int usedLink = 0;
      std::wstring peer;
      if (!Resolve(link, wantedHost, wantedPort, host, port, usedLink, peer)) {
        Wait(2000);
        continue;
      }

      SetProgress(MC_STATE_CONNECTING, L"Connecting to " + peer + L"…");
      std::string err;
      SOCKET s = ConnectTcp(host, port, 3000, &err);
      if (s == INVALID_SOCKET) {
        SetState(MC_STATE_SEARCHING, Widen(err) + L". Is the MobCam app open?");
        Wait(2000);
        continue;
      }
      socket_ = s;
      if (stop_) break;

      SetProgress(MC_STATE_CONNECTING, L"Securing the connection to " + peer + L"…");
      bool retry = true;
      // A hand-typed 127.0.0.1 is an adb forward too; the phone treats loopback as USB.
      const int trustLink = host == "127.0.0.1" ? MC_LINK_USB : usedLink;
      if (!Authenticate(s, trustLink, peer, &retry)) {
        CloseSocket();
        session_.Reset();
        if (!retry) break;  // the user has to act (denied, timed out...); mc_connect starts over
        Wait(3000);
        continue;
      }
      {
        std::lock_guard<std::mutex> g(statusMu_);
        status_.problem = MC_PROBLEM_NONE;
      }

      if (FAILED(decoder.Init())) {
        SetState(MC_STATE_SEARCHING, L"H.264 decoder unavailable");
        CloseSocket();
        session_.Reset();
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

      auto onVideo = [&](const uint8_t* data, size_t length) { assembler.Push(data, length); };
      bool damaged = false;
      while (!stop_) {
        const int n = recv(s, buffer.data(), int(buffer.size()), 0);
        if (n <= 0) break;
        bytes += n;
        if (!session_.Receive(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(n), onVideo)) {
          damaged = true;
          break;
        }

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
      session_.Reset();
      decoder.Close();
      if (!stop_) {
        {
          std::lock_guard<std::mutex> g(statusMu_);
          ResetStreamStatus();
        }
        SetState(MC_STATE_SEARCHING,
                 damaged ? L"Received damaged or forged data. Reconnecting…" : L"Phone disconnected. Reconnecting…");
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
    Nv12View canvas{canvas_.data(),    int(kCanvasWidth), canvas_.data() + size_t(kCanvasWidth) * kCanvasHeight,
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
  SecureSession session_;
  std::mutex statusMu_;
  // This PC user's answer to the pairing question: -1 none yet, 0 cancel, 1 codes match.
  std::mutex pairMu_;
  std::condition_variable pairCv_;
  int pairDecision_ = -1;
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

MC_API int32_t mc_phone_command(const char* command) { return command && Engine::Get().SendCommand(command) ? 1 : 0; }

MC_API int32_t mc_get_paired_phones(mc_paired_phone* phones, int32_t max_phones) {
  if (!phones || max_phones <= 0) return 0;
  return Engine::Get().GetPairedPhones(phones, max_phones);
}

MC_API int32_t mc_forget_phone(const wchar_t* id) { return id && mobcam::ForgetPairedPhone(id) ? 1 : 0; }

MC_API void mc_confirm_pairing(int32_t accept) { Engine::Get().ConfirmPairing(accept != 0); }

}  // extern "C"
