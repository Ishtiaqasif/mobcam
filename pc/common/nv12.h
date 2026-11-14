// Minimal NV12 helpers: fill, and a bilinear scale+rotate+letterbox blit.
// Header-only so both the DLL and the client can use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mobcam {

struct Nv12View {
  uint8_t* y = nullptr;
  int yPitch = 0;
  uint8_t* uv = nullptr;
  int uvPitch = 0;
  int width = 0;   // luma width
  int height = 0;  // luma height
};

struct Nv12ConstView {
  const uint8_t* y = nullptr;
  int yPitch = 0;
  const uint8_t* uv = nullptr;
  int uvPitch = 0;
  int width = 0;
  int height = 0;
};

// Video-range black.
constexpr uint8_t kBlackY = 16;
constexpr uint8_t kNeutralUV = 128;

inline void FillNv12(const Nv12View& d, uint8_t y, uint8_t u, uint8_t v) {
  for (int r = 0; r < d.height; ++r) memset(d.y + size_t(r) * d.yPitch, y, d.width);
  for (int r = 0; r < d.height / 2; ++r) {
    uint8_t* row = d.uv + size_t(r) * d.uvPitch;
    for (int c = 0; c < d.width / 2; ++c) {
      row[2 * c] = u;
      row[2 * c + 1] = v;
    }
  }
}

inline void CopyNv12(const Nv12ConstView& s, const Nv12View& d) {
  for (int r = 0; r < d.height; ++r) memcpy(d.y + size_t(r) * d.yPitch, s.y + size_t(r) * s.yPitch, d.width);
  for (int r = 0; r < d.height / 2; ++r) memcpy(d.uv + size_t(r) * d.uvPitch, s.uv + size_t(r) * s.uvPitch, d.width);
}

namespace detail {

// Maps each destination index along one axis to two source indices and a 0..256 weight.
struct Axis {
  std::vector<int> i0, i1;
  std::vector<int> w;  // weight of i1, 0..256
};

// Maps dstCount output samples onto the source window [srcStart, srcStart + srcSpan).
inline void BuildAxis(Axis& a, int dstCount, int srcCount, bool reverse, double srcStart, double srcSpan) {
  a.i0.resize(dstCount);
  a.i1.resize(dstCount);
  a.w.resize(dstCount);
  const double scale = srcSpan / double(dstCount);
  for (int d = 0; d < dstCount; ++d) {
    double s = srcStart + (d + 0.5) * scale - 0.5;
    if (reverse) s = (srcCount - 1) - s;
    s = std::clamp(s, 0.0, double(srcCount - 1));
    int i = int(std::floor(s));
    a.i0[d] = i;
    a.i1[d] = std::min(i + 1, srcCount - 1);
    a.w[d] = int((s - i) * 256.0 + 0.5);
  }
}

}  // namespace detail

// Rotates `src` clockwise by `rotation` degrees (0/90/180/270) and scales it into `dst`
// preserving aspect ratio. Fit: whole picture, black borders. Fill: covers `dst`, cropping the
// overflow (centered). Bilinear filtering.
//
// Caches its lookup tables between calls; keep one instance per stream and call Blit() per frame.
class Nv12Blitter {
 public:
  void Blit(const Nv12ConstView& src, const Nv12View& dst, int rotation, bool fill = false) {
    rotation = ((rotation % 360) + 360) % 360;
    if (rotation % 90 != 0) rotation = 0;
    if (src.width != srcW_ || src.height != srcH_ || dst.width != dstW_ || dst.height != dstH_ ||
        rotation != rotation_ || fill != fill_) {
      Prepare(src.width, src.height, dst.width, dst.height, rotation, fill);
    }

    // Borders (only when the picture doesn't cover the full canvas).
    if (outW_ != dstW_ || outH_ != dstH_) FillNv12(dst, kBlackY, kNeutralUV, kNeutralUV);

    const bool swapped = rotation_ == 90 || rotation_ == 270;

    // Luma
    for (int dy = 0; dy < outH_; ++dy) {
      uint8_t* out = dst.y + size_t(dy + outY_) * dst.yPitch + outX_;
      const int ra0 = yRow_.i0[dy], ra1 = yRow_.i1[dy], rw = yRow_.w[dy];
      if (!swapped) {
        const uint8_t* r0 = src.y + size_t(ra0) * src.yPitch;
        const uint8_t* r1 = src.y + size_t(ra1) * src.yPitch;
        for (int dx = 0; dx < outW_; ++dx) {
          const int x0 = yCol_.i0[dx], x1 = yCol_.i1[dx], fx = yCol_.w[dx];
          const int top = r0[x0] * (256 - fx) + r0[x1] * fx;
          const int bot = r1[x0] * (256 - fx) + r1[x1] * fx;
          out[dx] = uint8_t((top * (256 - rw) + bot * rw + 32768) >> 16);
        }
      } else {
        // Row axis selects source columns, column axis selects source rows.
        const int x0 = ra0, x1 = ra1, fx = rw;
        for (int dx = 0; dx < outW_; ++dx) {
          const uint8_t* r0 = src.y + size_t(yCol_.i0[dx]) * src.yPitch;
          const uint8_t* r1 = src.y + size_t(yCol_.i1[dx]) * src.yPitch;
          const int fy = yCol_.w[dx];
          const int top = r0[x0] * (256 - fx) + r0[x1] * fx;
          const int bot = r1[x0] * (256 - fx) + r1[x1] * fx;
          out[dx] = uint8_t((top * (256 - fy) + bot * fy + 32768) >> 16);
        }
      }
    }

    // Chroma (interleaved UV at half resolution)
    const int cw = outW_ / 2, ch = outH_ / 2;
    for (int dy = 0; dy < ch; ++dy) {
      uint8_t* out = dst.uv + size_t(dy + outY_ / 2) * dst.uvPitch + outX_;
      const int ra0 = cRow_.i0[dy], ra1 = cRow_.i1[dy], rw = cRow_.w[dy];
      for (int dx = 0; dx < cw; ++dx) {
        int x0, x1, fx, y0, y1, fy;
        if (!swapped) {
          x0 = cCol_.i0[dx];
          x1 = cCol_.i1[dx];
          fx = cCol_.w[dx];
          y0 = ra0;
          y1 = ra1;
          fy = rw;
        } else {
          x0 = ra0;
          x1 = ra1;
          fx = rw;
          y0 = cCol_.i0[dx];
          y1 = cCol_.i1[dx];
          fy = cCol_.w[dx];
        }
        const uint8_t* r0 = src.uv + size_t(y0) * src.uvPitch;
        const uint8_t* r1 = src.uv + size_t(y1) * src.uvPitch;
        for (int c = 0; c < 2; ++c) {
          const int top = r0[2 * x0 + c] * (256 - fx) + r0[2 * x1 + c] * fx;
          const int bot = r1[2 * x0 + c] * (256 - fx) + r1[2 * x1 + c] * fx;
          out[2 * dx + c] = uint8_t((top * (256 - fy) + bot * fy + 32768) >> 16);
        }
      }
    }
  }

