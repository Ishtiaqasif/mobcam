#pragma once

#include "common.h"
#include "media_stream.h"

namespace mobcam {

class MediaSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                          IMFAttributes, IMFGetService, IKsControl, IMFSampleAllocatorControl, FtmBase> {
 public:
  HRESULT RuntimeClassInitialize(IMFAttributes* attributes);

  // IMFMediaEventGenerator
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;

  // IMFMediaSource
  STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
  STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Shutdown() override;
  STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* start) override;
  STDMETHODIMP Stop() override;

  // IMFMediaSourceEx
  STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
  STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
  STDMETHODIMP SetD3DManager(IUnknown* manager) override;

  // IMFGetService
  STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

  // IMFSampleAllocatorControl
  STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
  STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

  // IKsControl
  STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
  STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
  STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

  // IMFAttributes
  MC_FORWARD_ATTRIBUTES(attributes_)

 private:
  ObjectCounter counter_;
  std::mutex lock_;
  ComPtr<IMFAttributes> attributes_;
  ComPtr<IMFMediaEventQueue> queue_;
  ComPtr<IMFPresentationDescriptor> descriptor_;
  ComPtr<MediaStream> stream_;
};

}  // namespace mobcam
