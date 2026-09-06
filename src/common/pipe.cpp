#include "pipe.hpp"

#include <windows.h>

#include <vector>

namespace gtabot::ipc {
namespace {

constexpr DWORD kBufferBytes  = 64 * 1024;
constexpr DWORD kReconnectMs  = 500;
// A single line can carry a whole world snapshot; anything past this means the
// peer is desynchronised and the connection is dropped rather than trusted.
constexpr std::size_t kMaxLineBytes = 8 * 1024 * 1024;

struct Overlapped {
  OVERLAPPED ov{};
  Overlapped() { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  ~Overlapped() { if (ov.hEvent) CloseHandle(ov.hEvent); }
  Overlapped(const Overlapped&)            = delete;
  Overlapped& operator=(const Overlapped&) = delete;
  void Reset() { ResetEvent(ov.hEvent); ov.Offset = 0; ov.OffsetHigh = 0; }
};

}  // namespace

Endpoint::~Endpoint() { Stop(); }

bool Endpoint::Start(MessageHandler on_message, StateHandler on_state) {
  if (thread_.joinable()) return false;
  on_message_ = std::move(on_message);
  on_state_   = std::move(on_state);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop_event_) return false;
  thread_ = std::thread(&Endpoint::Run, this);
  return true;
}

void Endpoint::Stop() {
  if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
  {
    // Unblock a peer parked in ReadFile.
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_handle_) CancelIoEx(static_cast<HANDLE>(write_handle_), nullptr);
  }
  if (thread_.joinable()) thread_.join();
  if (stop_event_) {
    CloseHandle(static_cast<HANDLE>(stop_event_));
    stop_event_ = nullptr;
  }
}

void Endpoint::Run() {
  while (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 0) !=
         WAIT_OBJECT_0) {
    HANDLE peer = static_cast<HANDLE>(AcquirePeer());
    if (peer == INVALID_HANDLE_VALUE || peer == nullptr) continue;

    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      write_handle_ = peer;
    }
    connected_.store(true, std::memory_order_release);
    if (on_state_) on_state_(true);

    PumpPeer(peer);

    connected_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      write_handle_ = nullptr;
    }
    inbox_.clear();
    if (on_state_) on_state_(false);

    DisconnectNamedPipe(peer);  // no-op on the client side
    CloseHandle(peer);
  }
}

void Endpoint::PumpPeer(void* raw) {
  HANDLE peer = static_cast<HANDLE>(raw);
  std::vector<char> buffer(kBufferBytes);
  Overlapped read;
  if (!read.ov.hEvent) return;

  for (;;) {
    read.Reset();
    DWORD got = 0;
    if (!ReadFile(peer, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                  &read.ov)) {
      if (GetLastError() != ERROR_IO_PENDING) return;
      HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), read.ov.hEvent};
      DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (which != WAIT_OBJECT_0 + 1) {
        CancelIoEx(peer, &read.ov);
        return;
      }
      if (!GetOverlappedResult(peer, &read.ov, &got, FALSE)) return;
    }
    if (got == 0) return;

    inbox_.append(buffer.data(), got);
    if (inbox_.size() > kMaxLineBytes) return;

    std::size_t start = 0;
    for (;;) {
      const std::size_t nl = inbox_.find('\n', start);
      if (nl == std::string::npos) break;
      DispatchLine(inbox_.substr(start, nl - start));
      start = nl + 1;
    }
    if (start) inbox_.erase(0, start);
  }
}

void Endpoint::DispatchLine(const std::string& line) {
  if (line.empty() || !on_message_) return;
  // A malformed line is the peer's problem, not ours: skip it and keep the
  // connection, otherwise one bad message costs the whole session.
  proto::json parsed = proto::json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) return;
  proto::Envelope env;
  try {
    env = parsed.get<proto::Envelope>();
  } catch (const std::exception&) {
    return;
  }
  on_message_(env);
}

bool Endpoint::Send(const proto::Envelope& env) {
  std::string line = proto::json(env).dump();
  line.push_back('\n');

  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!write_handle_) return false;
  HANDLE peer = static_cast<HANDLE>(write_handle_);

  Overlapped write;
  if (!write.ov.hEvent) return false;
  DWORD written = 0;
  if (!WriteFile(peer, line.data(), static_cast<DWORD>(line.size()), &written,
                 &write.ov)) {
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), write.ov.hEvent};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
      CancelIoEx(peer, &write.ov);
      return false;
    }
    if (!GetOverlappedResult(peer, &write.ov, &written, FALSE)) return false;
  }
  return written == line.size();
}

// ---------------------------------------------------------------------------

void* PipeServer::AcquirePeer() {
  HANDLE pipe = CreateNamedPipeA(
      proto::kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      /*nMaxInstances=*/1, kBufferBytes, kBufferBytes, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    WaitForSingleObject(static_cast<HANDLE>(stop_event_), kReconnectMs);
    return INVALID_HANDLE_VALUE;
  }

  Overlapped connect;
  if (!connect.ov.hEvent) {
    CloseHandle(pipe);
    return INVALID_HANDLE_VALUE;
  }

  if (!ConnectNamedPipe(pipe, &connect.ov)) {
    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), connect.ov.hEvent};
      if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &connect.ov);
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
      }
      DWORD ignored = 0;
      if (!GetOverlappedResult(pipe, &connect.ov, &ignored, FALSE)) {
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
      }
    } else if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe);
      return INVALID_HANDLE_VALUE;
    }
  }
  return pipe;
}

void* PipeClient::AcquirePeer() {
  for (;;) {
    if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 0) == WAIT_OBJECT_0)
      return INVALID_HANDLE_VALUE;

    HANDLE pipe = CreateFileA(proto::kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;

    // Server not up yet, or its single instance is taken. Either way: wait and
    // retry, so load order between the game and the MCP server never matters.
    if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), kReconnectMs) ==
        WAIT_OBJECT_0)
      return INVALID_HANDLE_VALUE;
  }
}

}  // namespace gtabot::ipc
