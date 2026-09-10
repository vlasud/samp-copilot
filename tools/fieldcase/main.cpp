// Run a saved field again, without the game.
//
//     fieldcase D:\SAMP\bot.fields                 every case, one line each
//     fieldcase D:\SAMP\bot.fields\shutin-0003.field    that one, in full
//
// Every real fix in this navigation work came from one number, and every
// number cost twenty-five minutes of closing the game, copying the mod in,
// waiting for a login and a spawn and a walk. The field writes down what it
// saw whenever a plan ends badly (see nav/casefile.hpp); this puts that back
// to the clearance and the search with whatever rules are in the build now,
// and says what came out. A directory of them runs in seconds.
//
// What it answers, which the log never could:
//   - was there a way at all, from where he stood, on what he could see?
//   - how much of what he could see could he reach?
//   - what stopped the search: walls, steps, corners, and the tallest step
//     it turned down.
// A change to a rule is a rebuild and a rerun, and the table says at once
// whether it helped or hurt, over every place that ever went wrong.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "nav/casefile.hpp"
#include "nav/grid.hpp"

using gtabot::nav::Chamfer;
using gtabot::nav::FieldCase;
using gtabot::nav::Grid;
using gtabot::nav::LoadCase;
using gtabot::nav::MarkLedges;
using gtabot::nav::Pull;
using gtabot::nav::Searcher;
using gtabot::nav::SearchRules;
using gtabot::nav::SmoothBetweenReadings;
using gtabot::nav::Vec3;

namespace {

// The same numbers the field itself uses. They live in nav/field.cpp, which
// cannot be linked here because it reads the world; if one of them moves
// there and not here, the replay is measuring the wrong thing, so they are
// named and gathered rather than sprinkled about.
constexpr float kSqueeze = 1.0f;      // the disc opened round his feet
constexpr float kMaxLeg = 60.0f;      // the longest straightened leg
constexpr int   kExpandPerStep = 200000;

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

struct Outcome {
  bool  reached = false;
  float length_m = 0;
  float short_by_m = 0;
  int   legs = 0;
  int   expanded = 0;
  int   walkable = 0;     // cells with floor and nothing solid
  int   reachable = 0;    // of those, ones he can actually get to
  int   refused_shut = 0, refused_step = 0, refused_corner = 0;
  float tallest_step = 0;
  int   ledges = 0;
  double ms = 0;
  std::vector<Vec3> route;
};

// Exactly what nav/field.cpp does from the clearance on, in the same order.
Outcome Replay(FieldCase* one) {
  Outcome out;
  Grid& g = one->grid;
  const LARGE_INTEGER zero = {};
  LARGE_INTEGER frequency = zero, began = zero, ended = zero;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&began);

  SmoothBetweenReadings(&g, one->stride);
  out.ledges = MarkLedges(&g, SearchRules{}.max_step);
  Chamfer(&g);

  int sx = 0, sy = 0, gx = 0, gy = 0;
  if (!g.cell_of(one->from, &sx, &sy)) return out;
  const int start = g.index(sx, sy);
  const int reach = static_cast<int>(std::ceil(kSqueeze / g.cell));
  for (int iy = sy - reach; iy <= sy + reach; ++iy)
    for (int ix = sx - reach; ix <= sx + reach; ++ix) {
      if (!g.inside(ix, iy)) continue;
      const int at = g.index(ix, iy);
      if (Away(g.centre(at), one->from) > kSqueeze) continue;
      g.blocked[at] = 0;
      if (g.known[at] != 1) { g.known[at] = 1; g.ground[at] = one->ref_z; }
      if (g.clear[at] < 3) g.clear[at] = 3;
    }
  Vec3 aim = one->to;
  aim.x = std::min(std::max(aim.x, g.x0 + g.cell), g.x0 + (g.W - 1) * g.cell);
  aim.y = std::min(std::max(aim.y, g.y0 + g.cell), g.y0 + (g.H - 1) * g.cell);
  g.cell_of(aim, &gx, &gy);
  const int goal = g.index(gx, gy);

  Searcher searcher;
  searcher.Start(&g, start, goal, one->to, SearchRules{});
  while (!searcher.Step(kExpandPerStep)) {
  }
  QueryPerformanceCounter(&ended);
  out.ms = frequency.QuadPart == 0
               ? 0
               : 1000.0 * static_cast<double>(ended.QuadPart - began.QuadPart) /
                     static_cast<double>(frequency.QuadPart);

  const int end = searcher.end();
  out.reached = searcher.reached_goal();
  out.expanded = searcher.expanded();
  out.refused_shut = searcher.refused_shut();
  out.refused_step = searcher.refused_step();
  out.refused_corner = searcher.refused_corner();
  out.tallest_step = searcher.tallest_step();
  out.short_by_m = Away(g.centre(end), one->to);
  for (int at = 0; at < g.W * g.H; ++at) {
    if (g.passable(at)) ++out.walkable;
    if (searcher.cost(at) > 0) ++out.reachable;
  }

  const std::vector<int> cells = searcher.CellsTo(end);
  const float pull_step =
      std::min(SearchRules{}.max_step, SearchRules{}.max_grade * g.cell * 1.42f);
  const std::vector<int> pulled = Pull(g, cells, kMaxLeg, pull_step);
  out.legs = static_cast<int>(pulled.size()) - 1;
  for (std::size_t i = 0; i + 1 < pulled.size(); ++i)
    out.length_m += Away(g.centre(pulled[i]), g.centre(pulled[i + 1]));
  for (int at : pulled) out.route.push_back(g.centre(at));
  return out;
}

