#pragma once
//
// MCP server: JSON-RPC 2.0 dispatch and a tool registry.
//
// Transport-agnostic on purpose - it takes a request object and returns a
// response object. The HTTP layer in http.hpp feeds it; nothing here knows
// about sockets.
//
#include <functional>
#include <map>
#include <string>

#include "types.hpp"

namespace gtabot::mcp {

struct Tool {
  std::string name;
  std::string description;
  json        input_schema;
  // Returns the tool result content. Throwing std::runtime_error turns into an
  // `isError` result rather than a transport-level failure, which is what MCP
  // clients expect from a tool that ran and failed.
  std::function<json(const json& arguments)> handler;
};

class Server {
 public:
  Server(std::string name, std::string version);

  void AddTool(Tool tool);

  // Handles one JSON-RPC request. Returns an empty json for notifications,
  // which carry no response.
  json Handle(const json& request);

  std::size_t tool_count() const { return tools_.size(); }
  std::uint64_t requests_served() const { return requests_; }

 private:
  json HandleInitialize(const json& params);
  json HandleToolsList() const;
  json HandleToolsCall(const json& params);

  static json Error(int code, const std::string& message);

  std::string name_;
  std::string version_;
  std::map<std::string, Tool> tools_;
  std::uint64_t requests_ = 0;
};

}  // namespace gtabot::mcp