 private:
  void Prepare(int sw, int sh, int dw, int dh, int rotation, bool fill) {
    srcW_ = sw;
    srcH_ = sh;
    dstW_ = dw;
    dstH_ = dh;
    rotation_ = rotation;
    fill_ = fill;
    const bool swapped = rotation == 90 || rotation == 270;
    const int rw = swapped ? sh : sw;  // rotated picture size
    const int rh = swapped ? sw : sh;

    // Visible window of the rotated picture (in rotated pixels) along x and y.
    double viewX0 = 0, viewXSpan = rw, viewY0 = 0, viewYSpan = rh;
    if (fill) {
      const double scale = std::max(double(dw) / rw, double(dh) / rh);
      outW_ = dw & ~1;
      outH_ = dh & ~1;
      outX_ = outY_ = 0;
      viewXSpan = dw / scale;
      viewYSpan = dh / scale;
      viewX0 = (rw - viewXSpan) / 2;
      viewY0 = (rh - viewYSpan) / 2;
    } else {
      const double scale = std::min(double(dw) / rw, double(dh) / rh);
      outW_ = std::max(2, int(rw * scale) & ~1);
      outH_ = std::max(2, int(rh * scale) & ~1);
      outW_ = std::min(outW_, dw & ~1);
      outH_ = std::min(outH_, dh & ~1);
      outX_ = ((dw - outW_) / 2) & ~1;
      outY_ = ((dh - outH_) / 2) & ~1;
    }

    // Output columns walk the rotated x axis; output rows walk the rotated y axis.
    //   rot   rotated-x -> source           rotated-y -> source
    //   0     sx = rx                       sy = ry
    //   90    sy = sh-1-rx                  sx = ry
    //   180   sx = sw-1-rx                  sy = sh-1-ry
    //   270   sy = rx                       sx = sw-1-ry
    int colSrc, rowSrc;
    bool colRev, rowRev;
    switch (rotation) {
      case 90:
        colSrc = sh;
        colRev = true;
        rowSrc = sw;
        rowRev = false;
        break;
      case 180:
        colSrc = sw;
        colRev = true;
        rowSrc = sh;
        rowRev = true;
        break;
      case 270:
        colSrc = sh;
        colRev = false;
        rowSrc = sw;
        rowRev = true;
        break;
      default:
        colSrc = sw;
        colRev = false;
        rowSrc = sh;
        rowRev = false;
        break;
    }
    detail::BuildAxis(yCol_, outW_, colSrc, colRev, viewX0, viewXSpan);
    detail::BuildAxis(yRow_, outH_, rowSrc, rowRev, viewY0, viewYSpan);
    detail::BuildAxis(cCol_, outW_ / 2, colSrc / 2, colRev, viewX0 / 2, viewXSpan / 2);
    detail::BuildAxis(cRow_, outH_ / 2, rowSrc / 2, rowRev, viewY0 / 2, viewYSpan / 2);
  }

  int srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0, rotation_ = -1;
  bool fill_ = false;
  int outW_ = 0, outH_ = 0, outX_ = 0, outY_ = 0;
  detail::Axis yCol_, yRow_, cCol_, cRow_;
};

}  // namespace mobcam
