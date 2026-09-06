#include "mcp/http.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

#include "log.hpp"
#include "mcp/server.hpp"

namespace gtabot::mcp {
namespace {

constexpr std::size_t kMaxRequestBytes = 1u << 20;  // 1 MiB of JSON is plenty
constexpr int         kAcceptPollMs    = 200;

std::string StatusLine(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 413: return "413 Payload Too Large";
    default:  return "500 Internal Server Error";
  }
}

std::string BuildResponse(int code, const std::string& body,
                          const char* content_type = "application/json") {
  std::string out = "HTTP/1.1 " + StatusLine(code) + "\r\n";
  out += "Content-Type: ";
  out += content_type;
  out += "\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n";
  // The endpoint is loopback-only and holds no credentials, but a browser tab
  // on some page should still not be able to drive the game.
  out += "Access-Control-Allow-Origin: null\r\n";
  out += "\r\n";
  out += body;
  return out;
}

bool SendAll(SOCKET socket, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int n = send(socket, data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// Reads headers, then exactly Content-Length bytes of body.
bool ReadRequest(SOCKET socket, std::string* method, std::string* target,
                 std::string* body) {
  std::string buffer;
  char        chunk[4096];

  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const int n = recv(socket, chunk, sizeof(chunk), 0);
    if (n <= 0) return false;
    buffer.append(chunk, static_cast<std::size_t>(n));
    if (buffer.size() > kMaxRequestBytes) return false;
    header_end = buffer.find("\r\n\r\n");
  }

  const std::string head = buffer.substr(0, header_end);
  const std::size_t first_space = head.find(' ');
  const std::size_t second_space =
      first_space == std::string::npos ? std::string::npos
                                       : head.find(' ', first_space + 1);
  if (second_space == std::string::npos) return false;
  *method = head.substr(0, first_space);
  *target = head.substr(first_space + 1, second_space - first_space - 1);

  std::size_t content_length = 0;
  // Header names are case-insensitive, and clients disagree about the casing.
  std::string lowered = head;
  for (char& c : lowered) c = static_cast<char>(tolower(c));
  const std::size_t at = lowered.find("content-length:");
  if (at != std::string::npos) {
    content_length = static_cast<std::size_t>(
        strtoul(lowered.c_str() + at + 15, nullptr, 10));
  }
  if (content_length > kMaxRequestBytes) return false;

  *body = buffer.substr(header_end + 4);
  while (body->size() < content_length) {
    const int n = recv(socket, chunk, sizeof(chunk), 0);
    if (n <= 0) return false;
    body->append(chunk, static_cast<std::size_t>(n));
  }
  body->resize(content_length);
  return true;
}

}  // namespace

HttpTransport::~HttpTransport() { Stop(); }

bool HttpTransport::Start(Server* server, std::uint16_t port) {
  if (running_.load(std::memory_order_acquire)) return true;
  server_ = server;
  port_   = port;

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    last_error_ = "WSAStartup failed";
    return false;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    last_error_ = "socket() failed";
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port   = htons(port);
  // Loopback only. Never INADDR_ANY: this endpoint drives someone's game.
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    last_error_ = "port " + std::to_string(port) + " is already in use";
    closesocket(listener);
    return false;
  }
  if (listen(listener, 4) == SOCKET_ERROR) {
    last_error_ = "listen() failed";
    closesocket(listener);
    return false;
  }

  listener_ = static_cast<std::uintptr_t>(listener);
  stopping_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(&HttpTransport::Run, this);
  LOG_INFO("mcp listening on http://127.0.0.1:{}/mcp", port);
  return true;
}

void HttpTransport::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stopping_.store(true, std::memory_order_release);
  if (listener_ != ~std::uintptr_t{0}) {
    closesocket(static_cast<SOCKET>(listener_));
    listener_ = ~std::uintptr_t{0};
  }
  if (thread_.joinable()) thread_.join();
  WSACleanup();
}

void HttpTransport::Run() {
  while (!stopping_.load(std::memory_order_acquire)) {
    // A short poll rather than a blocking accept, so Stop() does not depend on
    // a connection arriving to unblock the thread.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(static_cast<SOCKET>(listener_), &readable);
    timeval timeout{0, kAcceptPollMs * 1000};
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready <= 0) continue;

    SOCKET client = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    ServeConnection(static_cast<std::uintptr_t>(client));
    closesocket(client);
  }
}

void HttpTransport::ServeConnection(std::uintptr_t handle) {
  const SOCKET client = static_cast<SOCKET>(handle);
  std::string method;
  std::string target;
  std::string body;

  if (!ReadRequest(client, &method, &target, &body)) {
    SendAll(client, BuildResponse(400, R"({"error":"malformed request"})"));
    return;
  }

  if (method == "OPTIONS") {
    SendAll(client, BuildResponse(200, ""));
    return;
  }
  if (method == "GET") {
    // A convenience for "is it up?" in a browser or with curl.
    const json info{{"name", "gtabot"},
                    {"endpoint", "/mcp"},
                    {"transport", "POST JSON-RPC 2.0"},
                    {"requests_served", requests_.load()}};
    SendAll(client, BuildResponse(200, info.dump(2)));
    return;
  }
  if (method != "POST") {
    SendAll(client, BuildResponse(405, R"({"error":"use POST"})"));
    return;
  }
  if (target != "/mcp" && target != "/") {
    SendAll(client, BuildResponse(404, R"({"error":"POST to /mcp"})"));
    return;
  }

  const json request = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (request.is_discarded()) {
    SendAll(client,
            BuildResponse(400, json{{"jsonrpc", "2.0"},
                                    {"id", nullptr},
                                    {"error", {{"code", -32700},
                                               {"message", "parse error"}}}}
                                   .dump()));
    return;
  }

  requests_.fetch_add(1, std::memory_order_relaxed);
  json response;
  if (request.is_array()) {
    // A batch: answer with an array of the responses that are not empty.
    response = json::array();
    for (const json& one : request) {
      json single = server_->Handle(one);
      if (!single.empty()) response.push_back(std::move(single));
    }
    if (response.empty()) {
      SendAll(client, BuildResponse(200, ""));
      return;
    }
  } else {
    response = server_->Handle(request);
  }

  // A notification produces no body; MCP clients accept an empty 200 for it.
  SendAll(client, BuildResponse(200, response.empty() ? "" : response.dump()));
}

}  // namespace gtabot::mcp
