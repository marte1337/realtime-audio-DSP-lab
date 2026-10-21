// MidiPersist implementation: minimal JSON DOM + reader/writer.

#include "host/midi/MidiPersist.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace tdm
{
namespace midi
{
namespace
{

const char* kTypeNames[] = {"cc", "noteOn", "noteOff", "program"};
const char* kActionNames[] = {"continuous", "momentary", "oneShot", "toggle"};
const char* kTargetNames[] = {"specExpr",   "specMix", "exprEnable", "gestEnable",
                              "slot0",      "slot1",   "slot2",      "slot3",
                              "slot4",      "slot5",   "slot6",      "slot7"};

bool typeFromString(const std::string& s, MsgType& out)
{
  for (int i = 0; i < 4; ++i)
    if (s == kTypeNames[i])
    {
      out = static_cast<MsgType>(i);
      return true;
    }
  return false;
}
bool actionFromString(const std::string& s, Action& out)
{
  for (int i = 0; i < 4; ++i)
    if (s == kActionNames[i])
    {
      out = static_cast<Action>(i);
      return true;
    }
  return false;
}
bool targetFromString(const std::string& s, Target& out)
{
  for (int i = 0; i < 12; ++i)
    if (s == kTargetNames[i])
    {
      out = static_cast<Target>(i);
      return true;
    }
  return false;
}

// --- Minimal JSON DOM (only what we write/read) ---

struct JVal
{
  enum T
  {
    Nul,
    Bool,
    Num,
    Str,
    Arr,
    Obj
  };
  T type = Nul;
  bool b = false;
  double d = 0.0;
  std::string s;
  std::vector<JVal> a;
  std::vector<std::pair<std::string, JVal>> o;
  const JVal* find(const std::string& k) const
  {
    if (type != Obj)
      return nullptr;
    for (const auto& kv : o)
      if (kv.first == k)
        return &kv.second;
    return nullptr;
  }
};

struct Reader
{
  const char* p;
  const char* end;
  bool ok = true;
  Reader(const char* p0, const char* p1) : p(p0), end(p1) {}
  void ws()
  {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      ++p;
  }
  bool lit(const char* s)
  {
    const size_t n = std::strlen(s);
    if ((size_t)(end - p) < n || std::memcmp(p, s, n) != 0)
    {
      ok = false;
      return false;
    }
    p += n;
    return true;
  }
  bool parseStr(std::string& out)
  {
    if (p >= end || *p != '"')
    {
      ok = false;
      return false;
    }
    ++p;
    out.clear();
    while (p < end && *p != '"')
    {
      if (*p != '\\')
      {
        out += *p++;
        continue;
      }
      ++p;
      if (p >= end)
        break;
      const char e = *p++;
      switch (e)
      {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u':
      {
        unsigned cp = 0;
        for (int k = 0; k < 4 && p < end; ++k, ++p)
        {
          cp <<= 4;
          const char c = *p;
          if (c >= '0' && c <= '9')
            cp |= (unsigned)(c - '0');
          else if (c >= 'a' && c <= 'f')
            cp |= (unsigned)(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F')
            cp |= (unsigned)(c - 'A' + 10);
          else
          {
            ok = false;
            return false;
          }
        }
        if (cp < 0x80)
          out += (char)cp;
        else if (cp < 0x800)
        {
          out += (char)(0xC0 | (cp >> 6));
          out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
          out += (char)(0xE0 | (cp >> 12));
          out += (char)(0x80 | ((cp >> 6) & 0x3F));
          out += (char)(0x80 | (cp & 0x3F));
        }
        break;
      }
      default: ok = false; return false;
      }
      if (out.size() > 4096)
      {
        ok = false;
        return false;
      }
    }
    if (p >= end)
    {
      ok = false;
      return false;
    }
    ++p; // closing quote
    return true;
  }
  bool parseNum(double& out)
  {
    char* stop = nullptr;
    out = std::strtod(p, &stop);
    if (stop == p || !std::isfinite(out))
    {
      ok = false;
      return false;
    }
    p = stop;
    return true;
  }
  bool parseVal(JVal& out, int depth)
  {
    if (depth > 32)
    {
      ok = false;
      return false;
    }
    ws();
    if (p >= end)
    {
      ok = false;
      return false;
    }
    if (*p == '{')
    {
      ++p;
      out.type = JVal::Obj;
      ws();
      if (p < end && *p == '}')
      {
        ++p;
        return true;
      }
      while (ok)
      {
        ws();
        std::string k;
        JVal v;
        if (!parseStr(k))
          return false;
        ws();
        if (p >= end || *p != ':')
        {
          ok = false;
          return false;
        }
        ++p;
        if (!parseVal(v, depth + 1))
          return false;
        out.o.emplace_back(k, v);
        if (out.o.size() > 256)
        {
          ok = false;
          return false;
        }
        ws();
        if (p >= end)
        {
          ok = false;
          return false;
        }
        if (*p == ',')
        {
          ++p;
          continue;
        }
        if (*p == '}')
        {
          ++p;
          return true;
        }
        ok = false;
        return false;
      }
      return ok;
    }
    if (*p == '[')
    {
      ++p;
      out.type = JVal::Arr;
      ws();
      if (p < end && *p == ']')
      {
        ++p;
        return true;
      }
      while (ok)
      {
        JVal v;
        if (!parseVal(v, depth + 1))
          return false;
        out.a.push_back(v);
        if (out.a.size() > 256)
        {
          ok = false;
          return false;
        }
        ws();
        if (p >= end)
        {
          ok = false;
          return false;
        }
        if (*p == ',')
        {
          ++p;
          continue;
        }
        if (*p == ']')
        {
          ++p;
          return true;
        }
        ok = false;
        return false;
      }
      return ok;
    }
    if (*p == '"')
    {
      out.type = JVal::Str;
      return parseStr(out.s);
    }
    if (*p == 't')
    {
      if (!lit("true"))
        return false;
      out.type = JVal::Bool;
      out.b = true;
      return true;
    }
    if (*p == 'f')
    {
      if (!lit("false"))
        return false;
      out.type = JVal::Bool;
      out.b = false;
      return true;
    }
    if (*p == 'n')
    {
      if (!lit("null"))
        return false;
      out.type = JVal::Nul;
      return true;
    }
    out.type = JVal::Num;
    return parseNum(out.d);
  }
};

bool jBool(const JVal* v, bool fallback)
{
  return (v != nullptr && v->type == JVal::Bool) ? v->b : fallback;
}
std::string jStr(const JVal* v, const std::string& fallback)
{
  return (v != nullptr && v->type == JVal::Str) ? v->s : fallback;
}
int jClampInt(const JVal* v, int lo, int hi, int fallback)
{
  if (v == nullptr || v->type != JVal::Num)
    return fallback;
  int i = (int)std::llround(v->d);
  if (i < lo)
    i = lo;
  if (i > hi)
    i = hi;
  return i;
}
float jClampFloat(const JVal* v, float lo, float hi, float fallback)
{
  if (v == nullptr || v->type != JVal::Num)
    return fallback;
  float f = (float)v->d;
  if (!(f >= lo))
    f = lo;
  if (!(f <= hi))
    f = hi;
  return f;
}

void writeEscaped(std::string& out, const std::string& s)
{
  out += '"';
  for (char c : s)
  {
    switch (c)
    {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if ((unsigned char)c < 0x20)
      {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      }
      else
        out += c;
    }
  }
  out += '"';
}

} // namespace

PersistedState defaultState()
{
  PersistedState s;
  for (int i = 0; i < specfx::kNumGestureSlots; ++i)
  {
    s.slots[i].name = specfx::defaultGestureName(i);
    s.slots[i].cfg = specfx::defaultGestureConfig(i);
  }
  return s;
}

std::string saveJson(const PersistedState& s)
{
  std::string o = "{\n  \"version\": 1,\n";
  o += std::string("  \"omniLearn\": ") + (s.omniLearn ? "true" : "false") + ",\n";
  o += "  \"midiSources\": [";
  for (size_t i = 0; i < s.midiSources.size(); ++i)
  {
    if (i > 0)
      o += ", ";
    o += std::to_string(s.midiSources[i]);
  }
  o += "],\n  \"mappings\": [\n";
  bool first = true;
  for (size_t i = 0; i < s.mappings.size(); ++i)
  {
    const Mapping& m = s.mappings[i];
    const int ty = (int)m.trigger.type, tg = (int)m.target, ac = (int)m.action;
    if (ty < 0 || ty > 3 || tg < 0 || tg > 11 || ac < 0 || ac > 3)
      continue; // invalid entries never persist (Other/None skipped)
    if (!first)
      o += ",\n";
    first = false;
    o += "    {\"src\": " + std::to_string(m.trigger.sourceUid)
        + ", \"ch\": " + std::to_string((int)m.trigger.channel) + ", \"type\": \""
        + kTypeNames[(int)m.trigger.type] + "\", \"num\": " + std::to_string((int)m.trigger.number)
        + ", \"target\": \"" + kTargetNames[(int)m.target] + "\", \"action\": \""
        + kActionNames[(int)m.action] + "\", \"invert\": " + (m.invert ? "true" : "false")
        + ", \"min\": " + std::to_string((int)m.calMin) + ", \"max\": "
        + std::to_string((int)m.calMax) + "}";
  }
  if (!first)
    o += "\n";
  o += "  ],\n  \"gestureSlots\": [\n";
  for (int i = 0; i < specfx::kNumGestureSlots; ++i)
  {
    const PersistedSlot& sl = s.slots[i];
    char nums[160];
    std::snprintf(nums, sizeof(nums), "%d, \"dir\": %d, \"motion\": %d, \"depth\": %.3f, \"time\": %.3f, "
                                      "\"ret\": %.3f}",
                  sl.cfg.voice, (int)sl.cfg.dir, (int)sl.cfg.motion, (double)sl.cfg.depthSt,
                  (double)sl.cfg.timeMs, (double)sl.cfg.retMs);
    o += "    {\"name\": ";
    writeEscaped(o, sl.name.substr(0, 64));
    o += std::string(", \"voice\": ") + nums;
    o += (i + 1 < specfx::kNumGestureSlots) ? ",\n" : "\n";
  }
  o += "  ]\n}\n";
  return o;
}

bool loadJson(const std::string& text, PersistedState& out, std::string& error)
{
  PersistedState s = defaultState();
  Reader r(text.data(), text.data() + text.size());
  JVal root;
  if (!r.parseVal(root, 0) || !r.ok)
  {
    error = "not valid JSON";
    return false;
  }
  r.ws();
  if (r.p != r.end)
  {
    error = "trailing data after JSON document";
    return false;
  }
  if (root.type != JVal::Obj)
  {
    error = "root is not an object";
    return false;
  }
  if (jClampInt(root.find("version"), 0, 9999, -1) != 1)
  {
    error = "unsupported version (want 1)";
    return false;
  }
  s.omniLearn = jBool(root.find("omniLearn"), false);
  if (const JVal* srcs = root.find("midiSources"))
    if (srcs->type == JVal::Arr)
      for (const JVal& v : srcs->a)
        if (v.type == JVal::Num && v.d >= 0.0 && v.d < 4294967296.0)
          s.midiSources.push_back((uint32_t)v.d);
  if (const JVal* maps = root.find("mappings"))
    if (maps->type == JVal::Arr)
      for (const JVal& v : maps->a)
      {
        if (v.type != JVal::Obj)
          continue;
        Mapping m;
        if (const JVal* su = v.find("src"))
          if (su->type == JVal::Num && su->d >= 0.0 && su->d < 4294967296.0)
            m.trigger.sourceUid = (uint32_t)su->d;
        m.trigger.channel = (int8_t)jClampInt(v.find("ch"), -1, 15, -1);
        m.trigger.number = (uint8_t)jClampInt(v.find("num"), 0, 127, 0);
        MsgType ty = MsgType::Other;
        Target tg = Target::None;
        Action ac = Action::Continuous;
        if (!typeFromString(jStr(v.find("type"), ""), ty) || ty == MsgType::Other)
          continue; // unknown type: skip entry, keep the file
        if (ty == MsgType::NoteOff)
          ty = MsgType::NoteOn; // stored triggers are always key identity
        if (!targetFromString(jStr(v.find("target"), ""), tg))
          continue;
        if (!actionFromString(jStr(v.find("action"), ""), ac))
          continue;
        if (!MappingTable::compatible(tg, ac))
          continue;
        if (ac == Action::Momentary)
        {
          Trigger probe = m.trigger;
          probe.type = ty;
          if (!MappingTable::triggerHasRelease(probe))
            continue; // PC momentary: never fabricated
        }
        m.trigger.type = ty;
        m.target = tg;
        m.action = ac;
        m.invert = jBool(v.find("invert"), false);
        m.calMin = (uint8_t)jClampInt(v.find("min"), 0, 127, 0);
        m.calMax = (uint8_t)jClampInt(v.find("max"), 0, 127, 127);
        if (m.calMax <= m.calMin)
        {
          m.calMin = 0;
          m.calMax = 127;
        }
        s.mappings.push_back(m);
        if ((int)s.mappings.size() >= MappingTable::kMaxMappings)
          break;
      }
  if (const JVal* slots = root.find("gestureSlots"))
    if (slots->type == JVal::Arr)
      for (size_t i = 0; i < slots->a.size() && i < (size_t)specfx::kNumGestureSlots; ++i)
      {
        const JVal& v = slots->a[i];
        if (v.type != JVal::Obj)
          continue;
        PersistedSlot& sl = s.slots[i]; // starts at factory: partial entries safe
        sl.name = jStr(v.find("name"), sl.name).substr(0, 64);
        if (const JVal* vv = v.find("voice")) // unknown voice id: keep factory
          if (vv->type == JVal::Num)
          {
            const int vi = (int)std::llround(vv->d);
            if (vi >= 0 && vi <= 3)
              sl.cfg.voice = vi;
          }
        sl.cfg.dir = (jClampInt(v.find("dir"), 0, 1, (int)sl.cfg.dir) == 1)
            ? specfx::GestureDir::Down
            : specfx::GestureDir::Up;
        sl.cfg.motion = (jClampInt(v.find("motion"), 0, 1, (int)sl.cfg.motion) == 1)
            ? specfx::GestureMotion::FromDepth
            : specfx::GestureMotion::ToDepth;
        sl.cfg.depthSt = jClampFloat(v.find("depth"), 0.0f, specfx::kMaxGestureDepthSt, sl.cfg.depthSt);
        sl.cfg.timeMs = jClampFloat(v.find("time"), 0.0f, specfx::kMaxGestureMs, sl.cfg.timeMs);
        sl.cfg.retMs = jClampFloat(v.find("ret"), 0.0f, specfx::kMaxGestureMs, sl.cfg.retMs);
      }
  out = s;
  return true;
}

bool saveFile(const std::string& path, const PersistedState& s, std::string& error)
{
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos)
  {
    // Best-effort parent creation (~/Library/... always exists; harmless).
    const std::string dir = path.substr(0, slash);
    ::mkdir(dir.c_str(), 0755);
  }
  const std::string text = saveJson(s);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr)
  {
    error = "cannot open file for writing";
    return false;
  }
  const size_t n = std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  if (n != text.size())
  {
    error = "short write";
    return false;
  }
  return true;
}

bool loadFile(const std::string& path, PersistedState& out, std::string& error)
{
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr)
  {
    out = defaultState(); // missing file: factory fallback, not an error
    error.clear();
    return true;
  }
  std::string text;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
  {
    text.append(buf, n);
    if (text.size() > 1048576)
      break;
  }
  std::fclose(f);
  if (text.size() > 1048576)
  {
    out = defaultState();
    error = "file too large";
    return false;
  }
  if (!loadJson(text, out, error))
  {
    out = defaultState();
    return false;
  }
  return true;
}

std::string defaultPath()
{
  const char* home = std::getenv("HOME");
  const std::string h = (home != nullptr) ? home : "/tmp";
  return h + "/Library/Application Support/TechDeathMachine/midi.json";
}

} // namespace midi
} // namespace tdm
