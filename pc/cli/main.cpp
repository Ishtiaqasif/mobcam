// mobcam-cli.exe: command-line front end for the MobCam engine. Also the elevated helper the
// app uses to install the virtual camera.

#include <windows.h>
#include <conio.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "../core/internal.h"
#include "../core/mobcam_core.h"

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI OnConsoleCtrl(DWORD) {
  g_quit = true;
  return TRUE;
}

void PrintUsage() {
  wprintf(
      L"MobCam - use your Android phone as a webcam (command line)\n"
      L"\n"
      L"Usage:\n"
      L"  mobcam-cli                connect over USB if a phone is plugged in, else find it on Wi-Fi\n"
      L"  mobcam-cli --usb          connect over USB (needs USB debugging)\n"
      L"  mobcam-cli --wifi         find the phone on the local network\n"
      L"  mobcam-cli <ip>[:port]    connect to a specific phone address\n"
      L"  mobcam-cli install        install / update the virtual camera (admin)\n"
      L"  mobcam-cli uninstall      remove the virtual camera (admin)\n"
      L"\n"
      L"Options:\n"
      L"  --rotate auto|0|90|180|270  override the picture rotation (default: auto)\n"
      L"  --fill                      crop to fill the frame instead of showing black bars\n"
      L"  --fps 30|60                 ask the phone for this frame rate (60 where the lens allows it)\n"
      L"  --no-camera                 decode only, don't register the virtual camera\n");
}

// Non-blocking read of one answer key: from the console, or from a pipe (echo y | mobcam-cli).
// Returns 0 if nothing was typed.
int ReadAnswerKey() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  if (in == INVALID_HANDLE_VALUE || in == nullptr) return 0;
  const DWORD type = GetFileType(in);
  if (type == FILE_TYPE_CHAR) return _kbhit() ? _getch() : 0;
  if (type == FILE_TYPE_PIPE) {
    DWORD available = 0;
    if (!PeekNamedPipe(in, nullptr, 0, nullptr, &available, nullptr) || available == 0) return 0;
  }
  char c = 0;
  DWORD read = 0;
  return ReadFile(in, &c, 1, &read, nullptr) && read == 1 ? c : 0;
}

