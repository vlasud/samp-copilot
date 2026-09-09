#include "actions/walker.hpp"

#include "actions/contact.hpp"
#include "game/peds.hpp"
#include "nav/local.hpp"

#include <string>
#include "nav/ring.hpp"
#include "nav/trail.hpp"

#include "samp/input_state.hpp"
#include "samp/objects.hpp"
#include "samp/keys.hpp"
#include "game/mouse_watch.hpp"
#include "ui/overlay.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "game/bindings.hpp"
#include "game/exe.hpp"
#include "hooks/windowmode.hpp"
#include "log.hpp"
#include "nav/planner.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"

namespace gtabot::act {
namespace {

// CPad::UpdatePads fills the pad from the real keyboard and mouse:
// AffectPadFromKeyBoard puts each held key into CPad::PCTempKeyState, and
// CPad::Update reconciles that into NewState and clears it. The walker's
// stick goes into the same temp state at the end of the frame, from the
// frame hook, and the next UpdatePads picks it up exactly as it would a
// held key. No instruction of the game's is patched for it.
constexpr std::uint32_t kPads       = 0xB73458;
constexpr std::uint32_t kKeyState   = 0x78;    // CPad::PCTempKeyState
// The game's own key table, the one its window procedure fills on
// WM_KEYDOWN: F1..F12 first, then the 256 standard keys, 255 while down.
// The walk is expressed as the player's movement keys held in it; the
// game's AffectPadFromKeyBoard turns those into the stick the way it does
// for a hand on the keyboard, and nothing about the pad is ours.
constexpr std::uint32_t kTempKeyTable = 0xB72CB0;   // CPad::TempKeyState
constexpr std::uint32_t kStandardKeys = 0x18;
constexpr short kKeyDown = 255;
constexpr unsigned kFwd = 1, kBack = 2, kRight = 4, kLeft = 8;
constexpr unsigned kSprintKey = 16, kJumpKey = 32;
// CPad is 0x134 bytes; NewState is the first member. Its sticks are the first
// two shorts of it, and the buttons follow in PlayStation order: square is
// jump, cross is sprint.
constexpr std::uint32_t kNewStateLeftStickX  = 0x00;
constexpr std::uint32_t kNewStateLeftStickY  = 0x02;
constexpr std::uint32_t kNewStateButtonSquare = 0x1C;
constexpr std::uint32_t kNewStateButtonCross  = 0x20;
constexpr short kPressed = 255;

// CPhysical::m_vecMoveSpeed, whose z says whether he is going up or down.
// The ped's own standing/in-the-air flags are not read: by the time the pad
// is written, at the end of the frame, the physics has already cleared the
// one and left the other set, and a walker that trusted them never jumped.
constexpr std::uint32_t kMoveSpeed = 0x44;

// Full deflection. The game clamps to 128 either way.
constexpr short kFullStick = 127;

// How close counts as arrived. A person does not stop on a coin, and the
// route's own legs are metres long. Running, he needs a little more.
constexpr float kArriveNext = 1.8f;
// How near a point has to be before walking past it counts as having
// walked it: a stride or two, no more.
constexpr float kPassedWithin = 3.0f;
constexpr float kArriveLast = 1.4f;
// The floor: closer than this and he is standing on the point, which is a
// thing no amount of stick can reliably hold.
constexpr float kArriveFloor = 0.4f;
// Near enough to what he was sent to that touching it is the point.
constexpr float kTouchingDistance = 2.5f;
// Backing out of a corner: how far, for how long, and how many times before
// the corner is admitted to be a wall.
constexpr float kBackOutMetres = 3.0f;
constexpr unsigned long long kBackOutMs = 1500;
constexpr int   kMaxBackOuts = 3;

// Stuck: he has hardly moved at all for this long. Not "no closer to the
// target" - a man going round a fence is no closer to the target either, and
// he is doing exactly the right thing.
constexpr unsigned long long kStuckMs = 1300;
constexpr float kStuckMetres = 0.5f;
// And separately: moving, but no closer to the point he is heading for, for
// a long time. That is a loop or a wall being followed to nowhere, and the
// journey should have another look.
constexpr unsigned long long kNoCloserMs = 10000;
constexpr float kCloser = 1.0f;
// The whole walk, so a route that cannot be finished does not press the stick
// forever.
constexpr unsigned long long kWalkLimitMs = 180000;

// The stick's frame, from the game's own arithmetic rather than measured.
//
// CTaskSimplePlayerOnFoot::PlayerControlZelda turns the pad into a heading
// as GetRadianAngleBetweenPoints(0, 0, -stickX, stickY) - m_fOrientation,
// where that angle function is atan2(x, -y), the game's headings run
// anticlockwise from north, and the direction walked is (-sin h, cos h).
// Solving that for the stick, with the wanted direction as a mathematical
// angle theta (anticlockwise from east):
//
//   a      = theta - pi/2 + m_fOrientation
//   stickX = -127 sin a
//   stickY = -127 cos a
//
// So the frame offset is pi/2 - m_fOrientation and the sideways axis is
// mirrored - which is what the measuring used to discover for itself, two
// and a half seconds of running the wrong way into every session.
constexpr float kHalfPi = 1.57079633f;
constexpr float kMirrored = -1.0f;
// Should the derivation be wrong on some camera mode, the character faces
// off his line steadily while his line is not changing. Left for this long,
// that is a wrong frame and not a slow turn, and it is corrected by what is
// observed - the old way.
constexpr float kFrameErrorRadians = 0.52f;
constexpr float kIntentSteadyRadians = 0.30f;
constexpr unsigned long long kFrameErrorMs = 1500;
// Without a readable camera, the old way from the start: push forward
// briefly and watch where he ends up pointing.
constexpr unsigned long long kBootstrapMs = 450;

// Meeting something in the way that the whiskers did not see - a low thing
// under the knee line, a fence with holes in it. He steps round it and
// carries on, the other side next time. After a few of those it is a wall.
constexpr int   kMaxSidesteps    = 3;
constexpr float kSidestepMetres  = 2.8f;
constexpr unsigned long long kSidestepMs = 1400;

// The stick's direction is eased rather than snapped, so a turn is a turn
// and not a jolt; its magnitude is not. A keyboard only has full deflection,
// and a character who slows to a walk when he leans round a bin or nears a
// corner does not move like anyone at a keyboard. He runs, always, and stops
// when he is there.
constexpr float kStickEase = 0.28f;

// The whiskers: seven lines of sight fanned about the way he wants to go,
// cast every tenth of a second at knee, waist and chest height, each ending
// on the ground the game finds there. Centre first, then thirty-five degrees
// either side, seventy, and a hundred - the last pair pointing a little
// behind him, because a man with his nose against a fence has nothing clear
// in front of him at all and still has a way along it.
//
// What blocks the low lines but not the one at head height is low enough to
// jump: a boom gate, a rail, a low wall. That is not steered round; it is
// run at and jumped.
//
// Beside each is how far off his line he leans when that whisker is the
// nearest clear one on its side: a little more than the whisker's own angle,
// so the thing is cleared rather than grazed.
constexpr int   kWhiskers = 7;
constexpr float kWhiskerAngle[kWhiskers]  = {0.0f, 0.61f, -0.61f, 1.22f, -1.22f, 1.75f, -1.75f};
constexpr float kWhiskerLength[kWhiskers] = {4.0f, 3.0f, 3.0f, 2.4f, 2.4f, 2.4f, 2.4f};
constexpr float kLeanFor[kWhiskers]       = {0.0f, 0.79f, -0.79f, 1.40f, -1.40f, 1.92f, -1.92f};
// Half the width of a person, near enough. The game's own ped collision is
// about this, and it is what decides whether he fits between two things.
constexpr float kBodyRadius = 0.34f;
constexpr float kKnee  = 0.5f;
constexpr float kWaist = 0.95f;
constexpr float kChest = 1.35f;
constexpr float kHead  = 1.9f;
// The lines a whisker is made of, every fifteen centimetres from the shin
// to the chest. Three lines - knee, waist, chest - left a forty-centimetre
// gap between each, and the arm of a boom gate, five centimetres thick and
// a metre and a bit off the ground, sat in the gap between the waist and
// the chest: every whisker clear, and him running into it. Nothing thicker
// than a hand hides between these. The first four - up to eighty
// centimetres - are the low lines: blocked there and clear above is a
// thing to hop; blocked at the waist or above is a wall, whatever the
// head clears, because a thing that tall is climbed, not jumped, and
// climbing is what left him hanging on fences.
constexpr float kLines[] = {0.35f, 0.5f, 0.65f, 0.8f, 0.95f, 1.1f, 1.25f, 1.4f};
constexpr int   kLineCount = 8;
constexpr int   kLowLines  = 4;
// The shoulder lines, a body's width to either side of the centre one, are
// fewer: they are there for the chair leg beside the way, not the bar
// across it, and every line is a query.
constexpr float kSideLines[] = {0.5f, 0.95f, 1.25f};
constexpr int   kSideLineCount = 3;
// Steps he takes without thinking, and the ledges and drops he takes with a
// jump - the drop only when the way is meant to go down there.
constexpr float kMaxClimb     = 1.0f;
constexpr float kMaxJumpClimb = 2.0f;
constexpr float kMaxDrop      = 1.6f;
constexpr float kMaxJumpDrop  = 4.5f;
constexpr float kDescending   = 1.2f;
constexpr unsigned long long kProbeMs = 100;
// How near a low thing has to be before he jumps it.
constexpr float kJumpAt = 2.2f;
// How far past a raised edge to ask whether it is a floor, and how much the
// ground may fall away there and still be one. Half a metre of step down is
// a terrace with a kerb; a metre and a half is the far side of a fence.
constexpr float kBeyondLedge     = 1.5f;
constexpr float kLedgeKeepsGoing = 0.8f;

// Going round something. Once a side is chosen he keeps it - along a fence
// the way to the target stays blocked for as long as the fence is, and a
// character that changes his mind every probe walks a metre each way and
// gets nowhere. The side is given up only in a dead end: nothing clear on
// that side for most of a second. Two dead ends, or long enough following
// without the line ever clearing, and it is a wall for the journey to plan
// round.
constexpr int kDeadEndProbes = 8;
constexpr int kMaxSideChanges = 2;
constexpr unsigned long long kFollowGiveUpMs = 9000;
constexpr int kCentreClearProbes = 2;

// Running and jumping.
//
// Sprint is held on the ground when the way is clear and there is far
// enough to go. Sprint and jump are never down in the same frame, and the
// jump has priority: the moment a jump is decided, sprint is let go, and
// the jump is pressed two frames later with sprint still up - a server's
// anti-bunny-hop looks for the two keys together, and a sync packet sees
// one frame's keys. The character is still at sprinting speed those two
// frames later, so the jump is the long one. Sprint stays up for the whole
// of the flight and a moment after landing; then it is held again, and the
// next jump follows.
constexpr float kSprintMinRemaining = 10.0f;
constexpr float kSprintMaxError     = 0.70f;   // radians off his line
constexpr float kHopMinToNext       = 7.0f;
constexpr unsigned long long kHopIntervalMs   = 950;
constexpr int   kFramesSprintUpBeforeJump = 2;
constexpr unsigned long long kJumpTakesMs     = 300;   // airborne whatever the ground says
constexpr unsigned long long kSettleAfterLandMs = 150;
// On the ground: feet within this of it, and not moving vertically.
constexpr float kFeetOnGround = 0.35f;
constexpr float kStillVertical = 0.045f;

std::atomic<bool> g_installed{false};

std::mutex        g_mutex;
std::vector<Vec3> g_route;
std::size_t       g_leg = 0;
float             g_arrive_last = kArriveLast;
int               g_backouts = 0;
bool              g_last_leg_is_the_destination = false;
bool              g_strict = false;
// The ring: how often it is asked, how far it looks, and what it last said.
constexpr unsigned long long kRingEveryMs = 120;
constexpr float   kRingReach = 2.4f;
unsigned long long g_ring_ms = 0;
bool              g_ring_turned = false;
float             g_ring_steer = 0;
float             g_ring_free = 0;
// How many points ahead the smoothing may look. The route is half-metre
// squares, so eight of them is four metres - about as far as a person sees a
// clear line across a room.
constexpr int     kLookAhead = 8;
// How long a strict walk is given to get going before standing still counts
// as being blocked.
constexpr unsigned long long kStrictGraceMs = 3000;
bool              g_walking = false;
std::string       g_note = "idle";
unsigned long long g_started_ms = 0;
float             g_to_next = 0;
float             g_remaining = 0;

// Progress: the window that says whether he has moved at all, and the longer
// one that says whether he is getting anywhere.
Vec3  g_window_pos;
unsigned long long g_window_ms = 0;
float g_best_distance = 0;
unsigned long long g_closer_ms = 0;

// The frame the stick is expressed in. With the camera readable it is
// computed exactly and the correction stays at zero; measured only as a
// check, and applied only when the check fails for a while. Without the
// camera the old measured estimate takes over, hand and all.
float g_correction = 0;
bool  g_camera_frame = false;
unsigned long long g_frame_error_since = 0;
float g_steered_at_error = 0;
float g_offset = 0;
bool  g_offset_seen = false;
float g_last_emit = 0;
float g_last_steered = 0;
float g_hand = kMirrored;
unsigned long long g_bootstrap_until = 0;
unsigned long long g_wrong_since = 0;
bool  g_corrected = false;
float g_error_deg = 0;
bool  g_said_frame = false;

// Stepping round something, and the eased stick.
int   g_sidesteps = 0;
bool  g_sidestep_left = true;
unsigned long long g_sidestep_until = 0;
Vec3  g_sidestep_target;
float g_stick_x = 0, g_stick_y = 0;

// The whiskers, and the going-round they drive.
unsigned long long g_probe_ms = 0;
bool  g_whisker_clear[kWhiskers] = {true, true, true, true, true, true, true};
bool  g_whisker_low[kWhiskers]   = {false, false, false, false, false, false, false};

// Following closely: the route is a line to stay on, the way he goes is
// chosen on the local picture, and a blocked route goes back to the journey
// at once. See SetPrecise.
bool  g_precise = false;
std::string g_held_by;
int   g_wedged_hops = 0;
unsigned long long g_wedged_hops_ms = 0;
// Set while a hop meant to free him is in the air: the stick is let go for
// it. Pressed into what he is wedged against, the hop got him fifteen
// centimetres twenty-three times over; with nothing held it took him three
// metres and seven out of the pocket between two hospital beds on the
// first try.
bool  g_hop_loose = false;
nav::LocalPicture g_local;
Vec3  g_route_start;              // the first leg runs from here
float g_precise_delta = 0;        // off the line, as the picture last chose
float g_route_free = 0;           // metres of the route ahead found open
float g_route_low_at = -1;        // where the first low thing on it is, or -1
unsigned long long g_route_blocked_since = 0;
// The picture's reach, how far along the route is checked, and how far
// ahead on the line he heads for. Six metres is a second and a half of
// running; the plan owns everything past that.
constexpr float kLocalRadius = 6.0f;
constexpr float kRouteLook   = 6.0f;
constexpr float kPursuitAhead = 3.0f;
// Open enough to run at; blocked near enough to stop over.
constexpr float kRouteOpen = 2.5f;
constexpr float kRouteBlockedNear = 2.0f;
// Where he stands is not judged: the paint inflates every wall by half a
// cell and he is often against one.
constexpr float kStartSlack = 0.6f;
// The headings tried beside the line when it is shut: every ten degrees to
// seventy either side, three and a half metres out, the room beside each
// worth a little and every degree of turning costing a little.
constexpr float kSteerLook = 3.5f;
constexpr float kSteerStep = 0.1745f;   // ten degrees, out to a hundred and twenty
constexpr int   kSteerSteps = 12;
constexpr float kRoomWanted = 0.35f;
constexpr float kRoomWorth = 0.8f;
constexpr float kTurnCost = 1.2f;
// Somebody standing on the route is given this long to move.
constexpr unsigned long long kWaitForPersonMs = 3000;
// Blocked ahead and getting no nearer for this long: the plan is wrong,
// not merely a hand's breadth out.
constexpr unsigned long long kBlockedNoProgressMs = 2500;
// Hops allowed to a walk that is wedged, how far apart, and how long the
// count is remembered. The count does not belong to the walk: the journey
// draws a fresh route every second or two when a walk keeps handing itself
// back, and a per-walk count let him hop twenty-three times on the spot
// against a character the server had frozen.
constexpr int kJumpsWhenWedged = 2;
constexpr unsigned long long kJumpAgainMs = 1200;
constexpr unsigned long long kForgetWedgedMs = 30000;
// With the camera readable the frame cannot be wrong by a little; only a
// gross error - a mirrored or backward frame - is corrected, and only while
// the route ahead is open, because sliding along a wall is not a wrong
// frame. Thirty degrees of tolerance had the correction flapping forty
// degrees each way every time he brushed a fence.
constexpr float kGrossFrameError = 1.75f;
// Following closely, standing still is answered sooner.
constexpr unsigned long long kNoCloserPreciseMs = 6000;
Vec3  g_whisker_end[kWhiskers];
float g_lean = 0;             // radians added to the wanted heading
int   g_follow_side = 0;      // +1 left, -1 right, 0 straight
int   g_last_follow_side = 1;
unsigned long long g_follow_since = 0;
int   g_dead_end_probes = 0;
int   g_side_changes = 0;
int   g_centre_clear = 0;
bool  g_wall = false;

// The ground, as of the last probe, and running.
bool  g_on_ground = true;
// Whether anything was found under him at all, and how far below it was.
// "No ground here" is a question the reading could not answer, and treating
// it as "he is in the air" had him let go of keys he was never holding.
bool  g_ground_known = false;
float g_above_ground = 0;
bool  g_sprint_on = true;
bool  g_hop_on    = true;
bool  g_sprinting = false;
bool  g_jump_this_frame = false;
// Frames until a decided jump is pressed; negative when none is pending.
// Sprint is up throughout.
int   g_jump_countdown = -1;
unsigned long long g_last_jump_ms = 0;
unsigned long long g_hop_gap_ms = kHopIntervalMs;
unsigned long long g_landed_ms = 0;
bool  g_was_airborne = false;
// Hanging off something. A jump at a wall ends with his hands on the ledge,
// and if it is too high to pull up he stays there for as long as forward is
// held. Letting go of everything drops him.
// Leaning on a door. A great many of the things a server builds a room out
// of swing open when somebody walks into them, and to the whiskers those
// are a wall like any other - so before going round, he pushes.
unsigned long long g_pushing_until = 0;
int  g_pushes = 0;
constexpr unsigned long long kPushForMs = 1800;
constexpr int kPushesPerPlace = 2;
constexpr float kDoorReach = 3.0f;
// A door is worth walking to even when it is not straight ahead.
constexpr float kDoorSearch = 5.0f;
Vec3 g_push_at{};
bool g_pushing_at_something = false;
// The door he is already dealing with, so the same one is not spliced into
// the route over and over.
Vec3 g_door_seen{};
bool g_door_known = false;
constexpr float kThroughDoor = 3.0f;
// Doorways the route is meant to go through, and how near one has to be
// before the whiskers stop being listened to. Three metres is a stride and
// a half: near enough that what is ahead is the door frame, far enough that
// he is already pointed at it when the leaning stops.
std::vector<Vec3> g_doorways;
constexpr float kDoorwayNear = 3.0f;
// And how near a door has to be for a stuck strict walk to push it rather
// than back away. Wider than the whisker rule: he stalls a couple of metres
// short of a door, against its frame or the railing beside it, and backing
// out from there and walking up again was twelve seconds a cycle.
constexpr float kDoorPushReach = 5.0f;
constexpr int   kDoorPushes = 4;
// And how far off the line to it he may be aiming for it still to be the
// thing he is walking into: a door beside him is not a door ahead of him.
constexpr float kDoorwayArc = 1.2f;

unsigned long long g_hanging_since = 0;
unsigned long long g_letting_go_until = 0;
int   g_lets_go = 0;
constexpr unsigned long long kHangingMs = 2200;
constexpr unsigned long long kLetGoForMs = 900;
int   g_jumps = 0;

float Normalise(float radians) {
  while (radians > 3.14159265f)  radians -= 6.28318531f;
  while (radians < -3.14159265f) radians += 6.28318531f;
  return radians;
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

std::uintptr_t Pad() {
  const std::uintptr_t pad = game::At(kPads);
  if (pad == 0 || !asi::mem::IsReadable(pad, 0x110)) return 0;
  return pad;
}

void WriteShort(std::uintptr_t pad, std::uint32_t offset, short value) {
  *reinterpret_cast<short*>(pad + offset) = value;
}

std::atomic<unsigned long> g_pad_thread{0};
// The keys held in the table last frame, and the ones the player bound.
unsigned g_held = 0;
int  g_key_fwd = 'W', g_key_back = 'S', g_key_left = 'A', g_key_right = 'D';
int  g_key_sprint = VK_SPACE, g_key_jump = VK_LSHIFT;
bool g_keys_read = false;
bool g_said_background = false;
// Eight directions are what keys give. The one held changes only when the
// wanted heading has moved well into the next sector - a hand does not
// tap D twenty times a second to hold a heading of thirty degrees, it
// holds W until the corner and then W and D.
int g_direction = -1;
constexpr float kSectorDegrees = 45.0f;
constexpr float kSwitchBeyond  = 30.0f;   // from the held sector's centre
// A jump key is down for a real press, not a frame.
constexpr unsigned long long kJumpHoldMs = 90;
unsigned long long g_jump_release_ms = 0;
std::atomic<unsigned long long> g_key_events{0};
std::atomic<bool> g_test_keys{false};

void ReadBindings() {
  if (g_keys_read) return;
  g_keys_read = true;
  g_key_fwd    = game::KeyForAction(game::kGoForward, 'W');
  g_key_back   = game::KeyForAction(game::kGoBack, 'S');
  g_key_left   = game::KeyForAction(game::kGoLeft, 'A');
  g_key_right  = game::KeyForAction(game::kGoRight, 'D');
  g_key_sprint = game::KeyForAction(game::kSprint, VK_SPACE);
  g_key_jump   = game::KeyForAction(game::kJumping, VK_LSHIFT);
  LOG_INFO("walker: keys {} {} {} {} (forward, back, left, right), sprint {}, jump {}",
           game::KeyName(g_key_fwd), game::KeyName(g_key_back),
           game::KeyName(g_key_left), game::KeyName(g_key_right),
           game::KeyName(g_key_sprint), game::KeyName(g_key_jump));
}

// Whether keys may go out at all: the game window has to be the one in
// front - the keys go to whatever is - and the panel's own menu must not
// be up, since it eats key messages.
bool KeysMayGo() {
  HWND window = game::GameWindow();
  if (window == nullptr) return false;
  // Behind another window the keys are posted to this one rather than sent
  // through the system, so they still reach the game and only the game.
  if (GetForegroundWindow() != window && !asi::WindowMode::RunsInBackground())
    return false;
  if (asi::Overlay::MenuOpen()) return false;
  return true;
}

// Which keys stand for this stick, this frame.
unsigned DirectionKeys(short x, short y) {
  if (x == 0 && y == 0) {
    g_direction = -1;
    return 0;
  }
  float degrees = std::atan2(static_cast<float>(-y), static_cast<float>(x)) *
                  57.2957795f;
  if (degrees < 0) degrees += 360.0f;
  int k = g_direction;
  if (k < 0) {
    k = static_cast<int>((degrees + kSectorDegrees / 2) / kSectorDegrees) & 7;
  } else {
    float off = degrees - static_cast<float>(k) * kSectorDegrees;
    while (off > 180.0f)  off -= 360.0f;
    while (off < -180.0f) off += 360.0f;
    if (off > kSwitchBeyond)       k = (k + 1) & 7;
    else if (off < -kSwitchBeyond) k = (k + 7) & 7;
  }
  g_direction = k;
  unsigned keys = 0;
  if (k == 1 || k == 2 || k == 3) keys |= kFwd;
  if (k == 5 || k == 6 || k == 7) keys |= kBack;
  if (k == 0 || k == 1 || k == 7) keys |= kRight;
  if (k == 3 || k == 4 || k == 5) keys |= kLeft;
  return keys;
}

// Holds exactly these keys: the ones newly wanted go down, the ones no
// longer wanted come up. Keys the walk never pressed are not touched, so
// the player's own hand on W is left alone. Only the changes are sent -
// a held key is held by the system until its up.
void HoldKeys(unsigned want) {
  const int keys[6] = {g_key_fwd, g_key_back, g_key_left, g_key_right,
                       g_key_sprint, g_key_jump};
  const unsigned bits[6] = {kFwd, kBack, kLeft, kRight, kSprintKey, kJumpKey};
  std::vector<int> down;
  for (int i = 0; i < 6; ++i)
    if (want & bits[i]) down.push_back(keys[i]);
  samp::KeysHold(down);
  g_held = want;
}
// What was last written, for the line logged when the keyboard is taken.
short g_last_x = 0, g_last_y = 0;
bool  g_last_sprint = false;

void ClearStick() {
  if (GetCurrentThreadId() != g_pad_thread.load(std::memory_order_relaxed)) return;
  const std::uintptr_t pad = Pad();
  if (pad == 0) return;
  const std::uintptr_t keys = pad + kKeyState;
  WriteShort(keys, kNewStateLeftStickX, 0);
  WriteShort(keys, kNewStateLeftStickY, 0);
  WriteShort(keys, kNewStateButtonCross, 0);
  WriteShort(keys, kNewStateButtonSquare, 0);
  if (g_held != 0) HoldKeys(0);
}

float VerticalSpeed(std::uintptr_t ped) {
  float vz = 0;
  if (ped != 0) asi::mem::Read<float>(ped + kMoveSpeed + 8, &vz);
  return vz;
}

// Is the next thing on the way a door he is meant to walk into?
bool DoorwayAhead(const Vec3& here, float wanted) {
  for (const Vec3& door : g_doorways) {
    const float dx = door.x - here.x, dy = door.y - here.y;
    const float span = std::sqrt(dx * dx + dy * dy);
    if (span > kDoorwayNear) continue;
    if (span < 0.3f) return true;
    if (std::fabs(Normalise(std::atan2(dy, dx) - wanted)) <= kDoorwayArc)
      return true;
  }
  return false;
}

// The nearest of the route's doors within reach, if any.
bool NearestDoorway(const Vec3& here, Vec3* door, float within = kDoorwayNear) {
  float best = within;
  bool found = false;
  for (const Vec3& one : g_doorways) {
    const float dx = one.x - here.x, dy = one.y - here.y;
    const float span = std::sqrt(dx * dx + dy * dy);
    if (span <= best) {
      best = span;
      *door = one;
      found = true;
    }
  }
  return found;
}

void StopLocked(const char* why) {
  g_walking = false;
  g_route.clear();
  g_leg = 0;
  g_note = why;
  g_sidestep_until = 0;
  g_stick_x = 0;
  g_stick_y = 0;
  g_lean = 0;
  g_follow_side = 0;
  g_wall = false;
  g_sprinting = false;
  g_jump_this_frame = false;
  g_jump_countdown = -1;
  for (int i = 0; i < kWhiskers; ++i) {
    g_whisker_clear[i] = true;
    g_whisker_low[i] = false;
  }
  ClearStick();
}

// A jump is wanted: sprint comes up now, the press follows in a couple of
// frames. Nothing to do when one is already on its way.
void WantJump(unsigned long long now, const char* why) {
  if (g_jump_countdown >= 0) return;
  g_jump_countdown = kFramesSprintUpBeforeJump;
  g_last_jump_ms = now;   // so nothing decides another one meanwhile
  if (why != nullptr) LOG_INFO("walk: {}", why);
}

// The whisker at `index` is on the left of the line when its angle is
// positive; index 0 is the line itself.
int SideOf(int index) {
  return index == 0 ? 0 : kWhiskerAngle[index] > 0 ? 1 : -1;
}

// The nearest clear whisker on a side, by index, or -1 when none is. A low
// thing counts as clear: it is jumped, not gone round.
int NearestClear(int side) {
  for (int rank = 1; rank <= 3; ++rank) {
    const int index = side > 0 ? rank * 2 - 1 : rank * 2;
    if (g_whisker_clear[index]) return index;
  }
  return -1;
}

// Lines at the given heights between two points, on the ground's terms at
// each end. Cars count.
bool LinesClear(const Vec3& from, float from_feet, const Vec3& to, float to_feet,
                const float* heights, int count) {
  for (int i = 0; i < count; ++i)
    if (!game::LineClear(Vec3{from.x, from.y, from_feet + heights[i]},
                         Vec3{to.x, to.y, to_feet + heights[i]}, true))
      return false;
  return true;
}

// The same, at the width of his shoulders.
//
// A line down the middle of the way ahead says nothing about the chair leg
// half a metre to the side of it. He walks at it, leans, steps round, leans
// back, walks at it again - twenty seconds of that in the middle of a
// hospital ward, in front of everybody, and none of it is what a person
// looks like. So the question stops being "is this line clear" and becomes
// "does his body fit through there": the centre and both shoulders, each
// swept the length of the whisker.
bool WideClear(const Vec3& from, float from_feet, const Vec3& to, float to_feet,
               const float* heights, int count,
               const float* side_heights = nullptr, int side_count = 0) {
  const float dx = to.x - from.x, dy = to.y - from.y;
  const float span = std::sqrt(dx * dx + dy * dy);
  if (span < 0.01f) return LinesClear(from, from_feet, to, to_feet, heights, count);
  if (!LinesClear(from, from_feet, to, to_feet, heights, count)) return false;
  const float sx = -dy / span * kBodyRadius;
  const float sy =  dx / span * kBodyRadius;
  const float* at = side_heights != nullptr ? side_heights : heights;
  const int n = side_heights != nullptr ? side_count : count;
  const float sides[2] = {1.0f, -1.0f};
  for (const float side : sides) {
    const Vec3 a{from.x + sx * side, from.y + sy * side, 0};
    const Vec3 b{to.x + sx * side, to.y + sy * side, 0};
    if (!LinesClear(a, from_feet, b, to_feet, at, n)) return false;
  }
  return true;
}

// How far along a whisker the thing actually is, to within a quarter of
// its length: the line is halved twice. Two calls more per whisker, and the
// point goes into the obstacle rather than onto the ground in front of it.
float DistanceAlongWhisker(const Vec3& here, float angle, float length, float height) {
  if (!game::LineOfSightAvailable() || game::CallSlotsLeft() < 10) return length * 0.6f;
  const float feet = here.z - 1.0f;
  float low = 0, high = length;
  for (int step = 0; step < 2; ++step) {
    const float mid = (low + high) * 0.5f;
    const Vec3 end{here.x + std::cos(angle) * mid, here.y + std::sin(angle) * mid, 0};
    const bool clear = game::LineClear(Vec3{here.x, here.y, feet + height},
                                       Vec3{end.x, end.y, feet + height}, true);
    (clear ? low : high) = mid;
  }
  return high;
}

// Everything the whiskers found in the way, given to the planner as points
// to route round - each put a little past where its whisker met the thing,
// so it sits inside the fence and not on the pavement before it. Only what
// was actually seen, and only walls: a low thing is jumped, not remembered.
void RememberWhatIsAhead(const Vec3& here, float wanted, const char* what) {
  int remembered = 0;
  for (int i = 0; i < 5 && remembered < 3; ++i) {
    if (g_whisker_clear[i]) continue;
    const float angle = wanted + kWhiskerAngle[i];
    const float along =
        DistanceAlongWhisker(here, angle, kWhiskerLength[i], kChest) + 0.4f;
    nav::RememberObstacle(Vec3{here.x + std::cos(angle) * along,
                               here.y + std::sin(angle) * along, here.z},
                          what);
    ++remembered;
  }
}

// A hillside is not a cliff. When the ground at a whisker's end is well
// below his feet, the ground halfway along says which: on a slope it is
// about halfway down, over an edge it is still up here or already at the
// bottom. Slopes he walks and slides down; edges he does not step off.
bool ContinuousSlope(const Vec3& here, const Vec3& end, float feet, float ground_end) {
  const float drop = feet - ground_end;
  if (drop > 9.0f) return false;   // steeper than a hillside, whatever it is
  const Vec3 mid{(here.x + end.x) * 0.5f, (here.y + end.y) * 0.5f, here.z};
  float ground_mid = 0;
  if (!game::GroundBelow(Vec3{mid.x, mid.y, here.z + 1.5f}, &ground_mid)) return false;
  const float part = feet - ground_mid;
  return part > drop * 0.3f && part < drop * 0.7f;
}

// Casts the whiskers about `wanted` from where he stands. Each is clear when
// there is ground at its end within a step of his own - or a ledge he can
// jump up, or a drop he can jump down when the way is meant to go down -
// and nothing between him and it at knee, waist or chest height. When the
// low lines are blocked but the one at head height is not, the whisker is
// "low": the way is open to someone who jumps.
// The far look, cast in the same breath as the whiskers; it lives further
// down, beside the steering that uses it.
void LookAhead(const Vec3& here, float wanted);

void ProbeWhiskers(const Vec3& here, float wanted, bool descending) {
  // Past the game-call ceiling every whisker would read blocked. Keep what
  // they saw last time rather than invent a wall.
  // Three lines a whisker now, so three times the room to leave.
  if (game::CallSlotsLeft() < 240) return;
  std::vector<Vec3> ends;
  std::vector<bool> clear;
  ends.reserve(kWhiskers);
  clear.reserve(kWhiskers);
  // Whether a raised edge is a floor or the top of a rail. Asked a stride
  // beyond it: on a terrace the ground is still up there, on a fence it has
  // gone back down to the street.
  const auto StandsOnTop = [](const Vec3& from, float angle, float reach,
                              float top) {
    const Vec3 over{from.x + std::cos(angle) * (reach + kBeyondLedge),
                    from.y + std::sin(angle) * (reach + kBeyondLedge), from.z};
    float behind = 0;
    if (!game::GroundBelow(Vec3{over.x, over.y, top + 2.0f}, &behind))
      return false;
    return top - behind <= kLedgeKeepsGoing;
  };
  const bool can_look = game::LineOfSightAvailable();
  const float feet = here.z - 1.0f;
  const float max_drop = descending ? kMaxJumpDrop : kMaxDrop;
  for (int i = 0; i < kWhiskers; ++i) {
    const float angle = wanted + kWhiskerAngle[i];
    const Vec3 end{here.x + std::cos(angle) * kWhiskerLength[i],
                   here.y + std::sin(angle) * kWhiskerLength[i], here.z};
    bool ok = true;
    bool low = false;
    float ground = feet;
    float water = 0;
    if (!game::GroundBelow(Vec3{end.x, end.y, here.z + 1.5f}, &ground)) {
      ok = false;   // a cliff, or the edge of the loaded world
    } else if (game::WaterLevel(Vec3{end.x, end.y, ground}, &water) &&
               water > ground + 0.5f) {
      ok = false;   // water over what the ground call took for ground
    } else if (ground - feet > kMaxJumpClimb ||
               (feet - ground > max_drop && !ContinuousSlope(here, end, feet, ground))) {
      ok = false;   // a wall he cannot get onto, or a drop he should not take
    } else if (ground - feet > kMaxClimb) {
      // A ledge - but a ledge is only worth climbing if there is somewhere to
      // stand on it. The top of a fence reads exactly like the edge of a
      // terrace: ground, a metre and a half up, straight ahead. He would jump,
      // land astride the rail, and stay there, which is what kept happening.
      // So the ground is asked for again a stride further on, at the height
      // he would be standing at: a terrace goes on, a fence has nothing
      // behind it but the drop back to where he started.
      low = StandsOnTop(here, angle, kWhiskerLength[i], ground);
      ok = low;   // if it is a rail, it is a wall to go round
    } else if (can_look &&
               !WideClear(here, feet, end, ground, kLines, kLineCount,
                          kSideLines, kSideLineCount)) {
      // Low enough to jump means low enough to jump: a kerb, a bench, a
      // fence to the knee. It used to mean anything his chest cleared, and
      // a fence to the waist is not hopped: he runs at it, catches the top
      // and hangs there with his arms up until somebody notices. So the
      // waist decides. Blocked below it and clear from it up is a thing to
      // jump; blocked at the waist or above is a wall, whatever the air
      // above it is doing.
      if (WideClear(here, feet, end, ground, kLines + kLowLines, kLineCount - kLowLines,
                    kSideLines + 1, kSideLineCount - 1))
        low = true;   // something to jump over
      else
        ok = false;   // a wall
    }
    // Something low is something to go round, not something to run at. A
    // hospital bed, a bench, a counter: the head passes over it and the
    // knees do not, and a person walks around such a thing rather than
    // charging it. The jump is still there for when going round has failed
    // and he is standing against it - the stuck handler asks for it - but it
    // is no longer the first answer, which is what had him running into the
    // furniture in plain sight.
    g_whisker_clear[i] = ok && !low;
    g_whisker_low[i]   = ok && low;
    g_whisker_end[i] = Vec3{end.x, end.y, ground + 1.0f};
    ends.push_back(g_whisker_end[i]);
    clear.push_back(ok);
  }
  nav::SetDebugWhiskers(here, std::move(ends), std::move(clear));
  LookAhead(here, wanted);
}

// The long look: how far he can see, and which way is most open.
//
// The whiskers are short on purpose - they decide about the ground, and the
// ground four metres ahead is knowable while the ground fifteen metres ahead
// is a guess about a hill nobody has walked yet. But a man does not discover
// a wall by touching it, and this one did: he would run into it, run along
// it one way, then the other, then carry on. So there is a second look, far
// and shallow, that asks one question only - how far is the way open, at
// chest height, along each of these directions - and steers early on the
// answer.
constexpr int   kLookSpokes = 7;
constexpr float kLookAngle[kLookSpokes] = {0.0f, 0.35f, -0.35f, 0.70f,
                                           -0.70f, 1.05f, -1.05f};
constexpr float kLookReach = 14.0f;
// A metre of open ground is worth this much of a radian off his line, so a
// wide detour has to buy a good deal of room to be taken.
constexpr float kRoomPerRadian = 9.0f;
float g_look_free[kLookSpokes] = {kLookReach, kLookReach, kLookReach,
                                  kLookReach, kLookReach, kLookReach,
                                  kLookReach};

void LookAhead(const Vec3& here, float wanted) {
  for (int i = 0; i < kLookSpokes; ++i)
    g_look_free[i] = DistanceAlongWhisker(here, wanted + kLookAngle[i],
                                          kLookReach, kChest);
}

// Which way to lean while nothing is yet in reach: the most open direction
// that is not too far off his line, and only as much as the way ahead is
// closing in. With the road clear it returns nothing at all.
float EarlyLean() {
  const float ahead = g_look_free[0];
  if (ahead >= kLookReach * 0.95f) return 0.0f;
  int best = 0;
  float best_score = g_look_free[0];
  for (int i = 1; i < kLookSpokes; ++i) {
    const float score =
        g_look_free[i] - std::fabs(kLookAngle[i]) * kRoomPerRadian;
    if (score > best_score + 0.25f) {
      best_score = score;
      best = i;
    }
  }
  if (best == 0) return 0.0f;
  // All of the turn when the wall is on top of him, none of it when it is as
  // far as he can see.
  const float urgency = 1.0f - ahead / kLookReach;
  return kLookAngle[best] * urgency;
}

// Turns what the whiskers saw into a lean off the line - and, when the line
// stays blocked, into going round: a side is chosen and kept.
void DecideLean(unsigned long long now) {
  if (g_whisker_clear[0]) {
    // The line is open. Two probes of that in a row and the detour is over;
    // one might be a gap between two posts.
    if (++g_centre_clear >= kCentreClearProbes) {
      if (g_follow_side != 0) g_last_follow_side = g_follow_side;
      g_follow_side = 0;
      // Nothing within reach, so the far look has the floor: it starts the
      // turn while the wall is still a dozen metres off, which is when a
      // person starts it, instead of after walking into the thing.
      g_lean = g_strict ? 0.0f : EarlyLean();
    } else {
      g_lean *= 0.5f;
    }
    g_wall = false;
    return;
  }
  g_centre_clear = 0;

  if (g_follow_side == 0) {
    // Choose a side: the one with the nearest clear whisker; between equals,
    // the one with more of them clear; between those, the side taken last
    // time, which along one fence is the same side.
    const int left = NearestClear(1), right = NearestClear(-1);
    int side = 0;
    if (left >= 0 && right >= 0) {
      const int left_rank = (left + 1) / 2, right_rank = (right + 1) / 2;
      if (left_rank != right_rank) {
        side = left_rank < right_rank ? 1 : -1;
      } else {
        int left_count = 0, right_count = 0;
        for (int i = 1; i < kWhiskers; ++i)
          if (g_whisker_clear[i]) (SideOf(i) > 0 ? left_count : right_count)++;
        side = left_count != right_count ? (left_count > right_count ? 1 : -1)
                                         : g_last_follow_side;
      }
    } else if (left >= 0) {
      side = 1;
    } else if (right >= 0) {
      side = -1;
    }
    if (side == 0) {
      g_wall = true;   // nothing clear anywhere
      return;
    }
    g_follow_side = side;
    g_follow_since = now;
    g_dead_end_probes = 0;
    g_side_changes = 0;
    LOG_INFO("walk: something across the way, going round it on the {}",
             side > 0 ? "left" : "right");
  }

  if (now - g_follow_since > kFollowGiveUpMs) {
    // Long enough. Whatever this is, it wants planning round, not feeling.
    g_wall = true;
    return;
  }

  int nearest = NearestClear(g_follow_side);
  if (nearest < 0) {
    if (++g_dead_end_probes < kDeadEndProbes) return;   // hold the last lean
    if (++g_side_changes > kMaxSideChanges) {
      g_wall = true;
      return;
    }
    g_follow_side = -g_follow_side;
    g_dead_end_probes = 0;
    LOG_INFO("walk: dead end that way, going round on the {} instead",
             g_follow_side > 0 ? "left" : "right");
    nearest = NearestClear(g_follow_side);
    if (nearest < 0) return;
  }
  g_dead_end_probes = 0;
  g_lean = kLeanFor[nearest];
  g_wall = false;
}

// The nearest point of the current leg to him. The first leg runs from
// where the walk began.
Vec3 Projection(const Vec3& here) {
  const Vec3 from = g_leg == 0 ? g_route_start : g_route[g_leg - 1];
  const Vec3& to = g_route[g_leg];
  const float dx = to.x - from.x, dy = to.y - from.y;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 1e-6f ? ((here.x - from.x) * dx + (here.y - from.y) * dy) / len2 : 1.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  return Vec3{from.x + dx * t, from.y + dy * t, to.z};
}

// The way ahead as points: from him to the nearest point of the leg, then
// leg end by leg end.
std::vector<Vec3> WayAhead(const Vec3& here) {
  std::vector<Vec3> way;
  way.push_back(here);
  way.push_back(Projection(here));
  for (std::size_t i = g_leg; i < g_route.size(); ++i) way.push_back(g_route[i]);
  return way;
}

// The point `along` metres down the way ahead; its end when the way is
// shorter than that.
Vec3 RoutePoint(const Vec3& here, float along) {
  const std::vector<Vec3> way = WayAhead(here);
  float left = along;
  for (std::size_t i = 1; i < way.size(); ++i) {
    const float len = Distance2D(way[i - 1], way[i]);
    if (len >= left) {
      const float k = len > 1e-6f ? left / len : 0.0f;
      return Vec3{way[i - 1].x + (way[i].x - way[i - 1].x) * k,
                  way[i - 1].y + (way[i].y - way[i - 1].y) * k, way[i].z};
    }
    left -= len;
  }
  return way.back();
}

// The point on the line `ahead` metres past the nearest point of it to
// him: heading for that pulls him back onto the line rather than at the far
// end of the leg, which is how a shove off the line stayed a shove.
Vec3 PursuitPoint(const Vec3& here, float ahead) {
  const std::vector<Vec3> way = WayAhead(here);
  float left = ahead;
  for (std::size_t i = 2; i < way.size(); ++i) {
    const float len = Distance2D(way[i - 1], way[i]);
    if (len >= left) {
      const float k = len > 1e-6f ? left / len : 0.0f;
      return Vec3{way[i - 1].x + (way[i].x - way[i - 1].x) * k,
                  way[i - 1].y + (way[i].y - way[i - 1].y) * k, way[i].z};
    }
    left -= len;
  }
  return way.back();
}

// How far along the way ahead the picture finds open, up to `metres`, and
// where the first low thing on it is.
float RouteAhead(const Vec3& here, float metres, float* low_at) {
  *low_at = -1.0f;
  const std::vector<Vec3> way = WayAhead(here);
  float gone = 0;
  for (std::size_t i = 1; i < way.size() && gone < metres; ++i) {
    float low = -1.0f;
    const float len = Distance2D(way[i - 1], way[i]);
    // The slack is measured from him, not from the start of each leg: the
    // first leg is often half a metre long, and skipping only its own
    // samples left the cell he is standing beside judged all the same.
    const float free = g_local.FreeAlong(way[i - 1], way[i],
                                         std::max(0.0f, kStartSlack - gone), &low);
    if (*low_at < 0 && low >= 0) *low_at = gone + low;
    if (free < len - 1e-3f) return gone + free;
    gone += len;
  }
  return metres;
}

// The picture round him as text, for the log: `reach` metres each way, a
// character a cell, y upward. S is him, X the point asked about, # shut,
// ~ low, . open, ? no ground.
std::string PictureAbout(const Vec3& here, const Vec3& mark, float reach) {
  std::string out;
  if (!g_local.ok) return "(no picture)";
  const nav::Grid& g = g_local.high;
  const int cells = static_cast<int>(reach / g.cell);
  int hx = 0, hy = 0, mx = -1, my = -1;
  g.cell_of(here, &hx, &hy);
  g.cell_of(mark, &mx, &my);
  for (int iy = hy + cells; iy >= hy - cells; --iy) {
    for (int ix = hx - cells; ix <= hx + cells; ++ix) {
      char c = ' ';
      if (!g.inside(ix, iy)) c = ' ';
      else if (ix == hx && iy == hy) c = 'S';
      else if (ix == mx && iy == my) c = 'X';
      else {
        const int at = g.index(ix, iy);
        c = g.known[at] != 1 ? '?' : g.blocked[at] ? '#' : g_local.low[at] ? '~' : '.';
      }
      out += c;
    }
    out += '\n';
  }
  return out;
}

// How far off the line to head, chosen on the picture: nought when the line
// itself is open with room beside it, else the heading with the most open
// ground ahead for the least turning.
float PreciseDelta(const Vec3& here, float pursuit) {
  const auto along = [&](float a, float d) {
    return Vec3{here.x + std::cos(a) * d, here.y + std::sin(a) * d, here.z};
  };
  float low = -1.0f;
  const float straight = g_local.FreeAlong(here, along(pursuit, kSteerLook), kStartSlack, &low);
  if (straight >= kRouteOpen && g_local.Clearance(along(pursuit, 1.2f)) >= kRoomWanted)
    return 0.0f;
  float best = 0, best_score = -1e9f;
  for (int k = -kSteerSteps; k <= kSteerSteps; ++k) {
    const float a = pursuit + k * kSteerStep;
    const float free = g_local.FreeAlong(here, along(a, kSteerLook), kStartSlack, &low);
    const float room = g_local.Clearance(along(a, std::min(free, 1.2f)));
    const float score = std::min(free, kSteerLook) + std::min(room, 1.0f) * kRoomWorth -
                        std::fabs(k * kSteerStep) * kTurnCost;
    if (score > best_score) {
      best_score = score;
      best = k * kSteerStep;
    }
  }
  return best;
}

// Everything the walk decides, run from inside the pad hook. Returns the
// stick to press, or false to press nothing.
bool DecideStick(short* out_x, short* out_y) {
  g_jump_this_frame = false;
  g_sprinting = false;
  if (!g_walking) return false;

  const unsigned long long now = GetTickCount64();
  if (now - g_started_ms > kWalkLimitMs) {
    StopLocked("given up - the walk ran out of time");
    LOG_WARN("walk: {}", g_note);
    return false;
  }

  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) {
    StopLocked("stopped - the character cannot be read");
    return false;
  }
  const Vec3 here{self.x, self.y, self.z};
  // Where he is standing is passable, and no map can argue with it. This is
  // the whole of what the learned graph is made of.
  nav::TrailVisit(here, g_on_ground || !g_ground_known);

  // Arrived at this leg - or past it.
  //
  // Insisting on reaching each point exactly is what makes a character walk
  // like a machine: he heads for a corner, touches it, then turns on the spot
  // for the next one. A person rounds the corner. So a waypoint also counts
  // as done once the one after it is nearer than it is, which is what having
  // passed something means, and the turn happens while still moving.
  while (g_leg < g_route.size()) {
    const bool last = g_leg + 1 == g_route.size();
    const float d = Distance2D(here, g_route[g_leg]);
    bool done = d <= (last ? g_arrive_last : kArriveNext);
    // Having passed a point means being near it and past it, not merely
    // standing somewhere the next point happens to be nearer from. On a
    // route of one-metre squares - which is what the trail he has walked
    // before is made of - the loose reading swallowed nine legs in a frame
    // without a step being taken, the walk reported that it had arrived,
    // the journey drew the same route again, and that went round and round
    // with the character standing perfectly still.
    if (!done && !last && d <= kPassedWithin &&
        Distance2D(here, g_route[g_leg + 1]) < d)
      done = true;
    if (!done) break;
    ++g_leg;
    g_best_distance = 0;
    g_closer_ms = now;
  }
  if (g_leg >= g_route.size()) {
    StopLocked("arrived");
    LOG_INFO("walk: arrived");
    return false;
  }

  // Cutting the corners of a verified path.
  //
  // A route of half-metre squares walked square by square is a staircase, and
  // a person does not walk one. So the furthest point along it that he can
  // reach in a straight line his shoulders fit through becomes the one he
  // heads for. Nothing leaves the space the map already proved: the straight
  // line is tested the same way the squares were.
  if (g_strict && game::CallSlotsLeft() > 400) {
    const float low_lines[2] = {kKnee, kChest};
    const float feet_now = here.z - 1.0f;
    std::size_t furthest = g_leg;
    const std::size_t limit =
        std::min(g_route.size(), g_leg + static_cast<std::size_t>(kLookAhead));
    for (std::size_t i = g_leg + 1; i < limit; ++i) {
      if (!WideClear(here, feet_now, g_route[i], g_route[i].z - 1.0f,
                     low_lines, 2))
        break;
      furthest = i;
    }
    if (furthest != g_leg) {
      g_leg = furthest;
      g_best_distance = 0;
      g_closer_ms = now;
    }
  }

  // A leg the map drew inside something solid - a chair, a counter, a
  // parked crate - cannot be reached, and standing against it waiting to
  // reach it is how a walk ends up handing itself back for ever. Where the
  // picture says the point is inside something and there is another point
  // after it, that one becomes the leg. The last one is never skipped: it
  // is where he was sent, and standing against what he was sent to is
  // arriving.
  if (g_precise && g_local.ok) {
    while (g_leg + 1 < g_route.size() && g_local.Shut(g_route[g_leg]) &&
           Distance2D(here, g_route[g_leg]) < kLocalRadius - 0.5f) {
      LOG_INFO("walk: leg {} at ({:.1f},{:.1f}) is inside something - taking the "
               "next one instead", static_cast<int>(g_leg) + 1,
               g_route[g_leg].x, g_route[g_leg].y);
      ++g_leg;
      g_best_distance = 0;
      g_closer_ms = now;
    }
  }

  const Vec3& target = g_route[g_leg];
  const float distance = Distance2D(here, target);
  g_to_next = distance;
  g_remaining = distance;
  for (std::size_t i = g_leg + 1; i < g_route.size(); ++i)
    g_remaining += Distance2D(g_route[i - 1], g_route[i]);

  const float ahead = std::atan2(target.y - here.y, target.x - here.x);
  // Whether this leg is meant to go down: only then is an edge a way.
  const bool descending = target.z < here.z - kDescending;

  // On the ground, or in the air? The ground under his feet, as of the last
  // probe, and whether he is moving vertically now.
  const float vz = VerticalSpeed(self.game_ped);
  const bool airborne = !g_on_ground || std::fabs(vz) > kStillVertical ||
                        now - g_last_jump_ms < kJumpTakesMs;
  if (airborne) {
    g_was_airborne = true;
  } else if (g_was_airborne) {
    g_was_airborne = false;
    g_landed_ms = now;
  }
  const bool settled = !airborne && now - g_landed_ms >= kSettleAfterLandMs;

  // Off the ground and going nowhere: he is holding on to a ledge. Nothing
  // pressed for a moment and he drops, which is the only way down.
  if (now < g_letting_go_until) return false;
  // Hanging is judged by where he is, not by whether the game calls him
  // airborne. With his hands on a ledge and his feet off the ground he can
  // read as standing, and the release that was meant to save him never
  // fired: he stayed on the wall with his arms up while the walk reported
  // walking.
  if (g_ground_known && g_above_ground > 1.2f &&
      std::fabs(vz) < kStillVertical) {
    if (g_hanging_since == 0) g_hanging_since = now;
    if (now - g_hanging_since > kHangingMs) {
      g_hanging_since = 0;
      g_letting_go_until = now + kLetGoForMs;
      ++g_lets_go;
      g_landed_ms = now + kLetGoForMs;
      LOG_INFO("walk: hanging off something - letting go (time {})", g_lets_go);
      return false;
    }
  } else {
    g_hanging_since = 0;
  }

  // Getting anywhere at all?
  if (g_best_distance == 0 || distance < g_best_distance - kCloser) {
    g_best_distance = distance;
    g_closer_ms = now;
  } else if (now - g_closer_ms > (g_precise ? kNoCloserPreciseMs : kNoCloserMs)) {
    RememberWhatIsAhead(here, ahead, "where he got no closer for ten seconds");
    StopLocked("no closer for ten seconds - handing back to the journey");
    LOG_WARN("walk: {} ({:.1f} m short of leg {} of {})", g_note, distance,
             static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
    return false;
  }

  // Moving at all?
  if (now - g_window_ms >= kStuckMs) {
    const float moved = Distance2D(here, g_window_pos);
    g_window_ms  = now;
    g_window_pos = here;
    if (moved >= kStuckMetres) g_pushes = 0;   // moving again: the door gave
    if (moved < kStuckMetres && !airborne && now > g_sidestep_until &&
        now > g_pushing_until) {
      // Is it one of the server's own objects he is up against? Those are
      // what its doors and gates are made of, and a door is opened by
      // walking into it rather than by walking round it.
      // On a route the map drew, being stuck means the map is out of date -
      // a door has shut, somebody is standing in the corridor - and the
      // answer is another look, not a sidestep. Improvising here is what
      // walked him into the furniture in the first place.
      if (g_strict) {
        // Following a wall is not being stuck. The contact steerer is
        // already doing the one thing that works here, and every rule below
        // - the back-out, the door push, the hand-back - interrupts it in
        // the middle and starts it again from nothing.
        if (ContactGet().side != 0) {
          g_window_ms = now;
          g_window_pos = here;
          g_closer_ms = now;
          return true;
        }
        // Not before he has had time to turn and lean into it. From a
        // standstill, facing the wrong way, half a metre takes longer than
        // the stuck window - and calling that blocked threw the route away
        // and drew it again, over and over, with the character standing
        // perfectly still throughout.
        if (now - g_started_ms < kStrictGraceMs) {
          g_window_ms = now;
          g_window_pos = here;
          return true;
        }
        // A shut door on the map's route. The map went through it because
        // the server says it is a door, and a door is opened by walking into
        // it - so into it, not away from it. Backing out here is how he
        // reached the ward door, retreated from it, walked up to it and
        // retreated again, five times over, with the corridor beyond it
        // plotted and waiting.
        Vec3 door;
        if (g_pushes < kDoorPushes && NearestDoorway(here, &door, kDoorPushReach)) {
          ++g_pushes;
          g_pushing_until = now + kPushForMs;
          // Through the door, not at it: the route's own point beyond the
          // door is the direction, so the push goes through the frame and
          // not into the hinge side of the leaf.
          Vec3 through = door;
          for (std::size_t i = g_leg; i < g_route.size(); ++i) {
            const float dx = g_route[i].x - door.x, dy = g_route[i].y - door.y;
            if (std::sqrt(dx * dx + dy * dy) > 1.0f &&
                Distance2D(g_route[i], here) > Distance2D(door, here)) {
              through = g_route[i];
              break;
            }
          }
          g_push_at = through;
          g_pushing_at_something = true;
          g_closer_ms = now;
          g_window_ms = now;
          g_window_pos = here;
          LOG_INFO("walk: a shut door on the map's route at ({:.0f},{:.0f}) - "
                   "pushing it (push {})", door.x, door.y, g_pushes);
          return true;
        }
        // Otherwise the one thing a strict walk may improvise: a step back. Where he
        // starts is the one square the map never checked - it flooded out
        // from under his feet - and a character who spawned with his face in
        // a potted plant presses forward into it for ever. Backing out of
        // whatever he is wedged in is what a person does before anything
        // else, and it costs three metres.
        if (g_backouts < kMaxBackOuts && !airborne) {
          ++g_backouts;
          const float behind = Normalise(ahead + 3.14159265f);
          g_sidestep_target = Vec3{here.x + std::cos(behind) * kBackOutMetres,
                                   here.y + std::sin(behind) * kBackOutMetres,
                                   here.z};
          g_sidestep_until = now + kBackOutMs;
          g_window_ms = now;
          g_window_pos = here;
          g_closer_ms = now;
          LOG_INFO("walk: wedged at the start of the map's route - backing out "
                   "(try {})", g_backouts);
          return true;
        }
        StopLocked("the way the map found is blocked - looking again");
        LOG_INFO("walk: {} ({:.1f} m along it)", g_note, distance);
        return false;
      }
      // Unless he is already where he was sent. A destination is often a
      // thing - a counter, a bed, a pickup on the floor - and standing
      // against it is arriving, not being blocked. Leaning on it, shoving
      // it, then shoving it again is what a person never does and a machine
      // always does, and it happens in plain sight at the end of every walk.
      if (g_last_leg_is_the_destination && g_leg + 1 >= g_route.size() &&
          distance <= kTouchingDistance) {
        StopLocked("arrived - standing against what he was sent to");
        LOG_INFO("walk: {} ({:.1f} m from it)", g_note, distance);
        return false;
      }
      if (g_pushes < kPushesPerPlace) {
        const float reach = 1.6f;
        const Vec3 ahead_of_him{here.x + std::cos(ahead) * reach,
                                here.y + std::sin(ahead) * reach, here.z};
        // A door first, wherever it is within reach, because a door is what
        // opens; then whatever else is directly in front, because it might
        // be one the list does not know about.
        std::vector<samp::NearObject> things =
            samp::DoorsNear(here, kDoorSearch, 3);
        bool is_a_door = !things.empty();
        // Anything else of the server's is furniture, not a door. Leaning
        // on it twice for five seconds and then handing the route back is
        // what a chair by the reception desk cost him, over and over, while
        // the picture round him could see perfectly well how to walk past
        // it. Only worth trying when there is no picture to steer by.
        if (things.empty() && !g_precise)
          things = samp::ObjectsNear(ahead_of_him, kDoorReach, 3);
        if (!things.empty()) {
          const Vec3 what = things.front().at;
          ++g_pushes;
          g_pushing_until = now + kPushForMs;
          g_closer_ms = now;         // leaning on it is not standing still
          g_follow_side = 0;
          g_lean = 0;
          g_push_at = what;
          g_pushing_at_something = true;

          // A door is not somewhere to stand, it is somewhere to go through.
          // Leaning on it for a second and then heading off to the target
          // again is how he kept running past it, so the far side of it
          // becomes the next place he is walking to and the door is on the
          // way there.
          const float dx = what.x - here.x, dy = what.y - here.y;
          const float span = std::sqrt(dx * dx + dy * dy);
          const bool fresh_door = is_a_door && span > 0.2f &&
                                  (!g_door_known ||
                                   Distance2D(what, g_door_seen) > 2.0f);
          if (fresh_door) {
            g_door_known = true;
            g_door_seen = what;
            const Vec3 beyond{what.x + dx / span * kThroughDoor,
                              what.y + dy / span * kThroughDoor, what.z};
            if (g_leg <= g_route.size()) {
              g_route.insert(g_route.begin() + static_cast<long>(g_leg), beyond);
              g_best_distance = 0;
            }
            LOG_INFO("walk: a door at ({:.0f},{:.0f}), model {} - walking "
                     "through it to ({:.0f},{:.0f})", what.x, what.y,
                     things.front().model, beyond.x, beyond.y);
          } else {
            LOG_INFO("walk: {} in the way ({:.1f} m, model {}) at ({:.0f},{:.0f}) - "
                     "leaning on it (push {})",
                     is_a_door ? "a door" : "something of the server's",
                     things.front().away_m, things.front().model, what.x, what.y,
                     g_pushes);
          }
          return true;   // keep pressing, at it
        }
      }
      if (g_precise) {
        // Not before he has had time to turn and get going.
        if (now - g_started_ms < kStrictGraceMs) {
          g_window_ms = now;
          g_window_pos = here;
          return true;
        }
        // A shut door on the route. The map went through it because the
        // server says it is a door, and a door is opened by walking into
        // it - so into it, and through it, aiming at the route's own point
        // beyond so the push goes through the frame and not the hinge.
        Vec3 door;
        if (g_pushes < kDoorPushes && NearestDoorway(here, &door, kDoorPushReach)) {
          ++g_pushes;
          g_pushing_until = now + kPushForMs;
          Vec3 through = door;
          for (std::size_t i = g_leg; i < g_route.size(); ++i)
            if (Distance2D(g_route[i], door) > 1.0f &&
                Distance2D(g_route[i], here) > Distance2D(door, here)) {
              through = g_route[i];
              break;
            }
          g_push_at = through;
          g_pushing_at_something = true;
          g_closer_ms = now;
          g_window_ms = now;
          g_window_pos = here;
          LOG_INFO("walk: a shut door on the route at ({:.0f},{:.0f}) - pushing it "
                   "(push {})", door.x, door.y, g_pushes);
          return true;
        }
        // Wedged on top of something, or against something the picture
        // shows no way round: a hop is what a person does, and it is the
        // one improvisation that costs nothing and cannot walk him into a
        // trap. He stood on a hospital bed between two others for as long
        // as anybody let him, with the floor half a metre below and a step
        // to one side.
        if (now - g_wedged_hops_ms > kForgetWedgedMs) g_wedged_hops = 0;
        if (g_wedged_hops < kJumpsWhenWedged && std::fabs(vz) < kStillVertical &&
            now - g_last_jump_ms > kJumpAgainMs) {
          ++g_wedged_hops;
          g_wedged_hops_ms = now;
          g_hop_loose = true;
          WantJump(now, "wedged and getting nowhere - hopping out of it");
          g_closer_ms = now;
          g_window_ms = now;
          g_window_pos = here;
          return true;
        }
        // Standing against something the picture did not show - or did,
        // and he is wedged in it. Stepping round it by guesswork is what
        // walked him into traps; the spot is remembered and the journey
        // draws another route, which takes a second.
        const Vec3 spot{here.x + std::cos(ahead) * 0.9f,
                        here.y + std::sin(ahead) * 0.9f, here.z};
        nav::RememberObstacle(spot, "something he could not move past");
        StopLocked("not moving - handing back to the journey for another route");
        LOG_WARN("walk: {} ({:.1f} m short of leg {} of {})", g_note, distance,
                 static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
        return false;
      }
      RememberWhatIsAhead(here, ahead, "something he kept walking into");
      if (g_whisker_low[0] && now - g_last_jump_ms > 700 &&
          !(g_leg + 1 >= g_route.size() && distance < 6.0f)) {
        // Standing against something low: over it. Unless it is what he was
        // sent to, in which case standing against it is arriving.
        WantJump(now, "not moving against something low - jumping it");
      } else if (g_follow_side != 0 && g_side_changes < kMaxSideChanges) {
        // Going round it and not moving: that side is a dead end.
        g_follow_side = -g_follow_side;
        g_follow_since = now;
        ++g_side_changes;
        g_dead_end_probes = 0;
        LOG_INFO("walk: not moving, going round on the {} instead",
                 g_follow_side > 0 ? "left" : "right");
      } else if (g_sidesteps >= kMaxSidesteps) {
        StopLocked("stuck - stepped round it three times and still no way through");
        LOG_WARN("walk: {} ({:.1f} m short of leg {} of {})", g_note, distance,
                 static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
        return false;
      } else {
        // Out to one side of the way ahead, then carry on. Sides alternate,
        // so a corner that defeats one direction gets the other tried next.
        ++g_sidesteps;
        g_sidestep_left = !g_sidestep_left;
        const float side = ahead + (g_sidestep_left ? 1.5708f : -1.5708f);
        g_sidestep_target = Vec3{here.x + std::cos(side) * kSidestepMetres,
                                 here.y + std::sin(side) * kSidestepMetres,
                                 here.z};
        g_sidestep_until = now + kSidestepMs;
        LOG_INFO("walk: not moving, stepping {} (attempt {})",
                 g_sidestep_left ? "left" : "right", g_sidesteps);
      }
    }
  }

  // Full deflection is what the stick gets; the contact test needs to know
  // that to judge how far he should have gone.
  const float pace_for_contact = 1.0f;

  // Where he is going, and where he is going to be sent.
  const bool stepping = now < g_sidestep_until;
  if (now >= g_pushing_until) g_pushing_at_something = false;
  Vec3 aim = g_pushing_at_something ? g_push_at
             : stepping             ? g_sidestep_target
                                    : target;
  // Following closely, he heads for the line a few metres on, not the far
  // end of the leg.
  const bool pursuing = g_precise && !g_pushing_at_something && !stepping;
  if (pursuing) aim = PursuitPoint(here, kPursuitAhead);
  float wanted = std::atan2(aim.y - here.y, aim.x - here.x);

  // Look where he is going, and lean away from what is there.
  bool jump_low_now = false;
  if (now - g_probe_ms >= kProbeMs && game::CallsTrusted()) {
    g_probe_ms = now;
    // The ground under his own feet, for whether he is standing on it.
    float ground = 0;
    if (game::GroundBelow(Vec3{here.x, here.y, here.z + 0.5f}, &ground)) {
      g_on_ground = (here.z - 1.0f) - ground < kFeetOnGround;
      g_above_ground = (here.z - 1.0f) - ground;
      g_ground_known = true;
    } else {
      // Not knowing what is under him is not the same as being off it.
      g_on_ground = false;
      g_ground_known = false;
    }

    if (!g_strict && !g_precise) ProbeWhiskers(here, wanted, descending);
    // The last couple of metres of the last leg are different. A person
    // walking to a counter, a bed, a cash machine ends up touching it: the
    // thing he was sent to is in front of him, and leaning away from it is
    // how he circles a bed he was told to lie in. So close to where he was
    // sent, low things ahead stop being obstacles.
    const bool at_the_end =
        g_leg + 1 >= g_route.size() && distance <= kTouchingDistance;
    if (at_the_end) {
      for (int i = 0; i < kWhiskers; ++i)
        if (g_whisker_low[i]) {
          g_whisker_low[i] = false;
          g_whisker_clear[i] = true;
        }
    }
    if (g_strict) {
      g_lean = 0;
      g_wall = false;
      g_follow_side = 0;
      for (int i = 0; i < kWhiskers; ++i) {
        g_whisker_clear[i] = true;
        g_whisker_low[i] = false;
      }
    } else if (g_precise) {
      // The picture round him, fresh, and the route checked on it.
      g_lean = 0;
      g_wall = false;
      g_follow_side = 0;
      const Vec3 sent_to = g_route.empty() ? here : g_route.back();
      nav::PaintLocal(here, kLocalRadius,
                      nav::LocalBodies(here, kLocalRadius, &sent_to), &g_local);
      float low_at = -1.0f;
      const float free = RouteAhead(here, kRouteLook, &low_at);
      g_route_free = free;
      g_route_low_at = at_the_end ? -1.0f : low_at;
      // The whiskers' verdicts, for the sprint and the hop, from the picture.
      const float room = g_local.Clearance(here);
      g_whisker_clear[0] = free >= kRouteOpen;
      g_whisker_low[0] = g_route_low_at >= 0 && g_route_low_at < kRouteOpen;
      for (int i = 1; i < kWhiskers; ++i) {
        g_whisker_clear[i] = room >= 1.0f;
        g_whisker_low[i] = false;
      }
      if (free < kRouteBlockedNear && !at_the_end) {
        // Something across the route within a few strides. It may be a
        // thing the plan did not have - a car, a gate, a person - or it may
        // be the two pictures disagreeing about a cell by a hand's breadth,
        // which happens along every wall. So a block is not a reason to
        // stop by itself: he steers round it on the picture and keeps
        // going, and only if that gets him no nearer for a couple of
        // seconds is the plan wrong enough to draw again. Handing back on
        // the prediction alone is what had him replan the same two metres
        // every two seconds without moving.
        const Vec3 spot = RoutePoint(here, free);
        if (g_route_blocked_since == 0) g_route_blocked_since = now;
        // Somebody standing in the way is waited out - but not himself: the
        // ped list holds him too, and taking his own body for a stranger
        // had him stand three seconds at every step of the way.
        const bool somebody =
            !game::PedsNear(spot, 1.2f, 1, self.game_ped).empty() &&
            Distance2D(spot, here) > 1.0f;
        if (somebody && now - g_route_blocked_since < kWaitForPersonMs) {
          g_precise_delta = 0;
          g_closer_ms = now;   // waiting is not being stuck
          g_window_ms = now;
          g_window_pos = here;
          return false;        // stand
        }
        if (now - g_closer_ms > kBlockedNoProgressMs) {
          // A hop first. It is what a person does when a step will not do,
          // it costs nothing, and it cannot walk him into a trap - and it
          // is the difference between getting off a hospital bed he was
          // standing on and standing on it for as long as anybody lets him.
          // Steady on his feet is what matters, not what the ground probe
          // makes of him: standing on a hospital bed he reads as airborne,
          // because the ray under him finds the floor half a metre below
          // and not the bed he is on, and a hop refused on that account is
          // a hop refused exactly where it was needed.
          if (now - g_wedged_hops_ms > kForgetWedgedMs) g_wedged_hops = 0;
          if (g_wedged_hops < kJumpsWhenWedged && std::fabs(vz) < kStillVertical &&
              now - g_last_jump_ms > kJumpAgainMs) {
            ++g_wedged_hops;
            g_wedged_hops_ms = now;
            g_hop_loose = true;
            WantJump(now, "the way is shut and he is getting nowhere - hopping");
            g_closer_ms = now;
            g_window_ms = now;
            g_window_pos = here;
            return true;
          }
          const Vec3 past = RoutePoint(here, free + 0.4f);
          nav::RememberObstacle(past, somebody ? "somebody standing on the route"
                                               : "something across the route");
          const int legs = static_cast<int>(g_route.size()), leg = static_cast<int>(g_leg) + 1;
          const std::string picture = PictureAbout(here, spot, 3.0f);
          StopLocked("the route is blocked and he is getting no nearer - handing it back");
          LOG_WARN("walk: {} ({:.1f} m along it, {:.1f} m short of leg {} of {}); the picture, "
                   "three metres each way, x across, y up, S him, X the block:\n{}",
                   g_note, free, distance, leg, legs, picture);
          return false;
        }
      } else {
        g_route_blocked_since = 0;
      }
      g_precise_delta = PreciseDelta(here, wanted);
      // What the picture says, drawn in the world for the panel: eight
      // spokes, each as long as the way that way is open. Without this the
      // panel went on drawing the whiskers from whenever they last ran,
      // which is not what he steers by any more.
      {
        std::vector<Vec3> ends;
        std::vector<bool> clear;
        for (int k = 0; k < 8; ++k) {
          const float a = wanted + k * 0.7854f;
          const Vec3 far_end{here.x + std::cos(a) * kSteerLook,
                             here.y + std::sin(a) * kSteerLook, here.z};
          const float open = g_local.FreeAlong(here, far_end, kStartSlack, nullptr);
          ends.push_back(Vec3{here.x + std::cos(a) * open,
                              here.y + std::sin(a) * open, here.z});
          clear.push_back(open >= kSteerLook - 0.01f);
        }
        nav::SetDebugWhiskers(here, std::move(ends), std::move(clear));
      }
    } else {
      DecideLean(now);
    }
    // A door is not a wall to be got round. Within reach of one the route
    // means to go through, everything the whiskers found is the door frame,
    // and the only thing that opens it is his shoulder.
    if (DoorwayAhead(here, wanted)) {
      g_lean = 0;
      g_wall = false;
      g_follow_side = 0;
      g_whisker_low[0] = false;
      g_closer_ms = now;
    }
    if (g_wall) {
      // Everything ahead is blocked. From the middle of a room that is a
      // wall and the journey should hear about it; from the corner he has
      // just walked into it is a corner, and what a person does in a corner
      // is step back out of it. So he backs out first, and only calls it a
      // wall if backing out did not help either.
      if (g_backouts < kMaxBackOuts && !airborne) {
        ++g_backouts;
        const float behind = Normalise(wanted + 3.14159265f);
        g_sidestep_target = Vec3{here.x + std::cos(behind) * kBackOutMetres,
                                 here.y + std::sin(behind) * kBackOutMetres,
                                 here.z};
        g_sidestep_until = now + kBackOutMs;
        g_wall = false;
        g_lean = 0;
        g_follow_side = 0;
        g_closer_ms = now;
        g_window_ms = now;
        g_window_pos = here;
        LOG_INFO("walk: nothing ahead is open - backing out of it (try {})",
                 g_backouts);
        return true;
      }
      RememberWhatIsAhead(here, wanted, "a wall the plan did not know about");
      StopLocked("blocked - no way round from here, handing back to the journey");
      LOG_WARN("walk: {} ({:.1f} m short of leg {} of {})", g_note, distance,
               static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
      return false;
    }
    // Something low straight ahead: jump it when it is close.
    if (g_whisker_low[0] && g_follow_side == 0 && !airborne &&
        now - g_last_jump_ms > 700) {
      const float at = g_precise ? g_route_low_at
                                 : DistanceAlongWhisker(here, wanted, kWhiskerLength[0], kWaist);
      if (at <= kJumpAt) jump_low_now = true;
    }
  }
  if (pursuing) wanted = Normalise(wanted + g_precise_delta);

  // Indoors he is steered by what he actually walks into, not by what a map
  // predicted. The whiskers stay for the open street, where a plan really
  // can be ignorant of a parked car; in here the game answers the question
  // every frame by how far he got.
  // Indoors: look all the way round first and turn off before touching
  // anything, then let contact deal with whatever was not seen. The ring is
  // answered from the collision pools and the ped pool - the map of the room
  // as it is this instant, players included - and never by calling the game.
  float ring_aim = wanted;
  if (g_strict && now - g_ring_ms >= kRingEveryMs && game::CallSlotsLeft() > 600) {
    g_ring_ms = now;
    const nav::Ring ring = nav::LookRound(here, wanted, kRingReach);
    g_ring_turned = ring.ok && ring.turned;
    g_ring_steer = ring.ok ? ring.steer : wanted;
    g_ring_free = ring.ok ? ring.free_ahead_m : kRingReach;
  }
  if (g_strict && g_ring_turned) ring_aim = g_ring_steer;
  const float steered = g_strict
                            ? ContactSteer(here, ring_aim, pace_for_contact, now)
                            : Normalise(wanted + g_lean);

  // The stick is camera-relative: the game itself subtracts the camera's
  // orientation from the stick angle before it moves him. The camera stays
  // the player's - it is read here, never turned - and the frame follows
  // from its own number, so a spun view is simply a different number next
  // frame and never a wrong direction.
  float orientation = 0;
  const bool camera = game::CameraOrientation(&orientation);
  if (camera != g_camera_frame || !g_said_frame) {
    g_said_frame = true;
    g_camera_frame = camera;
    LOG_INFO("walk: steering {}", camera
        ? "from the camera's own orientation, the way the game does"
        : "by measurement - the camera's orientation is not readable");
    if (!camera) g_bootstrap_until = now + kBootstrapMs;
  }

  float offset = 0;
  float hand = kMirrored;
  bool bootstrapping = false;
  if (camera) {
    offset = Normalise(kHalfPi - orientation + g_correction);
    // The check on the derivation: facing steadily off a line that is not
    // moving. A turn is over in well under the time allowed; a wrong frame
    // never is.
    const float error = Normalise(self.heading - g_last_steered);
    const float frame_error_min = g_precise ? kGrossFrameError : kFrameErrorRadians;
    const bool going_freely = !g_precise || g_route_free >= kRouteOpen;
    if (g_offset_seen && settled && going_freely && std::fabs(error) > frame_error_min) {
      if (g_frame_error_since == 0 ||
          std::fabs(Normalise(steered - g_steered_at_error)) > kIntentSteadyRadians) {
        g_frame_error_since = now;
        g_steered_at_error  = steered;
      } else if (now - g_frame_error_since > kFrameErrorMs) {
        g_correction = Normalise(g_correction + error);
        g_corrected  = true;
        g_frame_error_since = 0;
        LOG_WARN("walk: facing {:.0f} degrees off a steady line for a second "
                 "and a half - the camera frame is off, correcting by that",
                 error * 57.2957795f);
      }
    } else {
      g_frame_error_since = 0;
    }
  } else {
    // The old way: measured off the character, who turns to face wherever
    // the stick sends him.
    bootstrapping = now < g_bootstrap_until;
    if (g_offset_seen && settled) {
      const float implied = Normalise(self.heading - g_last_emit);
      const float step = Normalise(implied - g_offset);
      g_offset = Normalise(g_offset + step * (bootstrapping ? 0.35f : 0.08f));
    }
    offset = g_offset;
    hand = g_hand;
  }
  const float emit = bootstrapping ? 0.0f : Normalise(steered - offset);
  g_last_emit    = emit;
  g_last_steered = steered;
  g_offset_seen  = true;

  // How far his facing is from where he is meant to be going. Kept for the
  // panel. Without the camera it also catches the one thing measurement
  // cannot fix by itself: a mirrored sideways axis makes every correction
  // push him further out, so the error never comes down.
  const float heading_error = Normalise(steered - self.heading);
  g_error_deg = heading_error * 57.2957795f;
  if (!camera && !bootstrapping && settled && std::fabs(heading_error) > 1.7453f) {
    if (g_wrong_since == 0) g_wrong_since = now;
    else if (now - g_wrong_since > 2000) {
      g_hand = -g_hand;
      g_corrected = true;
      g_wrong_since = 0;
      g_offset_seen = false;
      g_bootstrap_until = now + kBootstrapMs;
      LOG_INFO("walk: still pointing {:.0f} degrees away after correcting, so "
               "the sideways axis is the other way round - flipped",
               g_error_deg);
    }
  } else {
    g_wrong_since = 0;
  }

  // Full deflection, always - the way a keyboard does it.
  const float pace = 1.0f;

  // Run, when there is a clear way and somewhere to go - on the ground, and
  // never with a jump pending or under way. Jump while running when he is
  // on the ground and not about to turn; and jump the low thing ahead
  // whether running or not. The jump has priority: deciding one takes
  // sprint up at once, and the press follows with sprint still up.
  const bool way_clear = g_whisker_clear[0] && !g_whisker_low[0];
  // Open ground, not merely a clear line straight ahead. The hop that follows
  // is a runner's stride and nothing else - it gets him nowhere the running
  // would not - so it is not worth taking anywhere near a thing he could
  // land on. The centre whisker alone said nothing about the fence he was
  // running beside.
  bool nothing_about = true;
  for (int i = 0; i < 5; ++i)
    if (!g_whisker_clear[i] || g_whisker_low[i]) nothing_about = false;
  const bool could_sprint =
      g_sprint_on && !g_strict && !bootstrapping && !stepping && !airborne &&
      g_follow_side == 0 && way_clear && !descending &&
      g_remaining > kSprintMinRemaining &&
      std::fabs(heading_error) < kSprintMaxError;
  // Not at the door. A person walks the last few steps to a car, and the
  // car is exactly the low thing the whiskers want to hop over - which is
  // how he kept jumping past the one he had been sent to.
  const bool arriving = g_leg + 1 >= g_route.size() && distance < 6.0f;
  if (jump_low_now && !arriving && !g_strict) {
    WantJump(now, "something low ahead - jumping it");
  } else if (could_sprint && nothing_about && !arriving && g_hop_on &&
             settled && distance > kHopMinToNext &&
             now - g_last_jump_ms >= g_hop_gap_ms) {
    WantJump(now, nullptr);
    // The next after a gap of its own, and now and then a longer one. A hop
    // every 950 ms to the frame is a metronome, not a runner.
    g_hop_gap_ms = kHopIntervalMs + static_cast<unsigned long long>(std::rand() % 700);
    if (std::rand() % 5 == 0) g_hop_gap_ms += 1500;
  }
  if (g_jump_countdown >= 0) {
    g_sprinting = false;
    if (g_jump_countdown == 0) {
      g_jump_this_frame = true;
      g_last_jump_ms = now;
      ++g_jumps;
    }
    --g_jump_countdown;
  } else {
    g_sprinting = could_sprint;
  }

  // A hop meant to free him is taken with nothing held.
  if (g_hop_loose && g_jump_countdown < 0 && !airborne && now - g_last_jump_ms > 600)
    g_hop_loose = false;
  const float loose = g_hop_loose ? 0.0f : 1.0f;
  const float want_x = std::sin(emit) * hand * kFullStick * pace * loose;
  const float want_y = -std::cos(emit) * kFullStick * pace * loose;
  g_stick_x += (want_x - g_stick_x) * kStickEase;
  g_stick_y += (want_y - g_stick_y) * kStickEase;
  // The direction eases; the deflection stays full, as a keyboard's does.
  const float magnitude = std::sqrt(g_stick_x * g_stick_x + g_stick_y * g_stick_y);
  const float scale = magnitude > 1.0f ? kFullStick / magnitude : 0.0f;
  *out_x = static_cast<short>(g_stick_x * scale);
  *out_y = static_cast<short>(g_stick_y * scale);
  return true;
}

void PadFrameInner() {
  if (!g_installed.load(std::memory_order_acquire)) return;
  g_pad_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);

  // End of the frame. What is written now sits in the keyboard's temp state
  // until the next CPad::UpdatePads reconciles it into the pad and clears
  // it: one frame's press, the way a held key arrives. Only while a walk is
  // running - the rest of the time the pad is the player's alone.
  // Not while a dialog is up, the server has frozen him, he is not spawned,
  // the game's menu is open or SA-MP has taken the keyboard away: the route
  // waits, the pad stays the game's, and the walk's own clocks wait too, so
  // that standing still is not read as being stuck.
  const char* why = "";
  const bool off = samp::InputLegitimatelyOff(&why);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held_by = off ? why : "";
  }
  if (g_test_keys.load(std::memory_order_relaxed)) {
    // The experiment: keys and nothing else. No walk is decided, no probe
    // is made, no call into the game.
    if (off || !KeysMayGo()) {
      if (g_held != 0) HoldKeys(0);
      return;
    }
    ReadBindings();
    HoldKeys(kFwd | kSprintKey);
    return;
  }
  short x = 0, y = 0;
  bool press = false;
  bool sprint = false, jump = false;
  int  countdown = -1;
  unsigned long long since_jump = 0;
  bool on_ground = true;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (off) {
      const unsigned long long now = GetTickCount64();
      g_window_ms = now;
      g_closer_ms = now;
      g_jump_this_frame = false;
      g_sprinting = false;
    } else {
      press = DecideStick(&x, &y);
    }
    sprint     = g_sprinting;
    jump       = g_jump_this_frame;
    countdown  = g_jump_countdown;
    since_jump = GetTickCount64() - g_last_jump_ms;
    on_ground  = g_on_ground;
  }
  {
    static bool was_off = false;
    if (off != was_off) {
      was_off = off;
      if (off)
        LOG_INFO("walk: holding still - {} (a moment ago: stick {},{}, sprint {}, "
                 "jump countdown {}, last jump {} ms ago, on ground {})",
                 why, g_last_x, g_last_y, g_last_sprint, countdown, since_jump,
                 on_ground);
      else
        LOG_INFO("walk: moving again");
    }
  }
  const bool may = KeysMayGo();
  if (off || !press || !may) {
    if (g_held != 0) HoldKeys(0);
    if (press && !off && !may && !g_said_background) {
      g_said_background = true;
      LOG_INFO("walk: waiting - the game window is not in front, or the panel's "
               "menu is open; keys go only to the game");
    }
    return;
  }
  g_said_background = false;
  ReadBindings();
  // The direction as keys, and sprint and jump as keys, through the
  // system. Never sprint and jump together: sprint came up two frames
  // before the jump was decided on, and it is let go on the jump frame.
  // The jump key is down for one frame - the game jumps on the press.
  unsigned want = DirectionKeys(x, y);
  const unsigned long long now_ms = GetTickCount64();
  if (jump) g_jump_release_ms = now_ms + kJumpHoldMs;
  const bool jumping = now_ms < g_jump_release_ms;
  if (sprint && !jumping) want |= kSprintKey;
  if (jumping) want |= kJumpKey;
  HoldKeys(want);
  g_last_x = x;
  g_last_y = y;
  g_last_sprint = sprint;
}

}  // namespace

void PadFrame() { PadFrameInner(); }

void HoldTestKeys(bool hold) {
  g_test_keys.store(hold, std::memory_order_relaxed);
  if (!hold && GetCurrentThreadId() == g_pad_thread.load(std::memory_order_relaxed) &&
      g_held != 0)
    HoldKeys(0);
}

unsigned long long KeyEventsSent() {
  return g_key_events.load(std::memory_order_relaxed);
}

bool CutTo(std::size_t leg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_walking || leg >= g_route.size() || leg <= g_leg) return false;
  g_leg = leg;
  g_best_distance = 0;
  g_closer_ms = GetTickCount64();
  g_sidestep_until = 0;
  g_follow_side = 0;
  return true;
}

bool Install() {
  if (g_installed.load()) return true;
  if (!asi::WindowMode::WalkerAllowed()) {
    static bool said = false;
    if (!said) {
      said = true;
      LOG_INFO("walker: bot.cfg says walker=off - the character cannot be walked");
    }
    return false;
  }
  if (Pad() == 0) {
    LOG_WARN("walker: not the build CPad is known for - the character cannot "
             "be walked");
    return false;
  }
  g_installed.store(true, std::memory_order_release);
  LOG_INFO("walker ready: the walk is the player's own keys pressed through "
           "the system, from the frame hook");
  return true;
}

void Uninstall() {
  if (!g_installed.load()) return;
  Stop("stopped - shutting down");
  g_installed.store(false);
}

void WalkTo(std::vector<Vec3> route) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (route.empty()) {
    StopLocked("nothing to walk to");
    return;
  }
  const unsigned long long now = GetTickCount64();
  g_route = std::move(route);
  g_doorways.clear();
  g_backouts = 0;
  g_strict = false;
  g_precise = false;
  g_precise_delta = 0;
  g_route_free = 0;
  g_route_low_at = -1.0f;
  g_route_blocked_since = 0;
  g_local.ok = false;
  g_last_leg_is_the_destination = false;
  g_ring_ms = 0;
  g_ring_turned = false;
  ContactReset();
  g_leg = 0;
  g_walking = true;
  g_note = "walking";
  g_started_ms = now;
  g_window_ms = now;
  g_best_distance = 0;
  g_closer_ms = now;
  g_offset_seen = false;
  // Only the measured fallback needs a moment of pushing forward to learn
  // its frame; with the camera readable the first frame is already right.
  g_bootstrap_until = g_camera_frame ? 0 : now + kBootstrapMs;
  g_wrong_since = 0;
  g_frame_error_since = 0;
  g_error_deg = 0;
  g_sidesteps = 0;
  g_sidestep_until = 0;
  g_stick_x = 0;
  g_stick_y = 0;
  g_probe_ms = 0;
  g_lean = 0;
  g_follow_side = 0;
  g_dead_end_probes = 0;
  g_side_changes = 0;
  g_centre_clear = 0;
  g_wall = false;
  g_jumps = 0;
  g_jump_countdown = -1;
  g_hanging_since = 0;
  g_hop_loose = false;
  g_pushing_until = 0;
  g_pushes = 0;
  g_door_known = false;
  g_letting_go_until = 0;
  g_lets_go = 0;
  g_on_ground = true;
  for (int i = 0; i < kWhiskers; ++i) {
    g_whisker_clear[i] = true;
    g_whisker_low[i] = false;
  }
  const samp::LocalPed self = samp::ReadLocalPed();
  if (self.valid) g_window_pos = Vec3{self.x, self.y, self.z};
  g_route_start = self.valid ? g_window_pos : g_route.front();
  // What is left, from the start. The journey reads it before the first
  // step is decided, and a zero there reads as "nearly there" - which, with
  // the walker held still, was a plan every four seconds until it gave up.
  g_to_next = self.valid ? Distance2D(g_window_pos, g_route.front()) : 0;
  g_remaining = g_to_next;
  for (std::size_t i = 1; i < g_route.size(); ++i)
    g_remaining += Distance2D(g_route[i - 1], g_route[i]);
  LOG_INFO("walk: {} legs, first at ({:.1f}, {:.1f})", g_route.size(),
           g_route.front().x, g_route.front().y);
}

