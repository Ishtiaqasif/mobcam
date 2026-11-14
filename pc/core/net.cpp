#include "internal.h"

#include <iphlpapi.h>

#include <algorithm>
#include <set>

namespace mobcam {

bool InitWinsock() {
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

// ------------------------------------------------------------------ discovery

namespace {

// Directed broadcast address of every up IPv4 interface (plus 255.255.255.255), so the probe
// reaches the phone even with VPN / Hyper-V adapters present.
std::vector<in_addr> BroadcastAddresses() {
  std::vector<in_addr> out;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  }
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) == NO_ERROR) {
    for (auto* a = addrs; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
        const ULONG prefix = u->OnLinkPrefixLength;
        if (prefix == 0 || prefix >= 32) continue;
        const ULONG ip = ntohl(sin->sin_addr.s_addr);
        const ULONG mask = 0xFFFFFFFFu << (32 - prefix);  // prefix is 1..31 here
        in_addr bcast;
        bcast.s_addr = htonl(ip | ~mask);
        out.push_back(bcast);
      }
    }
  }
  in_addr all;
  all.s_addr = INADDR_BROADCAST;
  out.push_back(all);
  return out;
}

}  // namespace

std::vector<PhoneAddress> DiscoverPhones(int timeoutMs, bool stopAtFirst) {
  std::vector<PhoneAddress> found;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return found;
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

  static const char kProbe[] = "MOBCAM_DISCOVER";
  auto sendProbes = [&] {
    for (const auto& b : BroadcastAddresses()) {
      sockaddr_in to{};
      to.sin_family = AF_INET;
      to.sin_port = htons(kDiscoveryPort);
      to.sin_addr = b;
      sendto(s, kProbe, int(sizeof(kProbe) - 1), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
  };

  std::set<std::string> seen;
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  ULONGLONG nextProbe = 0;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) break;
    if (now >= nextProbe) {
      sendProbes();
      nextProbe = now + 400;
    }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    const long waitMs = long(std::min<ULONGLONG>(deadline - now, nextProbe - now));
    timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;

    char reply[256];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int n = recvfrom(s, reply, sizeof(reply) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) continue;
    reply[n] = 0;
    // "MOBCAM_HERE <port> <name...>"
    if (strncmp(reply, "MOBCAM_HERE ", 12) != 0) continue;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    if (!seen.insert(ip).second) continue;

    PhoneAddress p;
    p.host = ip;
    char* rest = reply + 12;
    p.port = uint16_t(strtoul(rest, &rest, 10));
    if (p.port == 0) p.port = kStreamPort;
    while (*rest == ' ') ++rest;
    p.name = rest;
    found.push_back(p);
    if (stopAtFirst) break;
  }
  closesocket(s);
  return found;
}

// ------------------------------------------------------------------ TCP

SOCKET ConnectTcp(const std::string& host, uint16_t port, int timeoutMs, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || !result) {
    if (error) *error = "cannot resolve " + host;
    return INVALID_SOCKET;
  }

  SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(result);
    if (error) *error = "socket() failed";
    return INVALID_SOCKET;
  }

  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);
  int rc = connect(s, result->ai_addr, int(result->ai_addrlen));
  freeaddrinfo(result);
  if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    rc = select(0, nullptr, &wr, &ex, &tv) == 1 && FD_ISSET(s, &wr) ? 0 : SOCKET_ERROR;
  }
  if (rc != 0) {
    closesocket(s);
    if (error) *error = "connection to " + host + ":" + std::to_string(port) + " failed";
    return INVALID_SOCKET;
  }
  nonBlocking = 0;
  ioctlsocket(s, FIONBIO, &nonBlocking);

  BOOL noDelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
  int rcvbuf = 1 << 20;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
  DWORD recvTimeout = 3000;  // detects a stalled phone
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));
  return s;
}

// ------------------------------------------------------------------ adb (USB)

namespace {

bool FileExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs a command line hidden, returning its exit code and stdout+stderr.
bool RunCapture(const std::wstring& commandLine, std::string* output, DWORD* exitCode, DWORD timeoutMs) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = commandLine;
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writePipe);
  if (!ok) {
    CloseHandle(readPipe);
    return false;
  }

  std::string out;
  char chunk[4096];
  DWORD n = 0;
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      if (ReadFile(readPipe, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) && n) out.append(chunk, n);
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      while (ReadFile(readPipe, chunk, sizeof(chunk), &n, nullptr) && n) out.append(chunk, n);
      break;
    }
    if (GetTickCount64() > deadline) {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(readPipe);
  if (output) *output = out;
  if (exitCode) *exitCode = code;
  return true;
}

}  // namespace

std::wstring FindAdb() {
  wchar_t found[MAX_PATH];
  if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;

  wchar_t env[MAX_PATH];
  for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
    if (GetEnvironmentVariableW(var, env, MAX_PATH)) {
      std::wstring p = std::wstring(env) + L"\\platform-tools\\adb.exe";
      if (FileExists(p)) return p;
    }
  }
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) {
    std::wstring p = std::wstring(env) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
    if (FileExists(p)) return p;
  }
  return {};
}

bool AdbHasDevice(const std::wstring& adb, std::string* deviceName) {
  std::string out;
  DWORD code = 1;
  if (!RunCapture(L"\"" + adb + L"\" devices -l", &out, &code, 15000) || code != 0) return false;
  size_t pos = 0;
  while ((pos = out.find('\n', pos)) != std::string::npos) {
    ++pos;
    size_t end = out.find('\n', pos);
    std::string line = out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (line.find(" device ") != std::string::npos || line.find("\tdevice") != std::string::npos) {
      if (deviceName) {
        size_t m = line.find("model:");
        *deviceName = m == std::string::npos ? line.substr(0, line.find_first_of(" \t"))
                                             : line.substr(m + 6, line.find(' ', m) - m - 6);
      }
      return true;
    }
  }
  return false;
}

bool AdbForward(const std::wstring& adb, uint16_t port, std::string* error) {
  std::string out;
  DWORD code = 1;
  const std::wstring p = std::to_wstring(port);
  if (!RunCapture(L"\"" + adb + L"\" forward tcp:" + p + L" tcp:" + p, &out, &code, 15000)) {
    if (error) *error = "could not run adb";
    return false;
  }
  if (code != 0) {
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (error) *error = out.empty() ? "adb forward failed" : out;
    return false;
  }
  return true;
}

}  // namespace mobcam
