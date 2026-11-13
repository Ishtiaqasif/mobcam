#include "internal.h"

#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstring>

namespace mobcam {

// ===================================================================== Annex-B parsing

namespace {

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};
constexpr char kRotationUuid[] = "MOBCAM-ROTATION!";  // 16 bytes, see StreamEngine.kt
constexpr char kLensListUuid[] = "MOBCAM-LENSLIST!";

// "Ultra-wide,0.67,b;Wide,1,b;Telephoto,3.4,b;Front,1,f"
std::vector<PhoneLens> ParseLensList(const char* text, size_t length) {
  std::vector<PhoneLens> lenses;
  std::string all(text, length);
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find(';', start);
    if (end == std::string::npos) end = all.size();
    std::string entry = all.substr(start, end - start);
    size_t c1 = entry.find(','), c2 = entry.rfind(',');
    if (c1 != std::string::npos && c2 != c1) {
      PhoneLens lens;
      std::string label = entry.substr(0, c1);
      lens.label.assign(label.begin(), label.end());
      lens.zoom = float(atof(entry.substr(c1 + 1, c2 - c1 - 1).c_str()));
      lens.front = entry.substr(c2 + 1) == "f";
      lenses.push_back(lens);
    }
    start = end + 1;
  }
  return lenses;
}

// Index of the next 00 00 01 at or after `from`, or npos.
size_t FindStartCode(const std::vector<uint8_t>& b, size_t from) {
  const size_t n = b.size();
  for (size_t i = from; i + 2 < n; ++i) {
    if (b[i + 2] > 1) {
      i += 2;
      continue;
    }  // fast skip
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) return i;
  }
  return std::string::npos;
}

}  // namespace

void AccessUnitAssembler::Reset() {
  pending_.clear();
  searched_ = 0;
  au_.clear();
  auHasSlice_ = false;
  auKey_ = false;
  phone_ = PhoneInfo{};
}

void AccessUnitAssembler::Push(const uint8_t* data, size_t length) {
  pending_.insert(pending_.end(), data, data + length);

  size_t start = FindStartCode(pending_, 0);
  if (start == std::string::npos) {
    // No sync yet: keep only a possible partial start code.
    if (pending_.size() > 3) pending_.erase(pending_.begin(), pending_.end() - 3);
    searched_ = 0;
    return;
  }

  for (;;) {
    const size_t from = std::max(start + 3, searched_ > 3 ? searched_ - 3 : 0);
    const size_t next = FindStartCode(pending_, from);
    if (next == std::string::npos) {
      searched_ = pending_.size();
      break;
    }
    // NAL = (start+3 .. next), minus trailing zeros (part of a 4-byte start code / padding).
    size_t end = next;
    while (end > start + 3 && pending_[end - 1] == 0) --end;
    if (end > start + 3) OnNal(pending_.data() + start + 3, end - (start + 3));
    start = next;
    searched_ = 0;
  }

  if (start > 0) {
    pending_.erase(pending_.begin(), pending_.begin() + start);
    searched_ = searched_ > start ? searched_ - start : 0;
  }

  // The phone ends every frame with an access unit delimiter. As soon as its header byte has
  // arrived the current picture is complete, so hand it to the decoder without waiting a frame.
  if (auHasSlice_ && pending_.size() >= 4 && (pending_[3] & 0x1F) == 9) Flush();
}

void AccessUnitAssembler::OnNal(const uint8_t* nal, size_t length) {
  const int type = nal[0] & 0x1F;
  switch (type) {
    case 9:  // access unit delimiter: always starts a new AU (the phone sends one after every frame)
    case 7:  // SPS
    case 8:  // PPS
    case 6:  // SEI
      if (auHasSlice_) Flush();
      break;
    case 1:
    case 5: {
      const bool firstSliceOfPicture = length > 1 && (nal[1] & 0x80);  // first_mb_in_slice == 0
      if (auHasSlice_ && firstSliceOfPicture) Flush();
      auHasSlice_ = true;
      if (type == 5) auKey_ = true;
      break;
    }
    default:
      break;
  }

  // MobCam SEI: payloadType 5 (user data unregistered), our UUID, then ASCII:
  //   [0] rotation '0'..'3' (x90)   [1] facing 'b'/'f'   [2..5] target height "0720"
  if (type == 6 && length >= 20 && nal[1] == 5 && nal[2] >= 17 && size_t(nal[2]) + 3 <= length &&
      memcmp(nal + 3, kRotationUuid, 16) == 0) {
    const uint8_t* info = nal + 19;
    const int infoLength = nal[2] - 16;
    const int quarter = info[0] - '0';
    if (quarter >= 0 && quarter <= 3) phone_.rotation = quarter * 90;
    if (infoLength >= 6) {
      phone_.facing = info[1] == 'f' ? 1 : (info[1] == 'b' ? 0 : -1);
      int target = 0;
      for (int i = 2; i < 6; ++i) target = target * 10 + (info[i] - '0');
      phone_.targetHeight = target;
    }
    if (infoLength >= 8) phone_.lensIndex = (info[6] - '0') * 10 + (info[7] - '0');
  }
  // Lens list (sent with keyframes)
  if (type == 6 && length >= 20 && nal[1] == 5 && nal[2] >= 16 && size_t(nal[2]) + 3 <= length &&
      memcmp(nal + 3, kLensListUuid, 16) == 0) {
    phone_.lenses = ParseLensList(reinterpret_cast<const char*>(nal + 19), size_t(nal[2]) - 16);
  }

  if (type == 9) {
    // Deliver the completed AU right away instead of waiting for the next frame's first NAL.
    return;
  }
  au_.insert(au_.end(), kStartCode, kStartCode + 4);
  au_.insert(au_.end(), nal, nal + length);
}

