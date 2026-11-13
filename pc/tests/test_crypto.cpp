// Protocol v2 key schedule and records against tests/fixtures/protocol-v2.txt, which the phone's
// SecureChannel.kt is tested against too.
#include "doctest.h"
#include "fixtures.h"
#include "secure.h"

using fixtures::Get;
using fixtures::Hex;
using mobcam::Bytes;

namespace {

const fixtures::Values& Vectors() {
  static const fixtures::Values v = fixtures::Load("protocol-v2.txt");
  return v;
}

Bytes V(const char* key) { return Hex(Get(Vectors(), key)); }

// Splits a whole record into header and sealed part and opens it.
bool OpenRecord(mobcam::v2::RecordCipher& cipher, const Bytes& record, Bytes& plain) {
  REQUIRE(record.size() >= 4);
  return cipher.Open(record.data(), record.data() + 4, record.size() - 4, plain);
}

Bytes Plain(uint8_t type, const Bytes& payload) {
  Bytes p{type};
  p.insert(p.end(), payload.begin(), payload.end());
  return p;
}

}  // namespace

TEST_CASE("ECDH matches on both sides and returns the X coordinate big-endian") {
  const Bytes shared = V("shared_secret");
  CHECK(mobcam::v2::Ecdh(V("pc_ephemeral_private"), V("pc_ephemeral_public"), V("phone_ephemeral_public")) == shared);
  CHECK(mobcam::v2::Ecdh(V("phone_ephemeral_private"), V("phone_ephemeral_public"), V("pc_ephemeral_public")) ==
        shared);
}

TEST_CASE("ECDH rejects a point that is not on the curve") {
  Bytes bad = V("phone_ephemeral_public");
  bad[40] ^= 0x01;
  CHECK(mobcam::v2::Ecdh(V("pc_ephemeral_private"), V("pc_ephemeral_public"), bad).empty());
}

TEST_CASE("session keys and pairing code") {
  const auto keys = mobcam::v2::DeriveKeys(V("transcript_hash"), V("shared_secret"));
  CHECK(keys.pcToPhone == V("key_pc_to_phone"));
  CHECK(keys.phoneToPc == V("key_phone_to_pc"));
  CHECK(keys.code == std::stoi(Get(Vectors(), "code")));
}

TEST_CASE("sealing video records reproduces the phone's bytes") {
  mobcam::v2::RecordCipher cipher;
  REQUIRE(cipher.Init(V("key_phone_to_pc")));
  const Bytes v0 = V("video0_payload"), v1 = V("video1_payload");
  Bytes r0, r1;
  REQUIRE(cipher.Seal(0x10, v0.data(), v0.size(), r0));
  REQUIRE(cipher.Seal(0x10, v1.data(), v1.size(), r1));  // counter 1
  CHECK(r0 == V("video0_record"));
  CHECK(r1 == V("video1_record"));
}

TEST_CASE("opening records: in order, and nothing tampered or replayed") {
  const Bytes command = V("command0_record"), paired = V("paired1_record");
  Bytes plain;

  SUBCASE("in order") {
    mobcam::v2::RecordCipher cipher;
    REQUIRE(cipher.Init(V("key_pc_to_phone")));
    REQUIRE(OpenRecord(cipher, command, plain));
    CHECK(plain == Plain(0x11, V("command0_payload")));
    REQUIRE(OpenRecord(cipher, paired, plain));
    CHECK(plain == Bytes{0x12});
  }
  SUBCASE("a flipped bit fails authentication") {
    mobcam::v2::RecordCipher cipher;
    REQUIRE(cipher.Init(V("key_pc_to_phone")));
    Bytes tampered = command;
    tampered[6] ^= 0x01;
    CHECK_FALSE(OpenRecord(cipher, tampered, plain));
  }
  SUBCASE("a changed length header fails authentication") {
    mobcam::v2::RecordCipher cipher;
    REQUIRE(cipher.Init(V("key_pc_to_phone")));
    Bytes tampered = command;
    tampered[3] ^= 0x01;
    CHECK_FALSE(OpenRecord(cipher, tampered, plain));
  }
  SUBCASE("out of order (wrong counter) fails") {
    mobcam::v2::RecordCipher cipher;
    REQUIRE(cipher.Init(V("key_pc_to_phone")));
    CHECK_FALSE(OpenRecord(cipher, paired, plain));
  }
  SUBCASE("the other direction's key fails") {
    mobcam::v2::RecordCipher cipher;
    REQUIRE(cipher.Init(V("key_phone_to_pc")));
    CHECK_FALSE(OpenRecord(cipher, command, plain));
  }
}
