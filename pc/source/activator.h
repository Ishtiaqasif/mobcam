#pragma once

#include "common.h"
#include "media_source.h"

namespace mobcam {

// What CoCreateInstance(kSourceClsid) returns. Frame Server calls ActivateObject to get the
// actual media source.
class Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase> {
 public:
  HRESULT RuntimeClassInitialize();

  // IMFActivate
  STDMETHODIMP ActivateObject(REFIID riid, void** object) override;
  STDMETHODIMP ShutdownObject() override;
  STDMETHODIMP DetachObject() override;

  // IMFAttributes
  MC_FORWARD_ATTRIBUTES(attributes_)

 private:
  ObjectCounter counter_;
  std::mutex lock_;
  ComPtr<IMFAttributes> attributes_;
  ComPtr<MediaSource> source_;
};

}  // namespace mobcam
