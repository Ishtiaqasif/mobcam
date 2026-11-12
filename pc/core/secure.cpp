// Protocol v2 on Windows CNG: identity key in the per-user key store (NCrypt), ephemeral ECDH,
// HKDF-SHA256 and AES-256-GCM records (BCrypt). Mirrors SecureChannel.kt on the phone; the wire
// format is specified in docs/protocol.md.

#include "secure.h"

#include <bcrypt.h>
#include <ncrypt.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace mobcam {
namespace {

constexpr uint8_t kMagic[6] = {'M', 'O', 'B', 'C', 'A', 'M'};

enum : uint8_t {
  kMsgCommit = 0x01,
  kMsgHello = 0x02,
  kMsgReveal = 0x03,
  kMsgPcAuth = 0x04,
  kMsgPairing = 0x05,
  kMsgPhoneAuth = 0x06,
  kMsgDenied = 0x07,
  kRecVideo = 0x10,
  kRecCommand = 0x11,
  kRecPaired = 0x12,  // PC -> phone: this PC's user confirmed the pairing code too
};

constexpr size_t kKeyLen = 65, kSigLen = 64, kNonceLen = 32, kMaxName = 64;
constexpr uint32_t kMaxMessage = 1024;
constexpr uint32_t kMaxRecord = 4 * 1024 * 1024;
constexpr DWORD kStepTimeoutMs = 10000;
constexpr DWORD kPairingTimeoutMs = 70000;
// After the phone's Allow, the phone waits 65 s for PAIRED; leave a margin for sending it.
constexpr int kUserConfirmTimeoutMs = 60000;
constexpr DWORD kStreamTimeoutMs = 3000;  // same as ConnectTcp: detects a stalled phone

const wchar_t kIdentityKeyName[] = L"MobCam PC identity v1";
const wchar_t kPairedPhonesKey[] = L"Software\\MobCam\\PairedPhones";

Bytes Str(const char* s) { return Bytes(s, s + strlen(s)); }

Bytes Cat(std::initializer_list<const Bytes*> parts) {
  Bytes out;
  for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
  return out;
}

void PutU32(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}

uint32_t GetU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::wstring Hex(const Bytes& b) {
  static const wchar_t* digits = L"0123456789abcdef";
  std::wstring s;
  for (uint8_t v : b) {
    s += digits[v >> 4];
    s += digits[v & 15];
  }
  return s;
}

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------- BCrypt helpers

struct Algorithms {
  BCRYPT_ALG_HANDLE sha256 = nullptr, hmac = nullptr, ecdh = nullptr, ecdsa = nullptr, aesGcm = nullptr;
  bool ok = false;

  Algorithms() {
    ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
         BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
         BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0)) &&
         BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
         BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&aesGcm, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
         BCRYPT_SUCCESS(BCryptSetProperty(aesGcm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
  }
};

const Algorithms& Alg() {
  static Algorithms a;  // process lifetime; thread-safe static init
  return a;
}

Bytes Sha256(const Bytes& data) {
  Bytes out(32);
  BCryptHash(Alg().sha256, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), out.data(), 32);
  return out;
}

Bytes Hmac(const Bytes& key, const Bytes& data) {
  Bytes out(32);
  BCryptHash(Alg().hmac, (PUCHAR)key.data(), ULONG(key.size()), (PUCHAR)data.data(), ULONG(data.size()), out.data(), 32);
  return out;
}

// HKDF-Expand for up to 32 bytes.
Bytes Expand(const Bytes& prk, const char* info, size_t length) {
  Bytes in = Str(info);
  in.push_back(1);
  Bytes t = Hmac(prk, in);
  t.resize(length);
  return t;
}

