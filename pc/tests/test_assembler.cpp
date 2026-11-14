// AccessUnitAssembler: splitting the phone's H.264 stream into frames and reading MobCam's SEI.
// The SEI bytes come from tests/fixtures/sei.txt, which the phone's builder is tested against.
#include <string>
#include <vector>

#include "doctest.h"
#include "fixtures.h"
#include "internal.h"

using Bytes = std::vector<uint8_t>;
using fixtures::Get;
using fixtures::Hex;

namespace {

const fixtures::Values& Sei() {
  static const fixtures::Values v = fixtures::Load("sei.txt");
  return v;
}

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// A NAL with a 4-byte start code. Slices: the first byte after the header has first_mb_in_slice
// == 0 encoded as a leading 1 bit when this is the first slice of a picture.
Bytes Nal(uint8_t type, bool firstSlice = true, size_t extra = 6) {
  Bytes n{0, 0, 0, 1, uint8_t(0x60 | type)};
  if (type == 1 || type == 5) n.push_back(firstSlice ? 0x88 : 0x08);
  for (size_t i = 0; i < extra; ++i) n.push_back(uint8_t(0x11 + i));
  return n;
}

const Bytes kAud{0, 0, 0, 1, 0x09, 0xF0};  // StreamEngine ends every frame with one

Bytes SeiNal(const char* uuid, const std::string& text) {
  Bytes n{0, 0, 0, 1, 0x06, 0x05, uint8_t(16 + text.size())};
  n.insert(n.end(), uuid, uuid + 16);
  n.insert(n.end(), text.begin(), text.end());
  n.push_back(0x80);
  return n;
}

struct Collector {
  mobcam::AccessUnitAssembler assembler;
  std::vector<Bytes> units;
  std::vector<bool> keys;
  Collector() {
    assembler.onAccessUnit = [this](const Bytes& au, bool key) {
      units.push_back(au);
      keys.push_back(key);
    };
  }
};

// Three frames as the phone sends them: config + lens list + metadata with the keyframe,
// metadata with each P frame, the last one in two slices.
Bytes ThreeFrames() {
  const Bytes meta = Hex(Get(Sei(), "metadata0.nal"));
  return Cat({Nal(7), Nal(8), Hex(Get(Sei(), "lenses.nal")), meta, Nal(5), kAud, meta, Nal(1), kAud, meta, Nal(1, true),
              Nal(1, false), kAud});
}

size_t CountNals(const Bytes& au) {
  size_t n = 0;
  for (size_t i = 0; i + 3 < au.size(); ++i)
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) ++n;
  return n;
}

}  // namespace

TEST_CASE("splits frames at access unit delimiters and flags keyframes") {
  const Bytes stream = ThreeFrames();

  SUBCASE("pushed at once") {
    Collector c;
    c.assembler.Push(stream.data(), stream.size());
    REQUIRE(c.units.size() == 3);
    CHECK(c.keys == std::vector<bool>{true, false, false});
    CHECK(CountNals(c.units[0]) == 5);  // SPS, PPS, 2 SEI, IDR (the AUD is not passed on)
    CHECK(CountNals(c.units[1]) == 2);
    CHECK(CountNals(c.units[2]) == 3);  // SEI + both slices of one picture
  }
  SUBCASE("pushed one byte at a time gives the same frames") {
    Collector whole, bytes;
    whole.assembler.Push(stream.data(), stream.size());
    for (uint8_t b : stream) bytes.assembler.Push(&b, 1);
    CHECK(bytes.units == whole.units);
    CHECK(bytes.keys == whole.keys);
  }
  SUBCASE("garbage before the first start code is skipped") {
    Collector c;
    const Bytes noisy = Cat({Bytes{0x12, 0x34, 0x00, 0x56}, stream});
    c.assembler.Push(noisy.data(), noisy.size());
    CHECK(c.units.size() == 3);
  }
  SUBCASE("a frame is delivered as soon as its delimiter arrives") {
    Collector c;
    const Bytes first = Cat({Nal(7), Nal(8), Nal(5), kAud});
    c.assembler.Push(first.data(), first.size());
    CHECK(c.units.size() == 1);
  }
}

