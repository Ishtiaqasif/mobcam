#include "activator.h"

#include "../common/mobcam_ids.h"

namespace mobcam {

HRESULT Activator::RuntimeClassInitialize() {
  MC_RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
  MC_RETURN_IF_FAILED(attributes_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
  MC_RETURN_IF_FAILED(attributes_->SetGUID(MFT_TRANSFORM_CLSID_Attribute, kSourceClsid));
  return S_OK;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  std::lock_guard<std::mutex> guard(lock_);
  if (!source_) {
    // Attributes set by Frame Server on the activator (client context etc.) flow to the source.
    MC_RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaSource>(&source_, attributes_.Get()));
  }
  return source_.CopyTo(riid, object);
}

STDMETHODIMP Activator::ShutdownObject() { return S_OK; }

STDMETHODIMP Activator::DetachObject() {
  std::lock_guard<std::mutex> guard(lock_);
  source_.Reset();
  return S_OK;
}

}  // namespace mobcam