bool Random(uint8_t* p, size_t n) {
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// X9.62 uncompressed point <-> BCRYPT_ECCPUBLIC_BLOB.
Bytes BlobToPoint(const Bytes& blob) {
  const auto* h = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
  if (blob.size() < sizeof(*h) + 64 || h->cbKey != 32) return {};
  Bytes p{4};
  p.insert(p.end(), blob.begin() + sizeof(*h), blob.begin() + sizeof(*h) + 64);
  return p;
}

BCRYPT_KEY_HANDLE ImportPoint(BCRYPT_ALG_HANDLE alg, ULONG magic, const uint8_t* point) {
  if (point[0] != 4) return nullptr;
  Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
  auto* h = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
  h->dwMagic = magic;
  h->cbKey = 32;
  std::copy(point + 1, point + 65, blob.begin() + sizeof(*h));
  BCRYPT_KEY_HANDLE key = nullptr;
  // CNG rejects points that are not on the curve.
  if (!BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), ULONG(blob.size()), 0)))
    return nullptr;
  return key;
}

Bytes ExportPublic(BCRYPT_KEY_HANDLE key) {
  ULONG cb = 0;
  if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cb, 0))) return {};
  Bytes blob(cb);
  if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), cb, &cb, 0))) return {};
  return BlobToPoint(blob);
}

bool VerifyEcdsa(const uint8_t* point, const Bytes& message, const uint8_t* sig) {
  BCRYPT_KEY_HANDLE key = ImportPoint(Alg().ecdsa, BCRYPT_ECDSA_PUBLIC_P256_MAGIC, point);
  if (!key) return false;
  Bytes hash = Sha256(message);
  const bool ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, hash.data(), 32, (PUCHAR)sig, ULONG(kSigLen), 0));
  BCryptDestroyKey(key);
  return ok;
}

// ---------------------------------------------------------------- PC identity (NCrypt, per user)

class PcIdentity {
 public:
  static PcIdentity& Get() {
    static PcIdentity id;
    return id;
  }

  bool Ready(std::string* error) {
    std::lock_guard<std::mutex> g(mu_);
    if (key_) return true;
    NCRYPT_PROV_HANDLE prov = 0;
    SECURITY_STATUS st = NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
    if (st != ERROR_SUCCESS) {
      if (error) *error = "cannot open the Windows key store";
      return false;
    }
    st = NCryptOpenKey(prov, &key_, kIdentityKeyName, 0, NCRYPT_SILENT_FLAG);
    if (st != ERROR_SUCCESS) {
      key_ = 0;
      st = NCryptCreatePersistedKey(prov, &key_, NCRYPT_ECDSA_P256_ALGORITHM, kIdentityKeyName, 0, 0);
      if (st == ERROR_SUCCESS) st = NCryptFinalizeKey(key_, NCRYPT_SILENT_FLAG);
      if (st != ERROR_SUCCESS && key_) {
        NCryptFreeObject(key_);
        key_ = 0;
      }
    }
    NCryptFreeObject(prov);
    if (!key_) {
      if (error) *error = "cannot create the PC identity key";
      return false;
    }
    ULONG cb = 0;
    NCryptExportKey(key_, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &cb, 0);
    Bytes blob(cb);
    if (NCryptExportKey(key_, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob.data(), cb, &cb, 0) == ERROR_SUCCESS)
      public_ = BlobToPoint(blob);
    if (public_.size() != kKeyLen) {
      if (error) *error = "cannot read the PC identity key";
      NCryptFreeObject(key_);
      key_ = 0;
      return false;
    }
    return true;
  }

  const Bytes& PublicKey() const { return public_; }

  Bytes Sign(const Bytes& message) {
    Bytes hash = Sha256(message);
    Bytes sig(kSigLen);
    DWORD cb = 0;
    std::lock_guard<std::mutex> g(mu_);
    if (NCryptSignHash(key_, nullptr, hash.data(), 32, sig.data(), DWORD(sig.size()), &cb, NCRYPT_SILENT_FLAG) !=
            ERROR_SUCCESS ||
        cb != kSigLen)
      return {};
    return sig;
  }

 private:
  std::mutex mu_;
  NCRYPT_KEY_HANDLE key_ = 0;
  Bytes public_;
};

std::string PcName() {
  wchar_t name[256];
  DWORD n = 256;
  if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n)) return "PC";
  std::string s = ToUtf8(name);
  if (s.size() > kMaxName) s.resize(kMaxName);  // may cut a UTF-8 sequence; the phone tolerates it
  return s;
}