TEST_CASE("reads the phone's metadata SEI (fixture)") {
  for (const char* prefix : {"metadata0", "metadata1"}) {
    CAPTURE(prefix);
    const std::string p = prefix;
    Collector c;
    const Bytes stream = Cat({Hex(Get(Sei(), p + ".nal")), Nal(5), kAud});
    c.assembler.Push(stream.data(), stream.size());
    const auto& phone = c.assembler.phone();
    CHECK(phone.rotation == std::stoi(Get(Sei(), p + ".rotation")));
    CHECK(phone.facing == (Get(Sei(), p + ".facing") == "f" ? 1 : 0));
    CHECK(phone.targetHeight == std::stoi(Get(Sei(), p + ".height")));
    CHECK(phone.lensIndex == std::stoi(Get(Sei(), p + ".lens")));
    CHECK(phone.targetFps == std::stoi(Get(Sei(), p + ".fps")));
    CHECK(phone.maxFps == std::stoi(Get(Sei(), p + ".maxfps")));
  }
}

TEST_CASE("reads the lens list SEI (fixture)") {
  Collector c;
  const Bytes stream = Cat({Hex(Get(Sei(), "lenses.nal")), Nal(5), kAud});
  c.assembler.Push(stream.data(), stream.size());
  const auto& lenses = c.assembler.phone().lenses;
  REQUIRE(lenses.size() == 4);
  for (size_t i = 0; i < lenses.size(); ++i) {
    // "Label|zoom|b"
    const std::string entry = Get(Sei(), "lens" + std::to_string(i));
    const size_t a = entry.find('|'), b = entry.rfind('|');
    const std::string label = entry.substr(0, a);
    CAPTURE(entry);
    CHECK(lenses[i].label == std::wstring(label.begin(), label.end()));
    CHECK(lenses[i].zoom == doctest::Approx(std::stof(entry.substr(a + 1, b - a - 1))));
    CHECK(lenses[i].front == (entry.substr(b + 1) == "f"));
  }
}

TEST_CASE("older phone apps' shorter metadata SEI") {
  SUBCASE("no frame rate (before 60 fps)") {
    Collector c;
    const Bytes stream = Cat({SeiNal("MOBCAM-ROTATION!", "1b072002"), Nal(5), kAud});
    c.assembler.Push(stream.data(), stream.size());
    CHECK(c.assembler.phone().lensIndex == 2);
    CHECK(c.assembler.phone().targetFps == 0);
    CHECK(c.assembler.phone().maxFps == 0);
  }
  SUBCASE("rotation, facing and height, no lens index") {
    Collector c;
    const Bytes stream = Cat({SeiNal("MOBCAM-ROTATION!", "1b0720"), Nal(5), kAud});
    c.assembler.Push(stream.data(), stream.size());
    CHECK(c.assembler.phone().rotation == 90);
    CHECK(c.assembler.phone().facing == 0);
    CHECK(c.assembler.phone().targetHeight == 720);
    CHECK(c.assembler.phone().lensIndex == -1);
  }
  SUBCASE("rotation only") {
    Collector c;
    const Bytes stream = Cat({SeiNal("MOBCAM-ROTATION!", "2"), Nal(5), kAud});
    c.assembler.Push(stream.data(), stream.size());
    CHECK(c.assembler.phone().rotation == 180);
    CHECK(c.assembler.phone().facing == -1);
  }
  SUBCASE("someone else's SEI is ignored") {
    Collector c;
    const Bytes stream = Cat({SeiNal("SOMEBODY-ELSE!!!", "3f1080"), Nal(5), kAud});
    c.assembler.Push(stream.data(), stream.size());
    CHECK(c.assembler.phone().rotation == -1);
  }
}