const wchar_t* StateName(int s) {
  switch (s) {
    case MC_STATE_SEARCHING:
      return L"searching";
    case MC_STATE_CONNECTING:
      return L"connecting";
    case MC_STATE_STREAMING:
      return L"live";
    case MC_STATE_PAIRING:
      return L"pairing";
    default:
      return L"idle";
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);

  int link = MC_LINK_AUTO;
  std::wstring host;
  int port = 0;
  int rotation = -1;
  bool fill = false;
  bool useCamera = true;
  int wantFps = 0;  // 0 = leave the phone's setting alone

  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"install" || a == L"uninstall") {
      if (!mobcam::IsElevated()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wprintf(L"Requesting administrator rights...\n");
        const int rc = mobcam::RunElevated(exe, a);
        wprintf(rc == 0 ? L"Done.\n" : L"Failed or cancelled (code %d).\n", rc);
        return rc == 0 ? 0 : 1;
      }
      return a == L"install" ? mobcam::InstallSource() : mobcam::UninstallSource();
    } else if (a == L"--usb") {
      link = MC_LINK_USB;
    } else if (a == L"--wifi") {
      link = MC_LINK_WIFI;
    } else if (a == L"--fill") {
      fill = true;
    } else if (a == L"--no-camera") {
      useCamera = false;
    } else if (a == L"--rotate" && i + 1 < argc) {
      std::wstring v = argv[++i];
      rotation = v == L"auto" ? -1 : _wtoi(v.c_str());
    } else if (a == L"--fps" && i + 1 < argc) {
      const int v = _wtoi(argv[++i]);
      if (v != 30 && v != 60) {
        PrintUsage();
        return 2;
      }
      wantFps = v;
    } else if (!a.empty() && a[0] != L'-') {
      const size_t colon = a.find(L':');
      if (colon != std::wstring::npos) {
        port = _wtoi(a.substr(colon + 1).c_str());
        a = a.substr(0, colon);
      }
      host = a;
      link = MC_LINK_WIFI;
    } else {
      PrintUsage();
      return 2;
    }
  }

  if (mc_init() != 0) {
    wprintf(L"Initialisation failed (Media Foundation / Winsock).\n");
    return 1;
  }
  SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

  if (useCamera) {
    int state = mc_vcam_state();
    if (state == MC_VCAM_NOT_INSTALLED || state == MC_VCAM_OUTDATED) {
      wprintf(state == MC_VCAM_NOT_INSTALLED ? L"Installing the virtual camera (approve the admin prompt)...\n"
                                             : L"Updating the virtual camera (approve the admin prompt)...\n");
      if (mc_vcam_install() != 0) {
        wprintf(L"Install failed or was cancelled. Run \"mobcam-cli install\" from an admin terminal.\n");
        mc_shutdown();
        return 1;
      }
    }
    const HRESULT hr = mc_vcam_start();
    if (FAILED(hr)) {
      wprintf(L"Could not start the virtual camera: %hs\n", mobcam::HrToString(hr).c_str());
      mc_shutdown();
      return 1;
    }
    wprintf(L"Virtual camera \"MobCam\" is ready.\n");
  }
  wprintf(L"Press Ctrl+C to stop.\n\n");

  mc_set_rotation(rotation);
  mc_set_fill(fill ? 1 : 0);
  mc_connect(link, host.c_str(), port);

  int shownCode = -1;
  int shownStage = MC_PAIRING_NONE;
  bool answered = false;
  bool fpsSent = false;
  ULONGLONG lastStatus = 0;
  while (!g_quit) {
    Sleep(100);
    mc_status s{};
    mc_get_status(&s);
    if (s.state == MC_STATE_PAIRING && s.pairing_code >= 0) {
      if (s.pairing_code != shownCode) {
        shownCode = s.pairing_code;
        shownStage = MC_PAIRING_NONE;
        answered = false;
        wprintf(L"\n\n  Pairing with %s\n  Code:  %03d %03d\n", s.peer, shownCode / 1000, shownCode % 1000);
      }
      if (s.pairing_stage != shownStage) {
        shownStage = s.pairing_stage;
        if (shownStage == MC_PAIRING_WAIT_PHONE) {
          wprintf(L"  Now tap Allow on the phone.\n");
        } else if (!answered) {
          wprintf(shownStage == MC_PAIRING_WAIT_USER
                      ? L"  The phone allowed it. Does it show the same code? [y/n] "
                      : L"  Does the phone show the same code? [y/n] (and tap Allow there) ");
        }
      }
      fflush(stdout);
      if (!answered) {
        const int key = ReadAnswerKey();
        if (key == 'y' || key == 'Y' || key == 'n' || key == 'N') {
          answered = true;
          const bool yes = key == 'y' || key == 'Y';
          wprintf(L"%c\n", yes ? L'y' : L'n');
          fflush(stdout);
          mc_confirm_pairing(yes ? 1 : 0);
        }
      }
      continue;
    }
    shownCode = -1;
    // The status line once a second; the loop runs faster only to catch the pairing answer.
    const ULONGLONG now = GetTickCount64();
    if (now - lastStatus < 1000) continue;
    lastStatus = now;
    if (s.state == MC_STATE_IDLE && s.problem != MC_PROBLEM_NONE) {
      wprintf(L"\n  %s\n", s.message);
      fflush(stdout);
      break;  // the engine stopped; the user has to act on the phone first
    }
    // --fps: once per connection, as soon as the phone reports its setting (the phone may change it later).
    if (s.state != MC_STATE_STREAMING) fpsSent = false;
    if (wantFps && !fpsSent && s.state == MC_STATE_STREAMING && s.phone_fps > 0) {
      fpsSent = true;
      if (s.phone_fps != wantFps) {
        char command[16];
        sprintf_s(command, "FPS %d", wantFps);
        mc_phone_command(command);
      }
    }
    if (s.state == MC_STATE_STREAMING && s.width) {
      wprintf(L"\r  %-4s | %s | %dx%d | %2d fps | %5d kbps | rot %3d | camera %-6s     ",
              s.link == MC_LINK_USB ? L"USB" : L"Wi-Fi", s.peer, s.width, s.height, s.fps, s.kbps, s.rotation,
              s.camera_in_use ? L"in use" : L"idle");
    } else {
      wprintf(L"\r  [%s] %-90s", StateName(s.state), s.message);
    }
    fflush(stdout);
  }

  wprintf(L"\nStopping...\n");
  mc_shutdown();
  return 0;
}
