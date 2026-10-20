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
// gesture voice's map: pitch voices always target their pos-1 extreme
// (Whammy polarity comes from Direction, NOT from which map end), while
// Ring follows Direction (Up = carrier rises to toe, Down = falls to
// heel); targetPos() scales the target by Depth (semitone magnitude for
// pitch voices, sweep range for Ring). The engine renders gesture voices
// with the SAME maps as normal voices, so depth lives entirely in the
// target. To-Depth across voices sweeps heel -> target absolutely
// (legacy dive shape); same-voice gestures and From-Depth anchor at the
// live base.

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
  int voice = 0; // SpecLaser::Voice as int (unknown ids ignored)
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

// Voice ids (SpecLaser::Voice as int): Whammy 0, Dual 1, Ring 2, DualWham 3.
constexpr int kVoiceWhammy = 0;
constexpr int kVoiceDual = 1;
constexpr int kVoiceRing = 2;
constexpr int kVoiceDualWham = 3;

// Which map end (0/1) is the gesture's depth target. Pitch voices always
// target pos 1 (Whammy polarity comes from Direction selecting the leg,
// not the map end; Dual/DualWham spread upward); Ring follows Direction.
inline int gestureDepthEnd(int voice, GestureDir dir)
{
  if (voice == kVoiceRing)
    return (dir == GestureDir::Up) ? 1 : 0;
  return 1;
}

// Single source of truth for control gating (UI enable/disable): Whammy
// expression takes Direction (polarity) + Depth; Dual/DualWham take Depth
// only (fixed spread/up); Ring expression is fixed-map (its character).
// Gestures add Direction for Ring (carrier sweep direction).
inline bool exprVoiceUsesDir(int voice)
{
  return voice == kVoiceWhammy;
}
inline bool exprVoiceUsesDepth(int voice)
{
  return voice != kVoiceRing;
}
inline bool gestureVoiceUsesDir(int voice)
{
  return voice == kVoiceWhammy || voice == kVoiceRing;
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

// Voice ids: kVoiceWhammy 0, kVoiceDual 1, kVoiceRing 2, kVoiceDualWham 3.
// Unified Whammy: Direction selects polarity (dives = Whammy + Down).
inline GestureConfig defaultGestureConfig(int slot)
{
  GestureConfig c;
  switch (slot)
  {
  case 0: // Fast Toe: legacy stab (Whammy/Up/To/12/50/100)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 50.0f;
    c.retMs = 100.0f;
    break;
  case 1: // Slow Dive (Whammy/Down/To/12/600/300)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 600.0f;
    c.retMs = 300.0f;
    break;
  case 2: // Reverse Dive (Whammy/Down/From/12/400/200)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::FromDepth;
    c.depthSt = 12.0f;
    c.timeMs = 400.0f;
    c.retMs = 200.0f;
    break;
  case 3: // Reverse Toe (Whammy/Up/From/12/250/150)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::FromDepth;
    c.depthSt = 12.0f;
    c.timeMs = 250.0f;
    c.retMs = 150.0f;
    break;
  case 4: // Ring Sweep (Ring/Up/To/12/500/300)
    c.voice = kVoiceRing;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 500.0f;
    c.retMs = 300.0f;
    break;
  case 5: // Half Toe (Whammy/Up/To/6/100/150)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Up;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 6.0f;
    c.timeMs = 100.0f;
    c.retMs = 150.0f;
    break;
  case 6: // Deep Drop (Whammy/Down/To/12/150/400)
    c.voice = kVoiceWhammy;
    c.dir = GestureDir::Down;
    c.motion = GestureMotion::ToDepth;
    c.depthSt = 12.0f;
    c.timeMs = 150.0f;
    c.retMs = 400.0f;
    break;
  case 7: // Ring Fall (Ring/Down/To/12/400/200)
  default:
    c.voice = kVoiceRing;
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
