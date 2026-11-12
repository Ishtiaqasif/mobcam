// Protocol v2: pairing, authentication and encrypted records (docs/protocol.md), on Windows CNG.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mobcam {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kProtocolVersion = 2;

// ---------------------------------------------------------------- trusted phones (per Windows user)

struct PairedPhone {
  std::wstring id;    // hex SHA-256 of the phone's identity key
  std::wstring name;
  int64_t pairedAt = 0;  // Unix seconds
};

std::vector<PairedPhone> ListPairedPhones();
bool ForgetPairedPhone(const std::wstring& id);

// ---------------------------------------------------------------- session

enum class HandshakeResult {
  Ok,
  PhoneTooOld,   // v1 phone app (no preamble) or an older protocol version
  PhoneTooNew,   // phone speaks a newer protocol: update the PC app
  Denied,        // phone sent DENIED, see deniedReason
  NotPaired,     // Wi-Fi phone skipped pairing but this PC doesn't know it
  Cancelled,     // this PC's user cancelled pairing or didn't confirm in time
  Failed,        // I/O error, timeout, bad signature...
};

// Two-sided pairing: the phone's user taps Allow and this PC's user confirms the same code.
struct PairingHooks {
  std::function<void(int code)> onPairing;        // the phone started asking its user; show the code
  std::function<void()> onPhoneAllowed;           // the phone's user tapped Allow
  std::function<bool(int timeoutMs)> waitForUser; // blocks until this PC's user answers; true = codes match
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
