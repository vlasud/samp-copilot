#include "state/probe.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "hooks/frame.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::asi {
namespace {

json DescribeModule(const wchar_t* name) {
  const mem::Module module = mem::FindModule(name);
  if (!module.valid()) return nullptr;
  return json{{"base", module.base}, {"size", module.size}};
}

// samp.exe starts the game with the connection details on the command line.
// Whatever it says is ground truth we did not have to find in memory - which
// makes it the perfect needle to prove a memory read against.
json ParseCommandLine(const std::string& line) {
  json out = json::object();
  const char* keys[][2] = {{"-n", "nick"}, {"-h", "host"}, {"-p", "port"},
                           {"-z", "password"}};
  for (const auto& key : keys) {
    const std::size_t at = line.find(std::string(key[0]) + " ");
    if (at == std::string::npos) continue;
    std::size_t start = at + 3;
    while (start < line.size() && line[start] == ' ') ++start;
    std::size_t end = line.find(' ', start);
    if (end == std::string::npos) end = line.size();
    if (end > start) out[key[1]] = line.substr(start, end - start);
  }
  return out;
}

std::string HexDump(std::uintptr_t address, std::size_t bytes) {
  std::string out;
  std::string ascii;
  for (std::size_t i = 0; i < bytes; ++i) {
    unsigned char byte = 0;
    if (!mem::Read<unsigned char>(address + i, &byte)) break;
    char buffer[4];
    std::snprintf(buffer, sizeof(buffer), "%02X ", byte);
    out += buffer;
    ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
  }
  return out + " |" + ascii + "|";
}

json HitsToJson(const std::vector<mem::Hit>& hits, bool with_context) {
  json out = json::array();
  for (std::size_t i = 0; i < hits.size(); ++i) {
    json entry{{"address", hits[i].address}};
    if (hits[i].rva) entry["rva"] = hits[i].rva;
    // A hexdump around the first few hits turns "we found it" into something
    // that can be eyeballed against what is on screen.
    if (with_context && i < 4)
      entry["context"] = HexDump(hits[i].address > 16 ? hits[i].address - 16 : 0,
                                 48);
    out.push_back(std::move(entry));
  }
  return out;
}

}  // namespace

json BuildWorldSnapshot() {
  const samp::Client client = samp::Detect();
  return json{
      {"samp",
       {{"loaded", client.base != 0},
        {"version", samp::ToString(client.version)},
        {"base", client.base}}},
      // Placeholders until the pools are read. Named now so the shape the
      // agent consumes does not change under it later.
      {"self", nullptr},
      {"players", json::array()},
      {"vehicles", json::array()},
  };
}

json BuildStatusSnapshot() {
  const FrameHook::Integrity integrity = FrameHook::CheckIntegrity();
  const std::uint64_t idle = FrameHook::idle_ms();

  // The whole point of this field: say plainly which of the two failure modes
  // is happening, instead of leaving a frozen counter to be interpreted.
  const char* verdict = "ok";
  if (!FrameHook::installed()) {
    verdict = "hook not installed";
  } else if (!integrity.present_intact && !integrity.endscene_intact) {
    verdict = "our patch bytes are gone - something else rewrote the entry point";
  } else if (FrameHook::frames() == 0) {
    verdict = "hooked, but the game has not presented a single frame yet";
  } else if (idle > 2000) {
    verdict = "patch intact but no frames - the game is not rendering "
              "(alt-tabbed out of exclusive fullscreen does this)";
  }

  return json{
      {"frame",
       {{"hook_installed", FrameHook::installed()},
        {"driver", FrameHook::driver()},
        {"frames", FrameHook::frames()},
        {"fps", FrameHook::fps()},
        {"idle_ms", idle}}},
      {"hook",
       {{"present_hooked", integrity.present_hooked},
        {"present_intact", integrity.present_intact},
        {"present_byte", integrity.present_byte},
        {"endscene_hooked", integrity.endscene_hooked},
        {"endscene_intact", integrity.endscene_intact},
        {"endscene_byte", integrity.endscene_byte}}},
      {"verdict", verdict},
      {"bridge",
       {{"pending_tasks", Bridge::pending_tasks()},
        {"dropped_tasks", Bridge::dropped_tasks()}}},
  };
}

json ProbeMemory(const json& args) {
  const std::string scope = args.value("scope", std::string{"samp"});
  const std::size_t max_hits = args.value("max_hits", std::size_t{16});
  std::string needle = args.value("needle", std::string{});

  const std::string command_line = GetCommandLineA();
  const json parsed = ParseCommandLine(command_line);

  // With no needle given, look for the nickname the launcher passed in. If it
  // turns up inside samp.dll's memory, the read path is proven end to end.
  bool needle_from_command_line = false;
  if (needle.empty() && parsed.contains("nick")) {
    needle = parsed["nick"].get<std::string>();
    needle_from_command_line = true;
  }

  const mem::Module samp_module = mem::FindModule(L"samp.dll");
  const bool whole_process = scope == "process";
  const std::vector<mem::Region> regions =
      whole_process ? mem::ReadableRegions()
                    : mem::ReadableRegions(samp_module.valid() ? &samp_module
                                                               : nullptr);

  std::size_t scanned_bytes = 0;
  for (const mem::Region& region : regions) scanned_bytes += region.size;

  json out{
      {"modules",
       {{"gta_sa.exe", DescribeModule(nullptr)},
        {"samp.dll", DescribeModule(L"samp.dll")},
        {"d3d9.dll", DescribeModule(L"d3d9.dll")}}},
      {"command_line", command_line},
      {"command_line_parsed", parsed},
      {"frame",
       {{"hook_installed", FrameHook::installed()},
        {"driver", FrameHook::driver()},
        {"frames", FrameHook::frames()},
        {"fps", FrameHook::fps()}}},
      {"scan",
       {{"scope", whole_process ? "process" : "samp.dll"},
        {"regions", regions.size()},
        {"bytes", scanned_bytes}}},
  };

  const samp::Client client = samp::Detect();
  out["samp"] = {{"loaded", client.base != 0},
                 {"version", samp::ToString(client.version)},
                 {"base", client.base},
                 {"size_of_image", client.size_of_image},
                 {"timestamp", client.timestamp}};

  if (!needle.empty()) {
    const std::vector<mem::Hit> hits =
        mem::Scan(regions, needle, max_hits, /*budget_bytes=*/1u << 30,
                  samp_module.base);
    out["search"] = {{"needle", needle},
                     {"from_command_line", needle_from_command_line},
                     {"found", hits.size()},
                     {"hits", HitsToJson(hits, /*with_context=*/true)}};
  } else {
    out["search"] = {{"needle", nullptr},
                     {"note",
                      "no needle given and no -n on the command line; pass one "
                      "explicitly, e.g. text you can see in the game"}};
  }

  // Only meaningful inside samp.dll, where a root pointer would live.
  if (!whole_process && samp_module.valid()) {
    const std::vector<mem::Hit> candidates =
        mem::ScanPointerCandidates(regions, /*max_hits=*/64, samp_module.base);
    out["pointer_candidates"] = {{"found", candidates.size()},
                                 {"sample", HitsToJson(candidates, false)}};
  }

  return out;
}

}  // namespace gtabot::asi