void Picture(const Grid& g, const FieldCase& one, const Outcome& out,
             int columns) {
  const int step = std::max(1, (g.W + columns - 1) / columns);
  std::vector<std::uint8_t> on_route(static_cast<std::size_t>(g.W) * g.H, 0);
  for (std::size_t i = 0; i + 1 < out.route.size(); ++i) {
    const Vec3& a = out.route[i];
    const Vec3& b = out.route[i + 1];
    const float span = std::max(0.1f, Away(a, b));
    for (float m = 0; m <= span; m += g.cell) {
      int ix = 0, iy = 0;
      const Vec3 p{a.x + (b.x - a.x) * m / span, a.y + (b.y - a.y) * m / span, 0};
      if (g.cell_of(p, &ix, &iy)) on_route[g.index(ix, iy)] = 1;
    }
  }
  int fx = 0, fy = 0, tx = 0, ty = 0;
  const bool have_from = g.cell_of(one.from, &fx, &fy);
  const bool have_to = g.cell_of(one.to, &tx, &ty);
  std::printf("\n");
  for (int iy = g.H - 1; iy >= 0; iy -= step) {
    std::string row;
    for (int ix = 0; ix < g.W; ix += step) {
      const int at = g.index(ix, iy);
      if (have_from && std::abs(ix - fx) < step && std::abs(iy - fy) < step)
        row += '@';
      else if (have_to && std::abs(ix - tx) < step && std::abs(iy - ty) < step)
        row += '*';
      else if (on_route[at])       row += 'o';
      else if (g.blocked[at])      row += '#';
      else if (g.known[at] != 1)   row += ' ';
      else                         row += '.';
    }
    std::printf("  %s\n", row.c_str());
  }
  std::printf("\n  @ where he stood   * where he was sent   o the way out\n"
              "  # solid   . floor   (blank) nothing read\n\n");
}

std::vector<std::string> CasesIn(const std::string& path) {
  std::vector<std::string> found;
  const DWORD what = GetFileAttributesA(path.c_str());
  if (what == INVALID_FILE_ATTRIBUTES) return found;
  if ((what & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    found.push_back(path);
    return found;
  }
  WIN32_FIND_DATAA entry;
  const std::string glob = path + "\\*.field";
  HANDLE search = FindFirstFileA(glob.c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) return found;
  do {
    found.push_back(path + "\\" + entry.cFileName);
  } while (FindNextFileA(search, &entry));
  FindClose(search);
  std::sort(found.begin(), found.end());
  return found;
}

const char* Leaf(const std::string& path) {
  const std::size_t cut = path.find_last_of("\\/");
  return path.c_str() + (cut == std::string::npos ? 0 : cut + 1);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "fieldcase <a .field file, or the directory of them>\n\n"
        "  Runs a field the mod saved through the clearance and the search\n"
        "  again, with the rules in this build, and says what came out.\n"
        "  The mod writes them to bot.fields beside the game whenever a plan\n"
        "  ends badly.\n");
    return 2;
  }
  const std::vector<std::string> cases = CasesIn(argv[1]);
  if (cases.empty()) {
    std::printf("nothing to run in %s\n", argv[1]);
    return 1;
  }
  const bool one_only = cases.size() == 1;

  if (!one_only)
    std::printf("%-34s %7s %6s %6s %7s %6s %6s %5s  %s\n", "case", "wanted",
                "got", "short", "reached", "of all", "expand", "ms", "then");
  int reached = 0;
  double total_ms = 0;
  for (const std::string& path : cases) {
    FieldCase one;
    if (!LoadCase(path, &one)) {
      std::printf("%-34s  could not be read\n", Leaf(path));
      continue;
    }
    const float wanted = Away(one.from, one.to);
    const Outcome out = Replay(&one);
    total_ms += out.ms;
    if (out.reached) ++reached;
    if (one_only) {
      std::printf("%s\n  %s\n", Leaf(path), one.note.c_str());
      std::printf("  from (%.1f, %.1f, %.1f) to (%.1f, %.1f) - %.0f m wanted\n",
                  one.from.x, one.from.y, one.from.z, one.to.x, one.to.y, wanted);
      std::printf("  grid %d x %d at %.2f m, floor read every %.1f m\n", one.grid.W,
                  one.grid.H, one.grid.cell, one.stride * one.grid.cell);
      std::printf("  NOW: %s, %.0f m in %d legs, %.0f m short of the mark\n",
                  out.reached ? "reached it" : "did not reach it", out.length_m,
                  out.legs, out.short_by_m);
      std::printf("  could walk on %d cells, could reach %d of them (%.0f%%)\n",
                  out.walkable, out.reachable,
                  out.walkable == 0
                      ? 0.0
                      : 100.0 * out.reachable / out.walkable);
      std::printf("  the search turned down %d moves for a wall, %d for a step "
                  "(tallest %.2f m), %d for a corner; %d ledge cells shut\n",
                  out.refused_shut, out.refused_step, out.tallest_step,
                  out.refused_corner, out.ledges);
      std::printf("  %d cells looked at in %.0f ms\n", out.expanded, out.ms);
      if (!one.route.empty())
        std::printf("  THEN, in the game: %d legs, and the mod called it \"%s\"\n",
                    static_cast<int>(one.route.size()) - 1, one.why.c_str());
      Picture(one.grid, one, out, 150);
    } else {
      std::printf("%-34s %7.0f %6.0f %6.0f %7s %5.0f%% %6d %5.0f  %s\n",
                  Leaf(path), wanted, out.length_m, out.short_by_m,
                  out.reached ? "yes" : "no",
                  out.walkable == 0 ? 0.0
                                    : 100.0 * out.reachable / out.walkable,
                  out.expanded, out.ms, one.why.c_str());
    }
  }
  if (!one_only)
    std::printf("\n%d of %d reached, %.0f ms in all\n", reached,
                static_cast<int>(cases.size()), total_ms);
  return 0;
}
