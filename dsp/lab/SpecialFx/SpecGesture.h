#pragma once

// SpecGesture: performance-gesture configuration for the Special FX Laser
// lab (dsp/lab/SpecialFx). No DSP here — plain structs, factory slot
// defaults, and the depth-target math shared by the engine, the router,
// and (later) MIDI/CoreMIDI + DAW automation. Slots are identified by
// stable 0-based IDs; names are UI-only and never address gestures.
//
// A gesture temporarily takes control of creative pitch: on press it moves
// from the current expression state toward a configured depth target over
// Time (To Depth) or engages AT the depth and sweeps back toward normal
// over Time (From Depth); on release it returns to the live expression
// state over Return. The depth target is a POSITION (0..1) in the
// gesture voice's map: depthEnd() picks which map end sounds like the
// configured Direction (WhamDown flips: its pos 1 IS pitch-down), and
// targetPos() scales it by Depth (semitone magnitude for pitch voices,
// sweep range for Ring). The engine renders gesture voices with the SAME
// maps as normal voices, so depth lives entirely in the target. To-Depth
// across voices sweeps heel -> target absolutely (legacy dive shape);
// same-voice gestures and From-Depth anchor at the live base.

namespace tdm
{
namespace specfx
{

enum class GestureDir
{
  Up = 0,
  Down = 1
};

enum class GestureMotion
{
  ToDepth = 0,   // press: normal -> depth over Time; release: -> normal
  FromDepth = 1  // press: engage AT depth, sweep -> normal over Time
};

struct GestureConfig
{
  int voice = 0; // SpecLaser::Voice as int (Detune unsupported: ignored)
  GestureDir dir = GestureDir::Up;
  GestureMotion motion = GestureMotion::ToDepth;
  float depthSt = 12.0f; // 0..12 (validated ceiling; UI + engine clamp)
  float timeMs = 100.0f; // primary movement
  float retMs = 100.0f;  // return to normal
};

constexpr int kNumGestureSlots = 8;
constexpr float kMaxGestureDepthSt = 12.0f; // validated Doppler ceiling
constexpr float kMaxGestureMs = 2000.0f;
constexpr float kGestureEngageMs = 10.0f; // From-Depth press fade (click-safe)

// Which map end (0/1) is the gesture's depth target. WhamDown's pos 1 is
// its pitch-down extreme, so it flips; Dual Down collapses to unison;
// Ring follows carrier direction. voiceWhamDown: pass (v == Voice::WhamDown).
inline int gestureDepthEnd(bool voiceWhamDown, GestureDir dir)
{
  const bool up = (dir == GestureDir::Up);
  return ((up != voiceWhamDown) ? 1 : 0);
}

// Depth-scaled target position: full depth reaches the map end, 0 stays
// at the gesture heel (harmless press). depthSt clamped to 0..12.
inline float gestureTargetPos(int depthEnd, float depthSt)
{
  float d = (depthSt < 0.0f) ? 0.0f : ((depthSt > kMaxGestureDepthSt) ? kMaxGestureDepthSt : depthSt);
  const float f = d / 12.0f; // 12/12 == 1.0f exactly
  return (depthEnd == 1) ? f : (1.0f - f);
}

// Factory slot defaults (in-memory; Save/Reset edit copies in the router).
// Slots 1-5 follow the refactor brief; 6-8 are useful spare starting points.
inline const char* defaultGestureName(int slot)
{
  switch (slot)
  {
  case 0: return "Fast Toe";
  case 1: return "Slow Dive";
  case 2: return "Reverse Dive";
  case 3: return "Reverse Toe";
  case 4: return "Ring Sweep";
  case 5: return "Half Toe";
  case 6: return "Deep Drop";
  case 7: return "Ring Fall";
  default: return "Gesture";
  }
}

// Voice ids: SpecLaser::Voice as int (WhamUp 0, Dual 1, Ring 2, Detune 3,
// WhamDown 4). Detune is not offered (unsupported as a gesture voice).
inline GestureConfig defaultGestureConfig(int slot)
{
  GestureConfig c;
  switch (slot)
  {
  case 0: // Fast Toe: legacy stab (WhamUp/Up/To/12/50/100)
    c.voice = 0;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 50.0f;
    c.retMs = 100.0f;
    break;
  case 1: // Slow Dive (WhamDown/Down/To/12/600/300)
    c.voice = 4;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 600.0f;
    c.retMs = 300.0f;
    break;
  case 2: // Reverse Dive (WhamDown/Down/From/12/400/200)
    c.voice = 4;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::FromDepth;
    c.depthSt = 12.0f;
    c.timeMs = 400.0f;
    c.retMs = 200.0f;
    break;
  case 3: // Reverse Toe (WhamUp/Up/From/12/250/150)
    c.voice = 0;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::FromDepth;
    c.depthSt = 12.0f;
    c.timeMs = 250.0f;
    c.retMs = 150.0f;
    break;
  case 4: // Ring Sweep (Ring/Up/To/12/500/300)
    c.voice = 2;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 500.0f;
    c.retMs = 300.0f;
    break;
  case 5: // Half Toe (WhamUp/Up/To/6/100/150)
    c.voice = 0;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 6.0f;
    c.timeMs = 100.0f;
    c.retMs = 150.0f;
    break;
  case 6: // Deep Drop (WhamDown/Down/To/12/150/400)
    c.voice = 4;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 150.0f;
    c.retMs = 400.0f;
    break;
  case 7: // Ring Fall (Ring/Down/To/12/400/200)
  default:
    c.voice = 2;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 400.0f;
    c.retMs = 200.0f;
    break;
  }
  return c;
}

} // namespace specfx
} // namespace tdm
