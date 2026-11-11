#include "media_stream.h"

#include <algorithm>

namespace mobcam {

namespace {

constexpr UINT32 kFps = 30;
constexpr LONGLONG kFrameDuration = 10'000'000 / kFps;  // 100 ns units

struct Size { UINT32 w, h; };
// All 16:9 so scaling from the 1920x1080 canvas never distorts.
constexpr Size kSizes[] = {{1920, 1080}, {1280, 720}, {640, 360}};

HRESULT MakeNv12Type(UINT32 w, UINT32 h, IMFMediaType** out) {
  ComPtr<IMFMediaType> t;
  MC_RETURN_IF_FAILED(MFCreateMediaType(&t));
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
  MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
  t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
  t->SetUINT32(MF_MT_AVG_BITRATE, w * h * 12 * kFps);
  t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  *out = t.Detach();
  return S_OK;
}

}  // namespace

HRESULT MediaStream::RuntimeClassInitialize(IMFMediaSource* source, DWORD streamId) {
  source_ = source;
  MC_RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 8));
  MC_RETURN_IF_FAILED(attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
  MC_RETURN_IF_FAILED(attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
  MC_RETURN_IF_FAILED(attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
  MC_RETURN_IF_FAILED(attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));

  MC_RETURN_IF_FAILED(MFCreateEventQueue(&queue_));

  IMFMediaType* types[_countof(kSizes)] = {};
  for (size_t i = 0; i < _countof(kSizes); ++i) {
    HRESULT hr = MakeNv12Type(kSizes[i].w, kSizes[i].h, &types[i]);
    if (FAILED(hr)) {
      for (auto* t : types) if (t) t->Release();
      return hr;
    }
  }
  HRESULT hr = MFCreateStreamDescriptor(streamId, _countof(types), types, &descriptor_);
  if (SUCCEEDED(hr)) {
    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
  }
  for (auto* t : types) t->Release();
  MC_RETURN_IF_FAILED(hr);
  return S_OK;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator) {
  if (!allocator) return E_POINTER;
  std::lock_guard<std::mutex> guard(lock_);
  allocator_.Reset();
  return allocator->QueryInterface(IID_PPV_ARGS(&allocator_));
}

HRESULT MediaStream::Start(IMFMediaType* type) {
  std::lock_guard<std::mutex> guard(lock_);
  return StartLocked(type);
}

HRESULT MediaStream::StartLocked(IMFMediaType* type) {
  if (!queue_) return MF_E_SHUTDOWN;

  ComPtr<IMFMediaType> current = type;
  if (!current) {
    ComPtr<IMFMediaTypeHandler> handler;
    MC_RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(&handler));
    MC_RETURN_IF_FAILED(handler->GetCurrentMediaType(&current));
  }
  MC_RETURN_IF_FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width_, &height_));

  // Frame Server normally hands us an allocator; create our own if it didn't.
  if (!allocator_) MC_RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
  allocator_->UninitializeSampleAllocator();
  MC_RETURN_IF_FAILED(allocator_->InitializeSampleAllocator(10, current.Get()));

  nextDue_ = 0;
  state_ = MF_STREAM_STATE_RUNNING;
  Log(L"MediaStream: started %ux%u", width_, height_);
  MC_RETURN_IF_FAILED(queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
  return S_OK;
}

HRESULT MediaStream::Stop() {
  std::lock_guard<std::mutex> guard(lock_);
  return StopLocked();
}

HRESULT MediaStream::StopLocked() {
  if (!queue_) return MF_E_SHUTDOWN;
  if (allocator_) allocator_->UninitializeSampleAllocator();
  state_ = MF_STREAM_STATE_STOPPED;
  Log(L"MediaStream: stopped");
  MC_RETURN_IF_FAILED(queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
  return S_OK;
}

void MediaStream::Shutdown() {
  std::lock_guard<std::mutex> guard(lock_);
  if (queue_) {
    queue_->Shutdown();
    queue_.Reset();
  }
  if (allocator_) allocator_->UninitializeSampleAllocator();
  allocator_.Reset();
  descriptor_.Reset();
  source_.Reset();  // breaks the source <-> stream reference cycle
}

// ---- IMFMediaEventGenerator ----

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  if (!event) return E_POINTER;
  *event = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
  if (!event) return E_POINTER;
  *event = nullptr;
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!queue_) return MF_E_SHUTDOWN;
    queue = queue_;
  }
  return queue->GetEvent(flags, event);  // may block; don't hold the lock
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->QueueEventParamVar(met, ext, status, value);
}

// ---- IMFMediaStream ----

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
  if (!source) return E_POINTER;
  *source = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!source_) return MF_E_SHUTDOWN;
  return source_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
  if (!descriptor) return E_POINTER;
  *descriptor = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!descriptor_) return MF_E_SHUTDOWN;
  return descriptor_.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
  ComPtr<IMFVideoSampleAllocatorEx> allocator;
  UINT32 width, height;
  LONGLONG wait = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!queue_ || !allocator_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    allocator = allocator_;
    width = width_;
    height = height_;

    // Pace output to ~30 fps; consumers may ask for samples back-to-back.
    const LONGLONG now = MFGetSystemTime();
    if (nextDue_ > now) wait = nextDue_ - now;
    nextDue_ = std::max(now, nextDue_) + kFrameDuration;
  }
  if (wait > 0) Sleep(DWORD(std::min<LONGLONG>(wait, 2 * kFrameDuration) / 10'000));

  ComPtr<IMFSample> sample;
  MC_RETURN_IF_FAILED(allocator->AllocateSample(&sample));
  MC_RETURN_IF_FAILED(WriteFrame(sample.Get(), width, height));
  MC_RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
  MC_RETURN_IF_FAILED(sample->SetSampleDuration(kFrameDuration));
  if (token) MC_RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));

  std::lock_guard<std::mutex> guard(lock_);
  if (!queue_) return MF_E_SHUTDOWN;
  return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT MediaStream::WriteFrame(IMFSample* sample, UINT32 width, UINT32 height) {
  ComPtr<IMFMediaBuffer> buffer;
  MC_RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

  ComPtr<IMF2DBuffer2> buffer2d;
  if (SUCCEEDED(buffer.As(&buffer2d))) {
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    MC_RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length));
    if (pitch > 0) {
      Nv12View view{scan0, int(pitch), scan0 + size_t(pitch) * height, int(pitch), int(width), int(height)};
      frames_.Fill(view);
    }
    buffer2d->Unlock2D();
    return S_OK;
  }

  BYTE* data = nullptr;
  DWORD maxLength = 0;
  MC_RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
  const DWORD needed = width * height * 3 / 2;
  if (maxLength >= needed) {
    Nv12View view{data, int(width), data + size_t(width) * height, int(width), int(width), int(height)};
    frames_.Fill(view);
  }
  buffer->Unlock();
  return buffer->SetCurrentLength(needed);
}

// ---- IMFMediaStream2 ----

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value) {
  std::lock_guard<std::mutex> guard(lock_);
  if (value == state_) return S_OK;
  switch (value) {
    case MF_STREAM_STATE_PAUSED:
      if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
      state_ = value;
      return S_OK;
    case MF_STREAM_STATE_RUNNING:
      return StartLocked(nullptr);
    case MF_STREAM_STATE_STOPPED:
      return StopLocked();
    default:
      return MF_E_INVALID_STATE_TRANSITION;
  }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value) {
  if (!value) return E_POINTER;
  std::lock_guard<std::mutex> guard(lock_);
  *value = state_;
  return S_OK;
}

// ---- IKsControl: no camera controls exposed ----

STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace mobcam
