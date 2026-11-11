#pragma once

#include <windows.h>
// COM/WRL headers first: ks.h redefines GUID_NULL as a macro, which breaks cguid.h if it
// is included afterwards.
#include <cguid.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wrl/ftm.h>
#include <wrl/implements.h>

#include <initguid.h>  // define MF/KS GUIDs in-module (selectany, safe in every TU)
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace mobcam {

extern HMODULE g_module;
extern std::atomic<long> g_objectCount;

// Writes to OutputDebugString (view with DebugView) and, if %ProgramData%\MobCam exists,
// appends to %ProgramData%\MobCam\mobcam-source.log.
void Log(const wchar_t* format, ...);

// Embed in every COM class so DllCanUnloadNow knows when nothing is alive.
struct ObjectCounter {
  ObjectCounter() { ++g_objectCount; }
  ~ObjectCounter() { --g_objectCount; }
};

}  // namespace mobcam

#define MC_RETURN_IF_FAILED(expr)                                                        \
  do {                                                                                   \
    HRESULT hr__ = (expr);                                                               \
    if (FAILED(hr__)) {                                                                  \
      mobcam::Log(L"FAILED 0x%08X: %hs (%hs:%d)", hr__, #expr, __FILE__, __LINE__);      \
      return hr__;                                                                       \
    }                                                                                    \
  } while (0)

// Implements every IMFAttributes method by forwarding to a ComPtr<IMFAttributes> member.
#define MC_FORWARD_ATTRIBUTES(store)                                                                         \
  STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return store->GetItem(k, v); }                  \
  STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return store->GetItemType(k, t); }    \
  STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return store->CompareItem(k, v, r); } \
  STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return store->Compare(o, m, r); } \
  STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return store->GetUINT32(k, v); }                   \
  STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return store->GetUINT64(k, v); }                   \
  STDMETHODIMP GetDouble(REFGUID k, double* v) override { return store->GetDouble(k, v); }                   \
  STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return store->GetGUID(k, v); }                         \
  STDMETHODIMP GetStringLength(REFGUID k, UINT32* v) override { return store->GetStringLength(k, v); }       \
  STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return store->GetString(k, s, n, l); } \
  STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return store->GetAllocatedString(k, s, l); } \
  STDMETHODIMP GetBlobSize(REFGUID k, UINT32* v) override { return store->GetBlobSize(k, v); }               \
  STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return store->GetBlob(k, b, n, s); } \
  STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return store->GetAllocatedBlob(k, b, s); } \
  STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return store->GetUnknown(k, i, p); }    \
  STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return store->SetItem(k, v); }                \
  STDMETHODIMP DeleteItem(REFGUID k) override { return store->DeleteItem(k); }                               \
  STDMETHODIMP DeleteAllItems() override { return store->DeleteAllItems(); }                                 \
  STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return store->SetUINT32(k, v); }                    \
  STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return store->SetUINT64(k, v); }                    \
  STDMETHODIMP SetDouble(REFGUID k, double v) override { return store->SetDouble(k, v); }                    \
  STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return store->SetGUID(k, v); }                       \
  STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return store->SetString(k, v); }                   \
  STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return store->SetBlob(k, b, n); }     \
  STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return store->SetUnknown(k, u); }               \
  STDMETHODIMP LockStore() override { return store->LockStore(); }                                           \
  STDMETHODIMP UnlockStore() override { return store->UnlockStore(); }                                       \
  STDMETHODIMP GetCount(UINT32* c) override { return store->GetCount(c); }                                   \
  STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return store->GetItemByIndex(i, k, v); } \
  STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return store->CopyAllItems(d); }
