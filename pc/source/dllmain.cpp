#include <cstdarg>
#include <cstdio>
#include <string>

#include "../common/mobcam_ids.h"
#include "activator.h"
#include "common.h"

namespace mobcam {

HMODULE g_module = nullptr;
std::atomic<long> g_objectCount{0};

void Log(const wchar_t* format, ...) {
  wchar_t message[1024];
  va_list args;
  va_start(args, format);
  _vsnwprintf_s(message, _TRUNCATE, format, args);
  va_end(args);

  wchar_t line[1200];
  SYSTEMTIME t;
  GetLocalTime(&t);
  swprintf_s(line, L"[%02u:%02u:%02u.%03u pid %lu] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             GetCurrentProcessId(), message);
  OutputDebugStringW(line);

  // Optional file log; the directory is created by `mobcam install`.
  wchar_t path[MAX_PATH];
  if (GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH) == 0) return;
  wcscat_s(path, L"\\MobCam\\mobcam-source.log");
  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char utf8[2400];
  int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
  if (n > 1) {
    DWORD written;
    WriteFile(f, utf8, DWORD(n - 1), &written, nullptr);
  }
  CloseHandle(f);
}

namespace {

class ClassFactory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
 public:
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    ComPtr<Activator> activator;
    MC_RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<Activator>(&activator));
    return activator.CopyTo(riid, object);
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) ++g_objectCount; else --g_objectCount;
    return S_OK;
  }

 private:
  ObjectCounter counter_;
};

std::wstring ModulePath() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
  return std::wstring(path, n);
}

}  // namespace
}  // namespace mobcam

using namespace mobcam;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (clsid != kSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
  auto factory = Microsoft::WRL::Make<ClassFactory>();
  if (!factory) return E_OUTOFMEMORY;
  return factory.CopyTo(riid, object);
}

STDAPI DllCanUnloadNow() {
  return g_objectCount.load() == 0 ? S_OK : S_FALSE;
}

// Frame Server only looks in HKLM, so registration requires an elevated process.
STDAPI DllRegisterServer() {
  const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidString;
  const std::wstring inproc = key + L"\\InprocServer32";
  const std::wstring path = ModulePath();

  HKEY h = nullptr;
  LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
  if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
  RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kCameraName), DWORD(sizeof(kCameraName)));
  RegCloseKey(h);

  st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
  if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
  st = RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
                      DWORD((path.size() + 1) * sizeof(wchar_t)));
  if (st == ERROR_SUCCESS) {
    static const wchar_t kBoth[] = L"Both";
    st = RegSetValueExW(h, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(kBoth), sizeof(kBoth));
  }
  RegCloseKey(h);
  return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer() {
  const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidString;
  LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
  return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
