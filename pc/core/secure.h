// Protocol v2: pairing, authentication and encrypted records (docs/protocol.md), on Windows CNG.
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mobcam {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kProtocolVersion = 2;

// ---------------------------------------------------------------- protocol v2 building blocks
// The deterministic parts of the key exchange and the record layer, used by SecureSession and
// tested against tests/fixtures/protocol-v2.txt (pc/tests), which the phone is tested against too.
namespace v2 {

struct SessionKeys {
  Bytes pcToPhone;
  Bytes phoneToPc;
  int code = 0;  // pairing code shown on both screens, 0..999999
};

// HKDF over the ECDH secret (X coordinate, big-endian) keyed with the transcript hash.
SessionKeys DeriveKeys(const Bytes& transcriptHash, const Bytes& sharedSecret);

// ECDH with a given key pair (tests): X coordinate of the shared point, big-endian; empty on error.
Bytes Ecdh(const Bytes& privateScalar, const Bytes& publicPoint, const Bytes& peerPoint);

// AES-256-GCM records: 4-byte big-endian length (also the AAD), then ciphertext and tag. The
// nonce is the record counter, so each direction needs its own instance. Not thread-safe.
class RecordCipher {
 public:
  RecordCipher() = default;
  ~RecordCipher() { Clear(); }
  RecordCipher(const RecordCipher&) = delete;
  RecordCipher& operator=(const RecordCipher&) = delete;

  bool Init(const Bytes& key);  // restarts the counter
  void Clear();
  bool ready() const { return key_ != nullptr; }

  // Appends one whole record (header included) carrying type ‖ payload.
  bool Seal(uint8_t type, const uint8_t* payload, size_t length, Bytes& record);
  // sealed = the record after its header; plain = type ‖ payload. False if authentication fails.
  bool Open(const uint8_t header[4], const uint8_t* sealed, size_t sealedLen, Bytes& plain);

 private:
  void Nonce(uint8_t nonce[12]);
  BCRYPT_KEY_HANDLE key_ = nullptr;
  uint64_t counter_ = 0;
};

}  // namespace v2

// ---------------------------------------------------------------- trusted phones (per Windows user)

struct PairedPhone {
  std::wstring id;  // hex SHA-256 of the phone's identity key
  std::wstring name;
  int64_t pairedAt = 0;  // Unix seconds
};

std::vector<PairedPhone> ListPairedPhones();
bool ForgetPairedPhone(const std::wstring& id);

// ---------------------------------------------------------------- session

enum class HandshakeResult {
  Ok,
  PhoneTooOld,  // v1 phone app (no preamble) or an older protocol version
  PhoneTooNew,  // phone speaks a newer protocol: update the PC app
  Denied,       // phone sent DENIED, see deniedReason
  NotPaired,    // Wi-Fi phone skipped pairing but this PC doesn't know it
  Cancelled,    // this PC's user cancelled pairing or didn't confirm in time
  Failed,       // I/O error, timeout, bad signature...
};

// Two-sided pairing: the phone's user taps Allow and this PC's user confirms the same code.
struct PairingHooks {
  std::function<void(int code)> onPairing;         // the phone started asking its user; show the code
  std::function<void()> onPhoneAllowed;            // the phone's user tapped Allow
  std::function<bool(int timeoutMs)> waitForUser;  // blocks until this PC's user answers; true = codes match
};

// DENIED reasons from the phone.
enum : int { kDeniedByUser = 1, kDeniedTimeout = 2, kDeniedBusy = 3, kDeniedBadPc = 4 };

struct HandshakeInfo {
  std::wstring phoneName;
  int deniedReason = 0;
  int peerVersion = 0;
  bool newlyPaired = false;
  std::string error;  // for Failed
};

// One phone connection after TCP connect. Handshake() and Receive() run on the connection worker;
// SendCommand() may be called from any thread.
class SecureSession {
 public:
  SecureSession();
  ~SecureSession();
  SecureSession(const SecureSession&) = delete;
  SecureSession& operator=(const SecureSession&) = delete;

  // Blocking. For a new phone on Wi-Fi it calls the pairing hooks and only trusts the phone (and
  // sends the PAIRED record) after this PC's user confirmed the code too.
  HandshakeResult Handshake(SOCKET s, bool usb, const PairingHooks& pairing, HandshakeInfo* info);

  // Feeds bytes from the socket; calls onVideo for every decrypted VIDEO record.
  // Returns false if a record is malformed or fails authentication (drop the connection).
  bool Receive(const uint8_t* data, size_t length, const std::function<void(const uint8_t*, size_t)>& onVideo);

  // Encrypts and sends one COMMAND record. False if not connected or the send failed.
  bool SendCommand(const std::string& command);

  // Forgets keys and the socket (does not close it).
  void Reset();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace mobcam
