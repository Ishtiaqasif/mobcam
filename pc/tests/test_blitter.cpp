// Nv12Blitter: rotation, Fit/Fill framing, no overshoot, and no writes outside the canvas.
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "nv12.h"

using mobcam::Nv12Blitter;

namespace {

// An NV12 image with padding around it, so stray writes outside the picture can be detected.
struct Image {
  static constexpr int kPad = 8;
  static constexpr uint8_t kCanary = 0xA5;
  int width, height, pitch;
  std::vector<uint8_t> y, uv;

  Image(int w, int h) : width(w), height(h), pitch(w + kPad), y(size_t(pitch) * (h + kPad), kCanary),
                        uv(size_t(pitch) * (h / 2 + kPad), kCanary) {}

  mobcam::Nv12View view() { return {y.data(), pitch, uv.data(), pitch, width, height}; }
  mobcam::Nv12ConstView cview() const { return {y.data(), pitch, uv.data(), pitch, width, height}; }
  uint8_t& Y(int x, int r) { return y[size_t(r) * pitch + x]; }
  uint8_t& U(int x, int r) { return uv[size_t(r) * pitch + 2 * x]; }
  uint8_t& V(int x, int r) { return uv[size_t(r) * pitch + 2 * x + 1]; }

  // Every padding byte still holds the canary.
  bool PaddingIntact() const {
    for (int r = 0; r < height + kPad; ++r)
      for (int c = 0; c < pitch; ++c)
        if ((r >= height || c >= width) && y[size_t(r) * pitch + c] != kCanary) return false;
    for (int r = 0; r < height / 2 + kPad; ++r)
      for (int c = 0; c < pitch; ++c)
        if ((r >= height / 2 || c >= width) && uv[size_t(r) * pitch + c] != kCanary) return false;
    return true;
  }
};

// Quadrants with distinct luma: top-left 40, top-right 80, bottom-left 120, bottom-right 160.
Image Quadrants(int w, int h) {
  Image img(w, h);
  for (int r = 0; r < h; ++r)
    for (int c = 0; c < w; ++c) img.Y(c, r) = uint8_t(40 + 40 * ((c >= w / 2) + 2 * (r >= h / 2)));
  for (int r = 0; r < h / 2; ++r)
    for (int c = 0; c < w / 2; ++c) {
      img.U(c, r) = 90;
      img.V(c, r) = 200;
    }
  return img;
}

}  // namespace

TEST_CASE("rotations put each corner where it belongs") {
  const Image src = Quadrants(32, 16);
  struct Case { int rotation; uint8_t topLeft, topRight, bottomLeft, bottomRight; };
  // Clockwise: at 90° the source's bottom-left (120) ends up top-left.
  for (const Case& k : {Case{0, 40, 80, 120, 160}, Case{90, 120, 40, 160, 80},
                        Case{180, 160, 120, 80, 40}, Case{270, 80, 160, 40, 120}}) {
    CAPTURE(k.rotation);
    const bool swapped = k.rotation % 180 != 0;
    Image dst(swapped ? 16 : 32, swapped ? 32 : 16);
    Nv12Blitter().Blit(src.cview(), dst.view(), k.rotation);
    CHECK(dst.Y(1, 1) == k.topLeft);
    CHECK(dst.Y(dst.width - 2, 1) == k.topRight);
    CHECK(dst.Y(1, dst.height - 2) == k.bottomLeft);
    CHECK(dst.Y(dst.width - 2, dst.height - 2) == k.bottomRight);
    CHECK(dst.PaddingIntact());
  }
}

TEST_CASE("Fit letterboxes with video-range black") {
  const Image src = Quadrants(32, 16);  // 2:1
  Image dst(32, 32);
  Nv12Blitter().Blit(src.cview(), dst.view(), 0, false);
  // The picture is 32x16, centred: rows 8..23. Bars above and below.
  CHECK(dst.Y(16, 0) == mobcam::kBlackY);
  CHECK(dst.Y(16, 7) == mobcam::kBlackY);
  CHECK(dst.Y(1, 9) == 40);
  CHECK(dst.Y(30, 22) == 160);
  CHECK(dst.Y(16, 24) == mobcam::kBlackY);
  CHECK(dst.U(8, 0) == mobcam::kNeutralUV);
  CHECK(dst.U(8, 8) == 90);
  CHECK(dst.V(8, 8) == 200);
  CHECK(dst.PaddingIntact());
}

TEST_CASE("Fill covers the whole canvas and crops the centre") {
  const Image src = Quadrants(32, 16);
  Image dst(16, 16);
  Nv12Blitter().Blit(src.cview(), dst.view(), 0, true);
  for (int r = 0; r < dst.height; ++r)
    for (int c = 0; c < dst.width; ++c) REQUIRE(dst.Y(c, r) != mobcam::kBlackY);
  // The middle 16 columns of the source: still all four quadrants.
  CHECK(dst.Y(1, 1) == 40);
  CHECK(dst.Y(14, 14) == 160);
  CHECK(dst.PaddingIntact());
}

TEST_CASE("bilinear filtering never overshoots the source range") {
  Image src(48, 26);
  uint32_t seed = 12345;
  for (auto& b : src.y) {
    seed = seed * 1103515245u + 12345u;
    b = uint8_t(60 + (seed >> 16) % 120);  // 60..179
  }
  for (auto& b : src.uv) b = 128;
  for (int rotation : {0, 90, 180, 270}) {
    for (bool fill : {false, true}) {
      CAPTURE(rotation);
      CAPTURE(fill);
      Image dst(40, 30);
      Nv12Blitter().Blit(src.cview(), dst.view(), rotation, fill);
      for (int r = 0; r < dst.height; ++r)
        for (int c = 0; c < dst.width; ++c) {
          const uint8_t v = dst.Y(c, r);
          REQUIRE((v == mobcam::kBlackY || (v >= 60 && v <= 179)));
        }
      CHECK(dst.PaddingIntact());
    }
  }
}

TEST_CASE("odd canvas sizes stay inside the canvas") {
  const Image src = Quadrants(32, 18);
  for (int rotation : {0, 90}) {
    for (bool fill : {false, true}) {
      CAPTURE(rotation);
      CAPTURE(fill);
      Image dst(31, 17);
      Nv12Blitter().Blit(src.cview(), dst.view(), rotation, fill);
      CHECK(dst.PaddingIntact());
    }
  }
}