// ---------------------------------------------------------------- paired phones (registry)
// Value name = hex SHA-256 of the identity key; data = key(65) | pairedAt int64 LE | name UTF-8.

bool IsPaired(const Bytes& key) {
  const std::wstring id = Hex(Sha256(key));
  uint8_t data[kKeyLen + 8 + 256];
  DWORD cb = sizeof(data);
  if (RegGetValueW(HKEY_CURRENT_USER, kPairedPhonesKey, id.c_str(), RRF_RT_REG_BINARY, nullptr, data, &cb) != ERROR_SUCCESS)
    return false;
  return cb >= kKeyLen && std::equal(key.begin(), key.end(), data);
}

void AddPaired(const Bytes& key, const std::string& name) {
  HKEY h = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kPairedPhonesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
    return;
  Bytes data = key;
  const int64_t now = int64_t(time(nullptr));
  for (int i = 0; i < 8; ++i) data.push_back(uint8_t(uint64_t(now) >> (8 * i)));
  data.insert(data.end(), name.begin(), name.end());
  const std::wstring id = Hex(Sha256(key));
  RegSetValueExW(h, id.c_str(), 0, REG_BINARY, data.data(), DWORD(data.size()));
  RegCloseKey(h);
}

// ---------------------------------------------------------------- AES-GCM

class Gcm {
 public:
  ~Gcm() { Clear(); }
  bool Init(const Bytes& key) {
    Clear();
    counter_ = 0;
    return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(Alg().aesGcm, &key_, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0));
  }
  void Clear() {
    if (key_) BCryptDestroyKey(key_);
    key_ = nullptr;
  }
  bool ready() const { return key_ != nullptr; }

  // header = 4-byte length (AAD). out = ciphertext ‖ tag.
  bool Seal(const uint8_t header[4], const Bytes& plain, uint8_t* out) {
    uint8_t nonce[12];
    Nonce(nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)header;
    info.cbAuthData = 4;
    info.pbTag = out + plain.size();
    info.cbTag = 16;
    ULONG cb = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(key_, (PUCHAR)plain.data(), ULONG(plain.size()), &info, nullptr, 0, out,
                                        ULONG(plain.size()), &cb, 0));
  }

  bool Open(const uint8_t header[4], const uint8_t* sealed, size_t sealedLen, Bytes& plain) {
    if (sealedLen < 17) return false;
    const size_t n = sealedLen - 16;
    plain.resize(n);
    uint8_t nonce[12];
    Nonce(nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = (PUCHAR)header;
    info.cbAuthData = 4;
    info.pbTag = (PUCHAR)sealed + n;
    info.cbTag = 16;
    ULONG cb = 0;
    return BCRYPT_SUCCESS(BCryptDecrypt(key_, (PUCHAR)sealed, ULONG(n), &info, nullptr, 0, plain.data(), ULONG(n), &cb, 0));
  }

 private:
  void Nonce(uint8_t nonce[12]) {
    std::fill(nonce, nonce + 4, uint8_t(0));
    const uint64_t c = counter_++;
    for (int i = 0; i < 8; ++i) nonce[11 - i] = uint8_t(c >> (8 * i));
  }
  BCRYPT_KEY_HANDLE key_ = nullptr;
  uint64_t counter_ = 0;
};

// ---------------------------------------------------------------- blocking socket I/O

