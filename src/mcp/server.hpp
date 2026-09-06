#pragma once
//
// Minimal MCP server over the stdio transport.
//
// The transport is newline-delimited JSON-RPC 2.0 on stdin/stdout. Nothing
// other than protocol traffic may ever be written to stdout - diagnostics go to
// stderr, which the client is expected to capture.
//
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gtabot::mcp {

using json = nlohmann::json;

struct Tool {
  std::string name;
  std::string description;
  json        input_schema;
  // Returns the tool result content. Throwing std::runtime_error turns into an
  // `isError` result rather than a transport-level failure, which is what MCP
  // clients expect for a tool that ran and failed.
  std::function<json(const json& arguments)> handler;
};

class Server {
 public:
  Server(std::string name, std::string version);

  void AddTool(Tool tool);

  // Reads stdin until EOF. Returns when the client closes the transport.
  void Run();

 private:
  json Dispatch(const json& request, bool* is_notification);
  json HandleInitialize(const json& params);
  json HandleToolsList() const;
  json HandleToolsCall(const json& params);

  static json Error(int code, const std::string& message);

  std::string name_;
  std::string version_;
  std::map<std::string, Tool> tools_;
  bool initialized_ = false;
};

}  // namespace gtabot::mcp
