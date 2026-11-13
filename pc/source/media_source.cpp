#include "media_source.h"

namespace mobcam {

constexpr DWORD kStreamId = 0;

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* attributes) {
  MC_RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 8));
  if (attributes) MC_RETURN_IF_FAILED(attributes->CopyAllItems(attributes_.Get()));

  // Sensor profile so apps that query camera profiles (e.g. Windows Camera) are happy.
  ComPtr<IMFSensorProfileCollection> profiles;
  ComPtr<IMFSensorProfile> legacy;
  if (SUCCEEDED(MFCreateSensorProfileCollection(&profiles)) &&
      SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy)) &&
      SUCCEEDED(legacy->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))")) &&
      SUCCEEDED(profiles->AddProfile(legacy.Get()))) {
    attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
  }

  MC_RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaStream>(&stream_, this, kStreamId));

  ComPtr<IMFStreamDescriptor> sd;
  MC_RETURN_IF_FAILED(stream_->GetStreamDescriptor(&sd));
  IMFStreamDescriptor* list[] = {sd.Get()};
  MC_RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, list, &descriptor_));
  MC_RETURN_IF_FAILED(MFCreateEventQueue(&queue_));
  Log(L"MediaSource: created (pid %lu)", GetCurrentProcessId());
  return S_OK;
}

// ---- IMFMediaEventGenerator ----

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  if (!event) return E_POINTER;
  *event = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
  if (!event) return E_POINTER;
  *event = nullptr;
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!queue_) return MF_E_SHUTDOWN;
    queue = queue_;
  }
  return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->QueueEventParamVar(met, ext, status, value);
}

// ---- IMFMediaSource ----

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
  if (!descriptor) return E_POINTER;
  *descriptor = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!descriptor_) return MF_E_SHUTDOWN;
  return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
  if (!characteristics) return E_POINTER;
  *characteristics = MFMEDIASOURCE_IS_LIVE;
  return S_OK;
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown() {
  Log(L"MediaSource: shutdown");
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  queue_->Shutdown();
  queue_.Reset();
  if (stream_) stream_->Shutdown();
  stream_.Reset();
  descriptor_.Reset();
  return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) {
  if (!pd || !start) return E_POINTER;
  if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_ || !descriptor_ || !stream_) return MF_E_SHUTDOWN;

  DWORD count = 0;
  MC_RETURN_IF_FAILED(pd->GetStreamDescriptorCount(&count));
  if (count != 1) return E_INVALIDARG;

  BOOL selected = FALSE;
  ComPtr<IMFStreamDescriptor> sd;
  MC_RETURN_IF_FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &sd));

  MF_STREAM_STATE state = MF_STREAM_STATE_STOPPED;
  stream_->GetStreamState(&state);
  const bool running = state != MF_STREAM_STATE_STOPPED;

  if (selected) {
    MC_RETURN_IF_FAILED(descriptor_->SelectStream(0));
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> type;
    MC_RETURN_IF_FAILED(sd->GetMediaTypeHandler(&handler));
    MC_RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

    ComPtr<IUnknown> unk;
    MC_RETURN_IF_FAILED(stream_.As(&unk));
    MC_RETURN_IF_FAILED(
        queue_->QueueEventParamUnk(running ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, unk.Get()));
    MC_RETURN_IF_FAILED(stream_->Start(type.Get()));
  } else if (running) {
    MC_RETURN_IF_FAILED(descriptor_->DeselectStream(0));
    MC_RETURN_IF_FAILED(stream_->Stop());
  }

  PROPVARIANT time;
  InitPropVariantFromInt64(MFGetSystemTime(), &time);
  HRESULT hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
  PropVariantClear(&time);
  return hr;
}

STDMETHODIMP MediaSource::Stop() {
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_ || !descriptor_ || !stream_) return MF_E_SHUTDOWN;
  stream_->Stop();
  descriptor_->DeselectStream(0);

  PROPVARIANT time;
  InitPropVariantFromInt64(MFGetSystemTime(), &time);
  HRESULT hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
  PropVariantClear(&time);
  return hr;
}

// ---- IMFMediaSourceEx ----

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
  if (!attributes) return E_POINTER;
  return QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
  if (!attributes) return E_POINTER;
  *attributes = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!stream_) return MF_E_SHUTDOWN;
  if (streamId != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
  return stream_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
  // Frames are produced on the CPU; ignore the GPU device.
  return S_OK;
}

// ---- IMFGetService ----

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
  if (object) *object = nullptr;
  return MF_E_UNSUPPORTED_SERVICE;
}

// ---- IMFSampleAllocatorControl ----

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!stream_) return MF_E_SHUTDOWN;
  if (streamId != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
  return stream_->SetAllocator(allocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
  if (!inputStreamId || !usage) return E_POINTER;
  if (streamId != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
  *inputStreamId = streamId;
  *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
  return S_OK;
}

// ---- IKsControl ----

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace mobcam
