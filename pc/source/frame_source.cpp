#include "common.h"

#include <sddl.h>

#include "../common/shared_frame.h"
#include "frame_source.h"

namespace mobcam {

FrameSource::~FrameSource() {
  if (view_) UnmapViewOfFile(view_);
  if (mapping_) CloseHandle(mapping_);
}

bool FrameSource::EnsureMapping() {
  if (view_) return true;
  const auto now = GetTickCount64();
  if (lastAttemptTick_ && now - lastAttemptTick_ < 1000) return false;
  lastAttemptTick_ = now;

  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kMappingSddl, SDDL_REVISION_1, &sd, nullptr)) {
    sa.lpSecurityDescriptor = sd;
  }

  // Frame Server runs in session 0, so it may create Global\ objects. Other processes that
  // load this DLL (the app, the monitor service) fall back to opening an existing mapping.
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(kMappingBytes >> 32),
                                DWORD(kMappingBytes & 0xFFFFFFFF), kMappingName);
  const DWORD createError = GetLastError();
  if (sd) LocalFree(sd);

  bool created = h && createError != ERROR_ALREADY_EXISTS;
  if (!h) h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
  if (!h) {
    if (!loggedState_) {
      Log(L"FrameSource: shared memory unavailable (create error %lu, open error %lu)", createError, GetLastError());
      loggedState_ = true;
    }
    return false;
  }

  void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingBytes);
  if (!view) {
    Log(L"FrameSource: MapViewOfFile failed %lu", GetLastError());
    CloseHandle(h);
    return false;
  }
  if (created) InitHeader(static_cast<SharedHeader*>(view));

  mapping_ = h;
  view_ = view;
  Log(L"FrameSource: shared memory %s", created ? L"created" : L"opened");
  return true;
}

void FrameSource::Fill(const Nv12View& dst) {
  std::lock_guard<std::mutex> guard(lock_);

  if (EnsureMapping()) {
    auto* header = static_cast<SharedHeader*>(view_);
    InterlockedExchange64(&header->lastReadTick, LONG64(GetTickCount64()));  // "an app is watching"
    const LONG slot = header->latestSlot;
    const auto age = GetTickCount64() - static_cast<unsigned long long>(header->lastWriteTick);
    if (HeaderValid(header) && slot >= 0 && slot < LONG(kSlotCount) && age < kStaleMs) {
      if (!hadSignal_) {
        Log(L"FrameSource: receiving frames");
        hadSignal_ = true;
      }
      uint8_t* base = SlotPtr(view_, uint32_t(slot));
      Nv12ConstView src{base,
                        int(kCanvasWidth),
                        base + size_t(kCanvasWidth) * kCanvasHeight,
                        int(kCanvasWidth),
                        int(kCanvasWidth),
                        int(kCanvasHeight)};
      if (dst.width == int(kCanvasWidth) && dst.height == int(kCanvasHeight)) {
        CopyNv12(src, dst);
      } else {
        blitter_.Blit(src, dst, 0);
      }
      return;
    }
  }

  if (hadSignal_) {
    Log(L"FrameSource: signal lost");
    hadSignal_ = false;
  }
  if (noSignalW_ != dst.width || noSignalH_ != dst.height) RenderNoSignal(dst.width, dst.height);
  const uint8_t* y = noSignal_.data();
  Nv12ConstView src{y, dst.width, y + size_t(dst.width) * dst.height, dst.width, dst.width, dst.height};
  CopyNv12(src, dst);
}

// Draws the "waiting" card once per output size with GDI and converts it to NV12.
void FrameSource::RenderNoSignal(int width, int height) {
  noSignalW_ = width;
  noSignalH_ = height;
  noSignal_.assign(size_t(width) * height * 3 / 2, 0);
  Nv12View out{noSignal_.data(), width, noSignal_.data() + size_t(width) * height, width, width, height};
  FillNv12(out, 30, 128, 128);

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  HDC dc = CreateCompatibleDC(nullptr);
  void* bits = nullptr;
  HBITMAP bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!dc || !bmp || !bits) {
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return;  // plain dark frame
  }
  HGDIOBJ oldBmp = SelectObject(dc, bmp);

  RECT all{0, 0, width, height};
  HBRUSH bg = CreateSolidBrush(RGB(18, 20, 24));
  FillRect(dc, &all, bg);
  DeleteObject(bg);
  SetBkMode(dc, TRANSPARENT);

  auto drawLine = [&](const wchar_t* text, int fontHeight, int weight, COLORREF color, int centerY) {
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT r{0, centerY - fontHeight, width, centerY + fontHeight};
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
  };
  drawLine(L"MobCam", height / 9, FW_BOLD, RGB(240, 242, 245), height * 42 / 100);
  drawLine(L"Waiting for your phone — start mobcam.exe on this PC", height / 26, FW_NORMAL, RGB(150, 156, 166),
           height * 58 / 100);
  GdiFlush();

  // BGRA -> NV12 (BT.601, video range)
  const auto* px = static_cast<const uint8_t*>(bits);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = px + (size_t(y) * width + x) * 4;
      const int b = p[0], g = p[1], r = p[2];
      out.y[size_t(y) * width + x] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
      if ((x & 1) == 0 && (y & 1) == 0) {
        uint8_t* uv = out.uv + size_t(y / 2) * width + x;
        uv[0] = uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        uv[1] = uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
      }
    }
  }

  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

}  // namespace mobcam
