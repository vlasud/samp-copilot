"""Minimal MCP client over the loopback HTTP endpoint bot.asi serves.

Shared by console.py and selftest.py, and short enough to copy into an agent
loop as-is.
"""
import json
import urllib.error
import urllib.request

DEFAULT_URL = "http://127.0.0.1:8765/mcp"


class NotRunning(Exception):
    """The endpoint refused the connection - the game is not up with the mod."""


class Client:
    def __init__(self, url=DEFAULT_URL, timeout=70):
        self.url = url
        self.timeout = timeout
        self._id = 0

    def call(self, method, params=None):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        return self._post(message)

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._post(message)

    def _post(self, message):
        request = urllib.request.Request(
            self.url, data=json.dumps(message).encode("utf-8"),
            headers={"Content-Type": "application/json",
                     "Accept": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read().decode("utf-8")
        except urllib.error.URLError as e:
            raise NotRunning(
                "no MCP endpoint at %s - start the game with bot.asi loaded "
                "(%s)" % (self.url, e.reason)) from None
        return json.loads(body) if body.strip() else {}

    def handshake(self, client_name="tools"):
        response = self.call("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": client_name, "version": "0"}})
        self.notify("notifications/initialized")
        return response["result"]

    def tools(self):
        return self.call("tools/list")["result"]["tools"]

    def tool(self, name, arguments=None):
        """Returns the tool's structured result, or {"error": ...} on failure."""
        response = self.call("tools/call",
                             {"name": name, "arguments": arguments or {}})
        if "error" in response:
            return {"transport_error": response["error"]}
        result = response["result"]
        if result.get("isError"):
            return {"error": result["content"][0]["text"]}
        return result.get("structuredContent", result)