void SetStrictRoute(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_strict = on;
}

std::string LocalPictureText(float reach) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) return "(the character cannot be read)";
  const Vec3 here{self.x, self.y, self.z};
  nav::PaintLocal(here, std::max(kLocalRadius, reach + 0.5f),
                  nav::LocalBodies(here, kLocalRadius), &g_local);
  char head[160];
  std::snprintf(head, sizeof(head),
                "%d ground reads, %d entities%s; S him at (%.1f, %.1f, feet %.2f)\n",
                g_local.ground_reads, g_local.entities, g_local.starved ? ", STARVED" : "",
                here.x, here.y, here.z - 1.0f);
  return head + PictureAbout(here, here, reach);
}

void SetPrecise(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_precise = on;
  if (on) LOG_INFO("walk: following the route closely, steering on the local picture");
}

void SetLastLegIsTheDestination(bool it_is) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_leg_is_the_destination = it_is;
}

void SetDoorways(std::vector<Vec3> doorways) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_doorways = std::move(doorways);
}

void SetArriveWithin(float metres) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (metres <= 0) {
    g_arrive_last = kArriveLast;
    return;
  }
  g_arrive_last = metres < kArriveFloor ? kArriveFloor : metres;
}

void Stop(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_walking) LOG_INFO("walk: {}", why);
  StopLocked(why);
}

void SetSprint(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sprint_on = on;
}

void SetBunnyHop(bool on) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hop_on = on;
}

bool Sprint() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_sprint_on;
}

bool BunnyHop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_hop_on;
}

Status Get() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Status status;
  status.walking     = g_walking;
  status.leg         = static_cast<int>(g_leg);
  status.legs        = static_cast<int>(g_route.size());
  status.to_next_m   = g_to_next;
  status.remaining_m = g_remaining;
  status.note        = g_note;
  status.held_by     = g_held_by;
  status.corrected   = g_corrected;
  status.error_deg   = g_error_deg;
  status.sidesteps   = g_sidesteps;
  status.steer_deg   = g_lean * 57.2957795f;
  status.wall        = g_follow_side != 0;
  status.sprinting   = g_sprinting;
  status.jumps       = g_jumps;
  return status;
}

}  // namespace gtabot::act