void AccessUnitAssembler::Flush() {
  if (auHasSlice_ && !au_.empty() && onAccessUnit) onAccessUnit(au_, auKey_);
  au_.clear();
  auHasSlice_ = false;
  auKey_ = false;
}

// ===================================================================== MF decoder

HRESULT H264Decoder::Init() {
  Close();
  HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
  if (FAILED(hr)) return hr;

  ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(mft_->GetAttributes(&attributes))) {
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
  }

  ComPtr<IMFMediaType> input;
  if (FAILED(hr = MFCreateMediaType(&input))) return hr;
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 30, 1);
  if (FAILED(hr = mft_->SetInputType(0, input.Get(), 0))) return hr;

  ConfigureOutput();  // may fail until the first SPS is seen; retried on stream change
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return S_OK;
}

void H264Decoder::Close() {
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  mft_.Reset();
  outSample_.Reset();
  outputReady_ = false;
}

HRESULT H264Decoder::ConfigureOutput() {
  outputReady_ = false;
  outSample_.Reset();
  ComPtr<IMFMediaType> type;
  HRESULT hr = S_OK;
  for (DWORD i = 0;; ++i) {
    type.Reset();
    hr = mft_->GetOutputAvailableType(0, i, &type);
    if (FAILED(hr)) return hr;
    GUID subtype{};
    if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) break;
  }
  if (FAILED(hr = mft_->SetOutputType(0, type.Get(), 0))) return hr;

  MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &codedWidth_, &codedHeight_);
  stride_ = MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, codedWidth_);
  visible_ = {0, 0, LONG(codedWidth_), LONG(codedHeight_)};
  MFVideoArea area{};
  if (SUCCEEDED(
          type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
    visible_ = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx,
                area.OffsetY.value + area.Area.cy};
  }

  MFT_OUTPUT_STREAM_INFO info{};
  if (FAILED(hr = mft_->GetOutputStreamInfo(0, &info))) return hr;
  providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  outputBufferSize_ = info.cbSize;
  outputReady_ = true;
  return S_OK;
}

HRESULT H264Decoder::Decode(const uint8_t* data, size_t length, LONGLONG timestamp, const FrameCallback& onFrame) {
  if (!mft_) return E_UNEXPECTED;

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(DWORD(length), &buffer);
  if (FAILED(hr)) return hr;
  BYTE* dst = nullptr;
  buffer->Lock(&dst, nullptr, nullptr);
  memcpy(dst, data, length);
  buffer->Unlock();
  buffer->SetCurrentLength(DWORD(length));

  ComPtr<IMFSample> sample;
  if (FAILED(hr = MFCreateSample(&sample))) return hr;
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(timestamp);
  sample->SetSampleDuration(333333);

  hr = mft_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    Drain(onFrame);
    hr = mft_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) return hr;
  return Drain(onFrame);
}

HRESULT H264Decoder::Drain(const FrameCallback& onFrame) {
  for (;;) {
    if (!outputReady_ && FAILED(ConfigureOutput())) return S_OK;  // decoder needs more data first

    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = 0;
    if (!providesSamples_) {
      // A fresh sample per output: the MS decoder rejects (E_FAIL) a reused one.
      outSample_.Reset();
      ComPtr<IMFMediaBuffer> b;
      const DWORD size = std::max<DWORD>(outputBufferSize_, codedWidth_ * codedHeight_ * 3 / 2);
      HRESULT hr = MFCreateMemoryBuffer(size, &b);
      if (FAILED(hr)) return hr;
      MFCreateSample(&outSample_);
      outSample_->AddBuffer(b.Get());
      out.pSample = outSample_.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
      // New resolution (e.g. the phone switched camera or resolution).
      outputReady_ = false;
      if (FAILED(ConfigureOutput())) return S_OK;
      continue;
    }
    if (FAILED(hr)) {
      if (providesSamples_ && out.pSample) out.pSample->Release();
      return hr;
    }

    ComPtr<IMFSample> decoded;
    if (providesSamples_)
      decoded.Attach(out.pSample);
    else
      decoded = outSample_;

    ComPtr<IMFMediaBuffer> buf;
    if (SUCCEEDED(decoded->GetBufferByIndex(0, &buf))) {
      BYTE* scan0 = nullptr;
      LONG pitch = 0;
      ComPtr<IMF2DBuffer> buf2d;
      bool locked2d = SUCCEEDED(buf.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch));
      DWORD len = 0;
      if (!locked2d) {
        buf->Lock(&scan0, nullptr, &len);
        pitch = LONG(stride_);
      }
      if (scan0 && pitch > 0) {
        const uint8_t* yPlane = scan0;
        const uint8_t* uvPlane = scan0 + size_t(pitch) * codedHeight_;
        const int x = visible_.left & ~1, y = visible_.top & ~1;
        Nv12ConstView view;
        view.y = yPlane + size_t(y) * pitch + x;
        view.yPitch = pitch;
        view.uv = uvPlane + size_t(y / 2) * pitch + x;
        view.uvPitch = pitch;
        view.width = (visible_.right - visible_.left) & ~1;
        view.height = (visible_.bottom - visible_.top) & ~1;
        if (view.width > 0 && view.height > 0) onFrame(view);
      }
      if (locked2d)
        buf2d->Unlock2D();
      else
        buf->Unlock();
    }
  }
}

}  // namespace mobcam