void SetTimeout(SOCKET s, DWORD ms) {
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

bool SendAll(SOCKET s, const uint8_t* p, size_t n) {
  while (n > 0) {
    const int sent = send(s, reinterpret_cast<const char*>(p), int(std::min<size_t>(n, 1 << 20)), 0);
    if (sent <= 0) return false;
    p += sent;
    n -= size_t(sent);
  }
  return true;
}

bool RecvAll(SOCKET s, uint8_t* p, size_t n) {
  while (n > 0) {
    const int got = recv(s, reinterpret_cast<char*>(p), int(n), 0);
    if (got <= 0) return false;
    p += got;
    n -= size_t(got);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- public: paired phones

std::vector<PairedPhone> ListPairedPhones() {
  std::vector<PairedPhone> out;
  HKEY h = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kPairedPhonesKey, 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[128];
    DWORD nameLen = 128;
    uint8_t data[kKeyLen + 8 + 256];
    DWORD cb = sizeof(data), type = 0;
    const LONG rc = RegEnumValueW(h, i, name, &nameLen, nullptr, &type, data, &cb);
    if (rc == ERROR_NO_MORE_ITEMS) break;
    if (rc != ERROR_SUCCESS || type != REG_BINARY || cb < kKeyLen + 8) continue;
    PairedPhone p;
    p.id = name;
    uint64_t t = 0;
    for (int b = 0; b < 8; ++b) t |= uint64_t(data[kKeyLen + b]) << (8 * b);
    p.pairedAt = int64_t(t);
    p.name = FromUtf8(std::string(reinterpret_cast<char*>(data) + kKeyLen + 8, cb - kKeyLen - 8));
    out.push_back(p);
  }
  RegCloseKey(h);
  std::sort(out.begin(), out.end(), [](const PairedPhone& a, const PairedPhone& b) { return a.pairedAt > b.pairedAt; });
  return out;
}

bool ForgetPairedPhone(const std::wstring& id) {
  return RegDeleteKeyValueW(HKEY_CURRENT_USER, kPairedPhonesKey, id.c_str()) == ERROR_SUCCESS;
}

// ---------------------------------------------------------------- public: session

struct SecureSession::Impl {
  std::mutex sendMu;
  SOCKET socket = INVALID_SOCKET;  // guarded by sendMu after the handshake
  Gcm send, recv;
  Bytes pending;  // received bytes not yet forming a whole record
  Bytes plain;

  // Encrypts and sends one record. Caller holds sendMu.
  bool SendRecordLocked(uint8_t type, const uint8_t* payload, size_t length) {
    if (socket == INVALID_SOCKET || !send.ready()) return false;
    Bytes p;
    p.reserve(1 + length);
    p.push_back(type);
    p.insert(p.end(), payload, payload + length);
    Bytes record;
    PutU32(record, uint32_t(p.size() + 16));
    record.resize(4 + p.size() + 16);
    if (!send.Seal(record.data(), p, record.data() + 4)) return false;
    return SendAll(socket, record.data(), record.size());
  }
};

SecureSession::SecureSession() : impl_(new Impl) {}
SecureSession::~SecureSession() { delete impl_; }

void SecureSession::Reset() {
  std::lock_guard<std::mutex> g(impl_->sendMu);
  impl_->socket = INVALID_SOCKET;
  impl_->send.Clear();
  impl_->recv.Clear();
  impl_->pending.clear();
}

HandshakeResult SecureSession::Handshake(SOCKET s, bool usb, const PairingHooks& hooks, HandshakeInfo* info) {
  Reset();
  HandshakeInfo local;
  HandshakeInfo& out = info ? *info : local;
  auto fail = [&](const char* why) {
    out.error = why;
    return HandshakeResult::Failed;
  };

  if (!Alg().ok) return fail("Windows crypto (CNG) unavailable");
  PcIdentity& id = PcIdentity::Get();
  if (!id.Ready(&out.error)) return HandshakeResult::Failed;

  Bytes transcript;
  SetTimeout(s, kStepTimeoutMs);

  auto sendMessage = [&](uint8_t type, const Bytes& body, bool inTranscript) {
    Bytes msg;
    PutU32(msg, uint32_t(1 + body.size()));
    msg.push_back(type);
    msg.insert(msg.end(), body.begin(), body.end());
    if (inTranscript) transcript.insert(transcript.end(), msg.begin(), msg.end());
    return SendAll(s, msg.data(), msg.size());
  };
  // Reads one message: returns its type (0 on error) and body.
  auto readMessage = [&](Bytes& body, bool inTranscript) -> uint8_t {
    uint8_t header[4];
    if (!RecvAll(s, header, 4)) return 0;
    const uint32_t len = GetU32(header);
    if (len < 1 || len > kMaxMessage) return 0;
    Bytes msg(len);
    if (!RecvAll(s, msg.data(), len)) return 0;
    if (inTranscript) {
      transcript.insert(transcript.end(), header, header + 4);
      transcript.insert(transcript.end(), msg.begin(), msg.end());
    }
    body.assign(msg.begin() + 1, msg.end());
    return msg[0];
  };

  // Preambles.
  Bytes preamble(kMagic, kMagic + 6);
  preamble.push_back(uint8_t(kProtocolVersion >> 8));
  preamble.push_back(uint8_t(kProtocolVersion));
  transcript = preamble;
  if (!SendAll(s, preamble.data(), preamble.size())) return fail("connection lost");
  uint8_t phonePreamble[8];
  if (!RecvAll(s, phonePreamble, 8)) return fail("no answer from the phone");
  if (!std::equal(kMagic, kMagic + 6, phonePreamble)) return HandshakeResult::PhoneTooOld;  // v1 sends H.264
  out.peerVersion = (phonePreamble[6] << 8) | phonePreamble[7];
  if (out.peerVersion < kProtocolVersion) return HandshakeResult::PhoneTooOld;
  if (out.peerVersion > kProtocolVersion) return HandshakeResult::PhoneTooNew;
  transcript.insert(transcript.end(), phonePreamble, phonePreamble + 8);

  // Ephemeral key + commitment.
  BCRYPT_KEY_HANDLE eph = nullptr;
  if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(Alg().ecdh, &eph, 256, 0)) || !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(eph, 0)))
    return fail("cannot create a session key");
  struct KeyGuard {
    BCRYPT_KEY_HANDLE k;
    ~KeyGuard() {
      if (k) BCryptDestroyKey(k);
    }
  } ephGuard{eph};
  const Bytes ePc = ExportPublic(eph);
  Bytes nPc(kNonceLen);
  if (ePc.size() != kKeyLen || !Random(nPc.data(), nPc.size())) return fail("cannot create a session key");
  const Bytes commitLabel = Str("MOBCAM v2 commit");
  const Bytes commitment = Sha256(Cat({&commitLabel, &ePc, &nPc}));

  // COMMIT
  const std::string pcName = PcName();
  Bytes commit = id.PublicKey();
  commit.insert(commit.end(), commitment.begin(), commitment.end());
  commit.insert(commit.end(), pcName.begin(), pcName.end());
  if (!sendMessage(kMsgCommit, commit, true)) return fail("connection lost");

  // HELLO
  Bytes hello;
  if (readMessage(hello, true) != kMsgHello || hello.size() < kKeyLen * 2 + kNonceLen) return fail("bad HELLO from the phone");
  const Bytes phoneKey(hello.begin(), hello.begin() + kKeyLen);
  const uint8_t* ePhone = hello.data() + kKeyLen;
  std::string phoneName(hello.begin() + kKeyLen * 2 + kNonceLen, hello.end());
  if (phoneName.size() > kMaxName) phoneName.resize(kMaxName);
  out.phoneName = FromUtf8(phoneName);

  // REVEAL
  if (!sendMessage(kMsgReveal, Cat({&ePc, &nPc}), true)) return fail("connection lost");
  const Bytes h = Sha256(transcript);

  // PC_AUTH
  const Bytes pcAuthLabel = Str("MOBCAM v2 pc auth");
  const Bytes sigPc = id.Sign(Cat({&pcAuthLabel, &h}));
  if (sigPc.empty()) return fail("cannot sign with the PC identity key");
  if (!sendMessage(kMsgPcAuth, sigPc, false)) return fail("connection lost");

  // Shared secret and keys.
  BCRYPT_KEY_HANDLE peer = ImportPoint(Alg().ecdh, BCRYPT_ECDH_PUBLIC_P256_MAGIC, ePhone);
  if (!peer) return fail("bad session key from the phone");
  KeyGuard peerGuard{peer};
  BCRYPT_SECRET_HANDLE secret = nullptr;
  if (!BCRYPT_SUCCESS(BCryptSecretAgreement(eph, peer, &secret, 0))) return fail("key agreement failed");
  Bytes z(32);
  ULONG cb = 0;
  const bool derived = BCRYPT_SUCCESS(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(), 32, &cb, 0)) && cb == 32;
  BCryptDestroySecret(secret);
  if (!derived) return fail("key agreement failed");
  std::reverse(z.begin(), z.end());  // CNG returns the raw secret little-endian
  const Bytes prk = Hmac(h, z);
  SecureZeroMemory(z.data(), z.size());
  const Bytes kSend = Expand(prk, "MOBCAM v2 pc->phone", 32);
  const Bytes kRecv = Expand(prk, "MOBCAM v2 phone->pc", 32);
  const Bytes codeBytes = Expand(prk, "MOBCAM v2 code", 4);
  const int code = int(GetU32(codeBytes.data()) % 1000000u);

  // PAIRING? then PHONE_AUTH or DENIED.
  Bytes body;
  uint8_t type = readMessage(body, false);
  bool pairing = false;
  if (type == kMsgPairing) {
    pairing = true;
    if (hooks.onPairing) hooks.onPairing(code);
    SetTimeout(s, kPairingTimeoutMs);
    type = readMessage(body, false);
  }
  if (type == kMsgDenied) {
    out.deniedReason = body.empty() ? 0 : body[0];
    return HandshakeResult::Denied;
  }
  if (type != kMsgPhoneAuth || body.size() != kSigLen) return fail(pairing ? "pairing timed out" : "bad answer from the phone");
  const Bytes phoneAuthLabel = Str("MOBCAM v2 phone auth");
  if (!VerifyEcdsa(phoneKey.data(), Cat({&phoneAuthLabel, &h}), body.data())) return fail("the phone's signature is invalid");

  // Trust. A new phone needs this PC's user to confirm the same code too: otherwise any device on
  // the network that answers discovery could pair itself with this PC and feed it video.
  if (!usb) {
    if (pairing) {
      if (hooks.onPhoneAllowed) hooks.onPhoneAllowed();
      if (!hooks.waitForUser || !hooks.waitForUser(kUserConfirmTimeoutMs)) return HandshakeResult::Cancelled;
      AddPaired(phoneKey, phoneName);
      out.newlyPaired = true;
    } else if (!IsPaired(phoneKey)) {
      return HandshakeResult::NotPaired;
    }
  }

  SetTimeout(s, kStreamTimeoutMs);
  std::lock_guard<std::mutex> g(impl_->sendMu);
  if (!impl_->send.Init(kSend) || !impl_->recv.Init(kRecv)) return fail("cannot set up encryption");
  impl_->socket = s;
  // Tells the phone both users confirmed; it remembers this PC and starts the video only then.
  if (out.newlyPaired && !impl_->SendRecordLocked(kRecPaired, nullptr, 0)) return fail("connection lost");
  return HandshakeResult::Ok;
}

bool SecureSession::Receive(const uint8_t* data, size_t length, const std::function<void(const uint8_t*, size_t)>& onVideo) {
  Impl& m = *impl_;
  if (!m.recv.ready()) return false;
  m.pending.insert(m.pending.end(), data, data + length);
  size_t pos = 0;
  while (m.pending.size() - pos >= 4) {
    const uint8_t* header = m.pending.data() + pos;
    const uint32_t len = GetU32(header);
    if (len < 17 || len > kMaxRecord) return false;
    if (m.pending.size() - pos - 4 < len) break;
    if (!m.recv.Open(header, header + 4, len, m.plain)) return false;
    if (!m.plain.empty() && m.plain[0] == kRecVideo) onVideo(m.plain.data() + 1, m.plain.size() - 1);
    pos += 4 + len;  // other record types: ignore (newer phones)
  }
  m.pending.erase(m.pending.begin(), m.pending.begin() + pos);
  return true;
}

bool SecureSession::SendCommand(const std::string& command) {
  std::lock_guard<std::mutex> g(impl_->sendMu);
  return impl_->SendRecordLocked(kRecCommand, reinterpret_cast<const uint8_t*>(command.data()), command.size());
}

}  // namespace mobcam
