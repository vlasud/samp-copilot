#include "server.hpp"

#include <cstdio>
#include <iostream>
#include <utility>

namespace gtabot::mcp {
namespace {

// The revision this server implements. Reported back verbatim during
// initialize; a client asking for something else still gets this, which is the
// documented behaviour when the requested revision is unsupported.
constexpr const char* kProtocolVersion = "2025-06-18";

constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;

void WriteLine(const json& j) {
  // A single write, then flush: clients read line by line and a partial line
  // stalls the session.
  const std::string line = j.dump() + "\n";
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(stdout);
}

}  // namespace

Server::Server(std::string name, std::string version)
    : name_(std::move(name)), version_(std::move(version)) {}

void Server::AddTool(Tool tool) {
  const std::string key = tool.name;
  tools_.emplace(key, std::move(tool));
}

json Server::Error(int code, const std::string& message) {
  return json{{"code", code}, {"message", message}};
}

json Server::HandleInitialize(const json& params) {
  initialized_ = true;
  const std::string asked = params.value("protocolVersion", std::string{});
  if (!asked.empty() && asked != kProtocolVersion) {
    std::fprintf(stderr, "[mcp] client asked for %s, offering %s\n",
                 asked.c_str(), kProtocolVersion);
  }
  return json{
      {"protocolVersion", kProtocolVersion},
      {"capabilities", {{"tools", {{"listChanged", false}}}}},
      {"serverInfo", {{"name", name_}, {"version", version_}}},
  };
}

json Server::HandleToolsList() const {
  json list = json::array();
  for (const auto& [name, tool] : tools_) {
    list.push_back({{"name", tool.name},
                    {"description", tool.description},
                    {"inputSchema", tool.input_schema}});
  }
  return json{{"tools", std::move(list)}};
}

json Server::HandleToolsCall(const json& params) {
  const std::string name = params.value("name", std::string{});
  const auto it = tools_.find(name);
  if (it == tools_.end()) throw std::invalid_argument("unknown tool: " + name);

  const json arguments = params.value("arguments", json::object());
  try {
    json payload = it->second.handler(arguments);
    return json{
        {"content", json::array({{{"type", "text"}, {"text", payload.dump(2)}}})},
        {"structuredContent", std::move(payload)},
        {"isError", false},
    };
  } catch (const std::exception& e) {
    // A failing tool is a result, not a transport error: the model should see
    // the message and be able to react.
    return json{
        {"content", json::array({{{"type", "text"}, {"text", e.what()}}})},
        {"isError", true},
    };
  }
}

json Server::Dispatch(const json& request, bool* is_notification) {
  const std::string method = request.value("method", std::string{});
  const json params = request.value("params", json::object());
  *is_notification = !request.contains("id") || request["id"].is_null();

  json response{{"jsonrpc", "2.0"}};
  if (!*is_notification) response["id"] = request["id"];

  try {
    if (method == "initialize") {
      response["result"] = HandleInitialize(params);
    } else if (method == "notifications/initialized") {
      return response;  // notification: nothing to send back
    } else if (method == "ping") {
      response["result"] = json::object();
    } else if (method == "tools/list") {
      response["result"] = HandleToolsList();
    } else if (method == "tools/call") {
      response["result"] = HandleToolsCall(params);
    } else {
      response["error"] = Error(kMethodNotFound, "unknown method: " + method);
    }
  } catch (const std::invalid_argument& e) {
    response["error"] = Error(kInvalidParams, e.what());
  } catch (const std::exception& e) {
    response["error"] = Error(kInvalidRequest, e.what());
  }
  return response;
}

void Server::Run() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    if (line.back() == '\r') line.pop_back();  // tolerate CRLF clients

    const json request = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (request.is_discarded() || !request.is_object()) {
      WriteLine({{"jsonrpc", "2.0"},
                 {"id", nullptr},
                 {"error", Error(kParseError, "malformed JSON-RPC message")}});
      continue;
    }

    bool is_notification = false;
    json response = Dispatch(request, &is_notification);
    if (!is_notification) WriteLine(response);
  }
}

}  // namespace gtabot::mcp
