#include "internal.h"

#include <sddl.h>
#include <shellapi.h>

#include <cstdio>

#include "../common/mobcam_ids.h"

namespace mobcam {

std::wstring ModuleDir() {
  // The binary that contains this code: mobcam-core.dll, or mobcam-cli.exe (static build).
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring p(path);
  return p.substr(0, p.find_last_of(L'\\'));
}

namespace {

std::wstring ExeDir() { return ModuleDir(); }

// The DLL is loaded by Frame Server (LOCAL SERVICE), which can't read files under a user
// profile, so it lives in Program Files.
std::wstring InstallDir() {
  wchar_t pf[MAX_PATH];
  GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
  return std::wstring(pf) + L"\\MobCam";
}

std::wstring LogDir() {
  wchar_t pd[MAX_PATH];
  GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
  return std::wstring(pd) + L"\\MobCam";
}

void StopService(const wchar_t* name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return;
  SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (svc) {
    SERVICE_STATUS status{};
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
      for (int i = 0; i < 50 && QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_STOPPED; ++i) {
        Sleep(100);
      }
    }
    CloseServiceHandle(svc);
  }
  CloseServiceHandle(scm);
}

HRESULT CallRegistration(const std::wstring& dll, const char* exportName) {
  HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m) return HRESULT_FROM_WIN32(GetLastError());
  using Fn = HRESULT(STDAPICALLTYPE*)();
  auto fn = reinterpret_cast<Fn>(GetProcAddress(m, exportName));
  HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  FreeLibrary(m);
  return hr;
}

}  // namespace

std::string HrToString(HRESULT hr) {
  char buf[512] = {};
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(hr), 0, buf,
                           sizeof(buf), nullptr);
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
  char out[600];
  snprintf(out, sizeof(out), "0x%08X%s%s", unsigned(hr), n ? " " : "", buf);
  return out;
}

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated;
}

namespace {

// Path of the DLL Frame Server will load, from the COM registration ("" if not registered).
std::wstring RegisteredSourcePath() {
  const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidString + L"\\InprocServer32";
  wchar_t path[MAX_PATH] = {};
  DWORD size = sizeof(path);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) {
    return {};
  }
  return path;
}

}  // namespace

bool IsSourceRegistered() {
  const std::wstring path = RegisteredSourcePath();
  return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsSourceOutdated() {
  WIN32_FILE_ATTRIBUTE_DATA installed{}, bundled{};
  const std::wstring installedPath = RegisteredSourcePath();
  const std::wstring bundledPath = ExeDir() + L"\\mobcam-source.dll";
  if (_wcsicmp(installedPath.c_str(), bundledPath.c_str()) == 0) return false;  // installed copy is this one
  if (!GetFileAttributesExW(bundledPath.c_str(), GetFileExInfoStandard, &bundled)) return false;
  if (!GetFileAttributesExW(installedPath.c_str(), GetFileExInfoStandard, &installed)) return true;
  // CopyFile preserves timestamps, so an identical install matches exactly.
  return installed.nFileSizeLow != bundled.nFileSizeLow || installed.nFileSizeHigh != bundled.nFileSizeHigh ||
         CompareFileTime(&installed.ftLastWriteTime, &bundled.ftLastWriteTime) != 0;
}

int RunElevated(const std::wstring& exe, const std::wstring& args) {
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = args.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) return -1;  // user declined UAC
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  return int(code);
}

int InstallSource() {
  const std::wstring source = ExeDir() + L"\\mobcam-source.dll";
  const std::wstring dir = InstallDir();
  const std::wstring target = dir + L"\\mobcam-source.dll";

  if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
    wprintf(L"mobcam-source.dll not found next to mobcam-cli.exe (%s)\n", source.c_str());
    return 1;
  }
  CreateDirectoryW(dir.c_str(), nullptr);

  if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
    // An older copy is loaded by the camera services. Stop them (they restart on demand).
    wprintf(L"Camera services are using the old version; restarting them...\n");
    StopService(L"FrameServerMonitor");
    StopService(L"FrameServer");
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
      wprintf(L"Could not copy to %s: %hs\nClose apps using the camera and try again.\n", target.c_str(),
              HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
      return 1;
    }
  }

  // Writable log folder for the DLL (it runs as LOCAL SERVICE).
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;WD)",
                                                           SDDL_REVISION_1, &sd, nullptr)) {
    sa.lpSecurityDescriptor = sd;
  }
  CreateDirectoryW(LogDir().c_str(), &sa);
  if (sd) LocalFree(sd);

  HRESULT hr = CallRegistration(target, "DllRegisterServer");
  if (FAILED(hr)) {
    wprintf(L"Registering the virtual camera failed: %hs\n", HrToString(hr).c_str());
    return 1;
  }
  wprintf(L"Virtual camera installed: %s\n", target.c_str());
  return 0;
}

int UninstallSource() {
  const std::wstring target = InstallDir() + L"\\mobcam-source.dll";
  HRESULT hr = CallRegistration(target, "DllUnregisterServer");
  if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) {
    wprintf(L"Unregister failed: %hs\n", HrToString(hr).c_str());
  }
  if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
    StopService(L"FrameServerMonitor");
    StopService(L"FrameServer");
    if (!DeleteFileW(target.c_str())) MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
  RemoveDirectoryW(InstallDir().c_str());
  wprintf(L"Virtual camera removed.\n");
  return 0;
}

HRESULT VirtualCamera::Start() {
  HRESULT hr =
      MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                            MFVirtualCameraAccess_CurrentUser, kCameraName, kSourceClsidString, nullptr, 0, &camera_);
  if (FAILED(hr)) return hr;
  hr = camera_->Start(nullptr);
  if (FAILED(hr)) camera_.Reset();
  return hr;
}

void VirtualCamera::Remove() {
  if (camera_) {
    camera_->Remove();  // not Shutdown(): that double-shuts the source and blocks removal
    camera_.Reset();
  }
}

}  // namespace mobcam
