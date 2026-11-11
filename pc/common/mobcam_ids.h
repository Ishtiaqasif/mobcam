#pragma once

#include <guiddef.h>

namespace mobcam {

// COM class of the virtual camera media source (mobcam-source.dll).
// {6D3A9C1E-4B7F-4E2A-9F1C-2B8E5D7A4C31}
constexpr GUID kSourceClsid = {0x6d3a9c1e, 0x4b7f, 0x4e2a, {0x9f, 0x1c, 0x2b, 0x8e, 0x5d, 0x7a, 0x4c, 0x31}};
constexpr wchar_t kSourceClsidString[] = L"{6D3A9C1E-4B7F-4E2A-9F1C-2B8E5D7A4C31}";

constexpr wchar_t kCameraName[] = L"MobCam";

}  // namespace mobcam
