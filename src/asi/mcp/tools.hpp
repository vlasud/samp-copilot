#pragma once
//
// The tools an agent can call. Kept apart from the JSON-RPC plumbing so the
// list of capabilities reads as one page.
//
namespace gtabot::mcp {

class Server;

void RegisterTools(Server* server);

}  // namespace gtabot::mcp
