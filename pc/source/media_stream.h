#pragma once

#include "common.h"
#include "frame_source.h"

namespace mobcam {

// The single video stream of the virtual camera. Produces NV12 samples on demand.
class MediaStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                        ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                                        IMFAttributes, IKsControl, FtmBase> {
 public:
  HRESULT RuntimeClassInitialize(IMFMediaSource* source, DWORD streamId);

  HRESULT Start(IMFMediaType* type);
  HRESULT Stop();
  void Shutdown();
  HRESULT SetAllocator(IUnknown* allocator);

  // IMFMediaEventGenerator
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;

  // IMFMediaStream
  STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
  STDMETHODIMP RequestSample(IUnknown* token) override;

  // IMFMediaStream2
  STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
  STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

  // IKsControl
  STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
  STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
  STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

  // IMFAttributes
  MC_FORWARD_ATTRIBUTES(attributes_)

 private:
  HRESULT StartLocked(IMFMediaType* type);
  HRESULT StopLocked();
  HRESULT WriteFrame(IMFSample* sample, UINT32 width, UINT32 height);

  ObjectCounter counter_;
  std::mutex lock_;
  ComPtr<IMFAttributes> attributes_;
  ComPtr<IMFMediaEventQueue> queue_;
  ComPtr<IMFStreamDescriptor> descriptor_;
  ComPtr<IMFMediaSource> source_;
  ComPtr<IMFVideoSampleAllocatorEx> allocator_;
  MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
  UINT32 width_ = 1920;
  UINT32 height_ = 1080;
  LONGLONG frameDuration_ = 10'000'000 / 30;  // 100 ns units, from the chosen format's frame rate
  LONGLONG nextDue_ = 0;
  FrameSource frames_;
};

}  // namespace mobcam
