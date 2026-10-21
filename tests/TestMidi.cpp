// Generic MIDI control tests: packet parsing, mapping/learn, action
// dispatch (fake sink + REAL DevSpecFx gesture integration), JSON
// persistence, and headless CoreMIDI transport. Synthetic traffic
// only — no hardware required. Simulated controllers: FCB1010-style
// (PC switches + CC pedals), generic USB (other CCs/channel),
// momentary note/CC controller, and multi-source filtering.

#include "tests/Assert.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/lab/SpecialFx/DevSpecFx.h"
#include "host/midi/MidiDispatch.h"
#include "host/midi/MidiInput.h"
#include "host/midi/MidiMapping.h"
#include "host/midi/MidiPersist.h"

namespace
{
using tdm::midi::Action;
using tdm::midi::Dispatcher;
using tdm::midi::LearnManager;
using tdm::midi::Mapping;
using tdm::midi::MappingTable;
using tdm::midi::Message;
using tdm::midi::MsgType;
using tdm::midi::StreamParser;
using tdm::midi::Target;
using tdm::midi::Trigger;

Message mkMsg(MsgType t, uint8_t ch, uint8_t num, uint8_t val, uint32_t uid = 0)
{
  Message m;
  m.sourceUid = uid;
  m.channel = ch;
  m.type = t;
  m.number = num;
  m.value = val;
  return m;
}

Trigger mkTrig(MsgType t, uint8_t num, int ch = -1, uint32_t uid = 0)
{
  Trigger tr;
  tr.sourceUid = uid;
  tr.channel = (int8_t)ch;
  tr.type = t;
  tr.number = num;
  return tr;
}

struct FakeSink : tdm::midi::ISink
{
  float cont[12];
  bool bools[12];
  int presses[8];
  int releases[8];
  int oneShots[8];
  FakeSink()
  {
    for (int i = 0; i < 12; ++i)
    {
      cont[i] = -1.0f;
      bools[i] = false;
    }
    for (int i = 0; i < 8; ++i)
    {
      presses[i] = releases[i] = oneShots[i] = 0;
    }
  }
  void setContinuous(Target t, float v) override { cont[(int)t] = v; }
  void setBool(Target t, bool v) override { bools[(int)t] = v; }
  void gesturePress(int s) override { ++presses[s]; }
  void gestureRelease(int s) override { ++releases[s]; }
  void gestureOneShot(int s) override { ++oneShots[s]; }
};

int feedAll(StreamParser& p, const std::vector<uint8_t>& bytes, uint32_t uid, Message* out, int cap)
{
  return p.feed(bytes.data(), (int)bytes.size(), uid, out, cap);
}

} // namespace

void runMidiTests()
{
  // 1. CC parsing across channels + values.
  {
    StreamParser p;
    Message out[8];
    const int n = feedAll(p, {0xB0, 7, 84, 0xB4, 14, 127}, 0, out, 8);
    TDM_CHECK(n == 2, "midi cc count");
    TDM_CHECK(out[0].type == MsgType::Cc && out[0].channel == 0 && out[0].number == 7
                  && out[0].value == 84,
              "midi cc ch0");
    TDM_CHECK(out[1].type == MsgType::Cc && out[1].channel == 4 && out[1].number == 14
                  && out[1].value == 127,
              "midi cc ch4");
  }
  // 2. Program Change (1 data byte) + running status across feeds.
  {
    StreamParser p;
    Message out[8];
    int n = feedAll(p, {0xC0, 3}, 0, out, 8);
    TDM_CHECK(n == 1 && out[0].type == MsgType::Program && out[0].number == 3
                  && out[0].value == 0,
              "midi pc");
    n = feedAll(p, {0x90, 60, 100, 62, 90}, 0, out, 8); // running status
    TDM_CHECK(n == 2 && out[0].number == 60 && out[1].number == 62
                  && out[1].type == MsgType::NoteOn,
              "midi running status");
    StreamParser q;
    n = feedAll(q, {0xB0, 7}, 0, out, 8); // split message, first half
    TDM_CHECK(n == 0, "midi split waits");
    n = feedAll(q, {84}, 0, out, 8); // second half completes via running status
    TDM_CHECK(n == 1 && out[0].type == MsgType::Cc && out[0].value == 84, "midi split completes");
  }
  // 3. Note On/Off incl. velocity-zero convention.
  {
    StreamParser p;
    Message out[8];
    const int n = feedAll(p, {0x91, 64, 100, 0x91, 64, 0, 0x81, 65, 40}, 0, out, 8);
    TDM_CHECK(n == 3, "midi note count");
    TDM_CHECK(out[0].type == MsgType::NoteOn && out[0].channel == 1, "midi note on");
    TDM_CHECK(out[1].type == MsgType::NoteOff && out[1].number == 64, "midi vel0 is off");
    TDM_CHECK(out[2].type == MsgType::NoteOff && out[2].number == 65, "midi note off");
  }
  // 4. Malformed traffic: stray data, sysex, realtime, unknown voices.
  {
    StreamParser p;
    Message out[8];
    int n = feedAll(p, {0x45, 0x7F}, 0, out, 8); // stray data, no status
    TDM_CHECK(n == 0, "midi stray ignored");
    n = feedAll(p, {0xF0, 0x41, 0x10, 0xF7, 0xF8, 0xFA}, 0, out, 8); // sysex + realtime
    TDM_CHECK(n == 0, "midi sysex/realtime ignored");
    n = feedAll(p, {0xE0, 0x00, 0x40}, 0, out, 8); // pitch bend: Other
    TDM_CHECK(n == 1 && out[0].type == MsgType::Other, "midi bend is Other");
    n = feedAll(p, {0xB0, 7, 100}, 0, out, 8); // parser still healthy after
    TDM_CHECK(n == 1 && out[0].type == MsgType::Cc && out[0].value == 100, "midi recovers");
    StreamParser q;
    n = feedAll(q, {0xF0, 0x41, 0xB0, 7, 100}, 0, out, 8); // status aborts sysex
    TDM_CHECK(n == 1 && out[0].type == MsgType::Cc, "midi sysex abort");
  }
  // 5. Trigger matching: exact vs wildcard source/channel.
  {
    const Message m = mkMsg(MsgType::Cc, 2, 7, 100, 4242);
    TDM_CHECK(mkTrig(MsgType::Cc, 7).matches(m), "midi trigger omni");
    TDM_CHECK(mkTrig(MsgType::Cc, 7, 2, 4242).matches(m), "midi trigger exact");
    TDM_CHECK(!mkTrig(MsgType::Cc, 7, 3, 4242).matches(m), "midi trigger ch filter");
    TDM_CHECK(!mkTrig(MsgType::Cc, 7, 2, 999).matches(m), "midi trigger src filter");
    TDM_CHECK(!mkTrig(MsgType::Cc, 8).matches(m), "midi trigger num filter");
    TDM_CHECK(!mkTrig(MsgType::NoteOn, 7).matches(m), "midi trigger type filter");
    TDM_CHECK(!mkTrig(MsgType::Cc, 7).matches(mkMsg(MsgType::Other, 2, 7, 100, 4242)),
              "midi trigger never Other");
  }
  // 6. Table: add/replace/clear, trigger-unique, compatibility gate.
  {
    MappingTable t;
    Mapping a;
    a.trigger = mkTrig(MsgType::Cc, 7);
    a.target = Target::SpecExpression;
    a.action = Action::Continuous;
    TDM_CHECK(t.addOrReplace(a), "midi table add");
    Mapping b = a;
    b.target = Target::SpecMix; // same trigger, new owner: replaces
    TDM_CHECK(t.addOrReplace(b), "midi table replace");
    Mapping f;
    TDM_CHECK(t.findTarget(Target::SpecMix, f) && !t.findTarget(Target::SpecExpression, f),
              "midi table replaced");
    Mapping bad = a; // Continuous on a slot: incompatible
    bad.target = Target::GestSlot0;
    bad.action = Action::Continuous;
    TDM_CHECK(!t.addOrReplace(bad), "midi table compat gate");
    TDM_CHECK(t.removeTrigger(mkTrig(MsgType::Cc, 7)), "midi table remove");
    TDM_CHECK(!t.removeTrigger(mkTrig(MsgType::Cc, 7)), "midi table remove miss");
    TDM_CHECK(t.addOrReplace(a), "midi table re-add");
    t.clear();
    TDM_CHECK(!t.findTarget(Target::SpecExpression, f), "midi table clear");
  }
  // 7. Learn: arm -> capture -> commit; cancel; OMNI; NoteOff normalizes.
  {
    LearnManager l;
    TDM_CHECK(l.armed() == Target::None, "midi learn idle");
    l.arm(Target::SpecExpression, false);
    TDM_CHECK(!l.offer(mkMsg(MsgType::Other, 0, 0, 0)), "midi learn ignores Other");
    TDM_CHECK(l.offer(mkMsg(MsgType::Cc, 1, 7, 90, 555)), "midi learn consumes");
    Trigger cap;
    TDM_CHECK(l.takeCapture(cap), "midi learn capture");
    TDM_CHECK(cap.sourceUid == 555 && cap.channel == 1 && cap.type == MsgType::Cc && cap.number == 7,
              "midi learn exact");
    TDM_CHECK(l.armed() == Target::None, "midi learn disarms");
    TDM_CHECK(!l.takeCapture(cap), "midi learn single take");
    l.arm(Target::GestSlot2, true); // OMNI
    l.offer(mkMsg(MsgType::NoteOff, 4, 60, 0, 777));
    TDM_CHECK(l.takeCapture(cap) && cap.sourceUid == 0 && cap.channel == -1
                  && cap.type == MsgType::NoteOn && cap.number == 60,
              "midi learn omni+normalize");
    l.arm(Target::SpecMix, false);
    l.cancel();
    TDM_CHECK(!l.offer(mkMsg(MsgType::Cc, 0, 1, 1)) && l.armed() == Target::None,
              "midi learn cancel");
  }
  // 8. Continuous dispatch: full range, invert, calibration window.
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping m;
    m.trigger = mkTrig(MsgType::Cc, 7);
    m.target = Target::SpecExpression;
    m.action = Action::Continuous;
    t.addOrReplace(m);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 7, 0));
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 0.0f, "midi cont min");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 7, 127));
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 1.0f, "midi cont max");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 7, 64));
    TDM_CHECK(std::fabs(s.cont[(int)Target::SpecExpression] - 64.0f / 127.0f) < 1e-6f,
              "midi cont mid");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 8, 127)); // unmapped CC: silent
    TDM_CHECK(std::fabs(s.cont[(int)Target::SpecExpression] - 64.0f / 127.0f) < 1e-6f,
              "midi cont unmapped silent");
    Mapping inv = m;
    inv.trigger = mkTrig(MsgType::Cc, 8);
    inv.target = Target::SpecMix;
    inv.invert = true;
    t.addOrReplace(inv);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 8, 0));
    TDM_CHECK(s.cont[(int)Target::SpecMix] == 1.0f, "midi cont invert");
    Mapping cal = m; // pedal only sweeps 10..110
    cal.trigger = mkTrig(MsgType::Cc, 9);
    cal.calMin = 10;
    cal.calMax = 110;
    t.addOrReplace(cal);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 9, 10));
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 0.0f, "midi cont cal lo");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 9, 110));
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 1.0f, "midi cont cal hi");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 9, 127)); // above window clamps
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 1.0f, "midi cont cal clamp");
  }
  // 9. Momentary slots: note pair, CC threshold pair, no stuck state.
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping mn;
    mn.trigger = mkTrig(MsgType::NoteOn, 60);
    mn.target = Target::GestSlot3;
    mn.action = Action::Momentary;
    t.addOrReplace(mn); // single key mapping: NoteOff aliases to it
    TDM_CHECK(mkTrig(MsgType::NoteOn, 60).matches(mkMsg(MsgType::NoteOff, 0, 60, 0)),
              "midi key alias matches");
    TDM_CHECK(!mkTrig(MsgType::NoteOn, 61).matches(mkMsg(MsgType::NoteOff, 0, 60, 0)),
              "midi key alias needs key");
    d.handleMessage(mkMsg(MsgType::NoteOn, 0, 60, 100));
    d.handleMessage(mkMsg(MsgType::NoteOff, 0, 60, 0));
    TDM_CHECK(s.presses[3] == 1 && s.releases[3] == 1, "midi momentary note");
    Mapping mc = mn;
    mc.trigger = mkTrig(MsgType::Cc, 20);
    mc.target = Target::GestSlot4;
    t.addOrReplace(mc);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 20, 127));
    d.handleMessage(mkMsg(MsgType::Cc, 0, 20, 100)); // repeat press: idempotent
    d.handleMessage(mkMsg(MsgType::Cc, 0, 20, 0));
    TDM_CHECK(s.presses[4] == 2 && s.releases[4] == 1, "midi momentary cc");
    // Rapid retrigger: pairs stay balanced, newest wins downstream.
    for (int i = 0; i < 50; ++i)
    {
      d.handleMessage(mkMsg(MsgType::NoteOn, 0, 60, 100));
      d.handleMessage(mkMsg(MsgType::NoteOff, 0, 60, 0));
    }
    TDM_CHECK(s.presses[3] == 51 && s.releases[3] == 51, "midi momentary balanced");
  }
  // 10. One-shot slots: PC, note, CC rising edge (no repeat spam).
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping pc;
    pc.trigger = mkTrig(MsgType::Program, 3);
    pc.target = Target::GestSlot0;
    pc.action = Action::OneShot;
    t.addOrReplace(pc);
    d.handleMessage(mkMsg(MsgType::Program, 0, 3, 0));
    d.handleMessage(mkMsg(MsgType::Program, 0, 3, 0));
    TDM_CHECK(s.oneShots[0] == 2 && s.presses[0] == 0 && s.releases[0] == 0,
              "midi oneshot pc (no fabricated release)");
    Mapping cc;
    cc.trigger = mkTrig(MsgType::Cc, 21);
    cc.target = Target::GestSlot1;
    cc.action = Action::OneShot;
    t.addOrReplace(cc);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 21, 127));
    d.handleMessage(mkMsg(MsgType::Cc, 0, 21, 120)); // held: no refire
    d.handleMessage(mkMsg(MsgType::Cc, 0, 21, 0));   // re-arm
    d.handleMessage(mkMsg(MsgType::Cc, 0, 21, 127)); // refire
    TDM_CHECK(s.oneShots[1] == 2, "midi oneshot cc edge");
  }
  // 11. Toggle bools: PC + note + CC edge; momentary bools follow level.
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping tg;
    tg.trigger = mkTrig(MsgType::Program, 6);
    tg.target = Target::SpecGestEnable;
    tg.action = Action::Toggle;
    t.addOrReplace(tg);
    d.handleMessage(mkMsg(MsgType::Program, 0, 6, 0));
    TDM_CHECK(s.bools[(int)Target::SpecGestEnable], "midi toggle on");
    d.handleMessage(mkMsg(MsgType::Program, 0, 6, 0));
    TDM_CHECK(!s.bools[(int)Target::SpecGestEnable], "midi toggle off");
    Mapping mm;
    mm.trigger = mkTrig(MsgType::Cc, 22);
    mm.target = Target::SpecExprEnable;
    mm.action = Action::Momentary;
    t.addOrReplace(mm);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 22, 127));
    TDM_CHECK(s.bools[(int)Target::SpecExprEnable], "midi bool momentary on");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 22, 0));
    TDM_CHECK(!s.bools[(int)Target::SpecExprEnable], "midi bool momentary off");
  }
  // 12. Multi-source + channel filtering: same CC, different origins.
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping a; // Scarlett pedal (uid 100, ch 0)
    a.trigger = mkTrig(MsgType::Cc, 7, 0, 100);
    a.target = Target::SpecExpression;
    a.action = Action::Continuous;
    Mapping b; // USB controller (uid 200, ch 5)
    b.trigger = mkTrig(MsgType::Cc, 7, 5, 200);
    b.target = Target::SpecMix;
    b.action = Action::Continuous;
    t.addOrReplace(a);
    t.addOrReplace(b);
    d.handleMessage(mkMsg(MsgType::Cc, 0, 7, 127, 100));
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 1.0f
                  && s.cont[(int)Target::SpecMix] == -1.0f,
              "midi source A routes");
    d.handleMessage(mkMsg(MsgType::Cc, 5, 7, 64, 200));
    TDM_CHECK(std::fabs(s.cont[(int)Target::SpecMix] - 64.0f / 127.0f) < 1e-6f
                  && s.cont[(int)Target::SpecExpression] == 1.0f,
              "midi source B routes");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 7, 0, 200)); // uid 200 but ch 0: silent
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == 1.0f
                  && std::fabs(s.cont[(int)Target::SpecMix] - 64.0f / 127.0f) < 1e-6f,
              "midi ch filter silent");
  }
  // 13. Gesture integration: dispatcher drives the REAL DevSpecFx by
  // slot ID (press/release/one-shot), gestures fire with Expression
  // OFF (accepted requirement), release returns to idle.
  {
    tdm::specfx::DevSpecFx ds;
    ds.preTap().reset(48000.0, 256);
    ds.postTap().reset(48000.0, 256);
    ds.setExpressionEnabled(false); // accepted: gestures work dry-based
    ds.setGesturesEnabled(true);
    struct SpecSink : tdm::midi::ISink
    {
      tdm::specfx::DevSpecFx* d = nullptr;
      void setContinuous(Target, float) override {}
      void setBool(Target t, bool v) override
      {
        if (t == Target::SpecExprEnable)
          d->setExpressionEnabled(v);
        else if (t == Target::SpecGestEnable)
          d->setGesturesEnabled(v);
      }
      void gesturePress(int s) override { d->pressSlot(s); }
      void gestureRelease(int s) override { d->releaseSlot(s); }
      void gestureOneShot(int s) override { d->oneShotSlot(s); }
    } sink;
    sink.d = &ds;
    MappingTable t;
    Dispatcher d(t, sink);
    for (int slot = 0; slot < 8; ++slot) // all eight slots by ID (PC 10..17)
    {
      Mapping m;
      m.trigger = mkTrig(MsgType::Program, (uint8_t)(10 + slot));
      m.target = (Target)((int)Target::GestSlot0 + slot);
      m.action = Action::OneShot;
      t.addOrReplace(m);
    }
    std::vector<float> io(256, 0.1f);
    for (int slot = 0; slot < 8; ++slot)
    {
      d.handleMessage(mkMsg(MsgType::Program, 0, (uint8_t)(10 + slot), 0));
      ds.preTap().process(io.data(), io.data(), 256);
      TDM_CHECK(ds.preGestureActive(), "midi slot fires by ID");
      for (int k = 0; k < 220; ++k) // slowest factory slot: 600+300 ms + margin
        ds.preTap().process(io.data(), io.data(), 256);
      TDM_CHECK(!ds.preGestureActive(), "midi one-shot completes");
    }
    // Momentary pair through the router: press activates, release ends.
    MappingTable t2;
    Dispatcher d2(t2, sink);
    Mapping mn;
    mn.trigger = mkTrig(MsgType::NoteOn, 60);
    mn.target = Target::GestSlot1;
    mn.action = Action::Momentary;
    t2.addOrReplace(mn); // key aliasing delivers the release
    d2.handleMessage(mkMsg(MsgType::NoteOn, 0, 60, 100));
    ds.preTap().process(io.data(), io.data(), 256);
    TDM_CHECK(ds.preGestureActive(), "midi router press");
    d2.handleMessage(mkMsg(MsgType::NoteOff, 0, 60, 0));
    for (int k = 0; k < 60; ++k)
      ds.preTap().process(io.data(), io.data(), 256);
    TDM_CHECK(!ds.preGestureActive(), "midi router release");
  }
  // 14. Persistence round-trip: mappings + slots + sources survive.
  {
    tdm::midi::PersistedState s = tdm::midi::defaultState();
    Mapping m;
    m.trigger = mkTrig(MsgType::Cc, 7, 0, 4242);
    m.target = Target::SpecExpression;
    m.action = Action::Continuous;
    m.invert = true;
    m.calMin = 5;
    m.calMax = 120;
    s.mappings.push_back(m);
    Mapping g;
    g.trigger = mkTrig(MsgType::Program, 3);
    g.target = Target::GestSlot2;
    g.action = Action::OneShot;
    s.mappings.push_back(g);
    s.midiSources.push_back(4242);
    s.slots[2].name = "Custom \"Dive\"\nX";
    s.slots[2].cfg.depthSt = 6.0f;
    s.slots[2].cfg.timeMs = 111.0f;
    const std::string json = tdm::midi::saveJson(s);
    tdm::midi::PersistedState back;
    std::string err;
    TDM_CHECK(tdm::midi::loadJson(json, back, err), "midi persist round-trip");
    TDM_CHECK(back.mappings.size() == 2, "midi persist map count");
    TDM_CHECK(back.mappings[0].trigger == m.trigger && back.mappings[0].invert
                  && back.mappings[0].calMin == 5 && back.mappings[0].calMax == 120,
              "midi persist cont map");
    TDM_CHECK(back.mappings[1].target == Target::GestSlot2
                  && back.mappings[1].action == Action::OneShot,
              "midi persist slot map");
    TDM_CHECK(back.midiSources.size() == 1 && back.midiSources[0] == 4242, "midi persist sources");
    TDM_CHECK(back.slots[2].name == "Custom \"Dive\"\nX" && back.slots[2].cfg.depthSt == 6.0f
                  && back.slots[2].cfg.timeMs == 111.0f,
              "midi persist slot edit");
    TDM_CHECK(back.slots[0].name == std::string(tdm::specfx::defaultGestureName(0)),
              "midi persist slot default kept");
  }
  // 15. Persistence fallback: corrupt JSON, wrong version, bad values.
  {
    tdm::midi::PersistedState out;
    std::string err;
    TDM_CHECK(!tdm::midi::loadJson("{oops", out, err), "midi persist corrupt fails");
    TDM_CHECK(!tdm::midi::loadJson("{\"version\": 99}", out, err), "midi persist version fails");
    TDM_CHECK(!tdm::midi::loadJson("[1,2]", out, err), "midi persist root fails");
    // Bad entries skipped, good entries kept, numbers clamped.
    const char* mixed = "{ \"version\": 1, \"mappings\": ["
                        " {\"src\": 0, \"ch\": 0, \"type\": \"wat\", \"num\": 1,"
                        "  \"target\": \"specExpr\", \"action\": \"continuous\"},"
                        " {\"src\": 0, \"ch\": 99, \"type\": \"cc\", \"num\": 300,"
                        "  \"target\": \"specMix\", \"action\": \"continuous\","
                        "  \"min\": 200, \"max\": -5},"
                        " {\"src\": 0, \"ch\": 0, \"type\": \"program\", \"num\": 1,"
                        "  \"target\": \"slot0\", \"action\": \"momentary\"}"
                        "], \"gestureSlots\": ["
                        " {\"name\": \"X\", \"voice\": 9, \"depth\": 999.0,"
                        "  \"time\": -5.0, \"ret\": 50.0}"
                        "] }";
    TDM_CHECK(tdm::midi::loadJson(mixed, out, err), "midi persist mixed loads");
    TDM_CHECK(out.mappings.size() == 1, "midi persist bad skipped");
    TDM_CHECK(out.mappings[0].trigger.number == 127 && out.mappings[0].trigger.channel == 15
                  && out.mappings[0].calMin == 0 && out.mappings[0].calMax == 127,
              "midi persist clamped");
    TDM_CHECK(out.slots[0].cfg.voice == tdm::specfx::defaultGestureConfig(0).voice
                  && out.slots[0].cfg.depthSt == tdm::specfx::kMaxGestureDepthSt
                  && out.slots[0].cfg.timeMs == 0.0f && out.slots[0].cfg.retMs == 50.0f,
              "midi persist slot clamped");
    // Missing file -> defaults, success.
    TDM_CHECK(tdm::midi::loadFile("/tmp/tdm-midi-nope-12345.json", out, err)
                  && out.mappings.empty(),
              "midi persist missing file");
  }
  // 16. File round-trip through /tmp (save + load).
  {
    tdm::midi::PersistedState s = tdm::midi::defaultState();
    s.omniLearn = true;
    s.midiSources.push_back(777);
    Mapping m;
    m.trigger = mkTrig(MsgType::Cc, 11, 2, 777);
    m.target = Target::SpecMix;
    m.action = Action::Continuous;
    s.mappings.push_back(m);
    const std::string path = "/tmp/tdm-midi-test.json";
    std::string err;
    TDM_CHECK(tdm::midi::saveFile(path, s, err), "midi persist save");
    tdm::midi::PersistedState back;
    TDM_CHECK(tdm::midi::loadFile(path, back, err), "midi persist load");
    TDM_CHECK(back.omniLearn && back.midiSources.size() == 1
                  && back.mappings.size() == 1
                  && back.mappings[0].trigger == m.trigger,
              "midi persist file round-trip");
    std::remove(path.c_str());
  }
  // 17. CoreMIDI transport, both environments: with a MIDI server
  // (normal Mac) it enumerates >= 0 sources; without one (sandboxed
  // CI/shell: kMIDINoConnection) every call degrades gracefully —
  // no crash, no hang, no partial state.
  {
    tdm::midi::MidiInput in;
    if (in.available())
    {
      auto srcs = in.refreshSources(); // >= 0 sources; hardware not required
      TDM_CHECK(!in.needsRefresh(), "midi transport refresh clears");
      TDM_CHECK(!in.setConnected(0xDEADBEEFu, true), "midi transport unknown uid");
      TDM_CHECK(in.setConnected(0xDEADBEEFu, false), "midi transport disconnect idempotent");
      auto lat = in.latest();
      TDM_CHECK(lat.count == 0 && lat.type == MsgType::Other, "midi transport latest idle");
      (void)srcs;
    }
    else
    {
      auto srcs = in.refreshSources();
      TDM_CHECK(srcs.empty(), "midi transport no-server empty");
      TDM_CHECK(!in.setConnected(1, true), "midi transport no-server connect fails");
      TDM_CHECK(in.setConnected(1, false), "midi transport no-server disconnect ok");
      auto lat = in.latest();
      TDM_CHECK(lat.count == 0, "midi transport no-server latest idle");
    }
  }
  // 18. Burst traffic: a 2000-iteration mixed CC/PC/note storm keeps
  // discrete events exactly ordered (press/release balance, one-shot
  // count, toggle parity), a fast full-range sweep always lands on
  // the latest pedal position, and a full 32-mapping table still
  // routes (dummies stay silent until addressed).
  {
    MappingTable t;
    FakeSink s;
    Dispatcher d(t, s);
    Mapping sw;
    sw.trigger = mkTrig(MsgType::Cc, 7);
    sw.target = Target::SpecExpression;
    sw.action = Action::Continuous;
    t.addOrReplace(sw);
    Mapping os;
    os.trigger = mkTrig(MsgType::Program, 10);
    os.target = Target::GestSlot0;
    os.action = Action::OneShot;
    t.addOrReplace(os);
    Mapping mom;
    mom.trigger = mkTrig(MsgType::NoteOn, 60);
    mom.target = Target::GestSlot1;
    mom.action = Action::Momentary;
    t.addOrReplace(mom);
    Mapping tg;
    tg.trigger = mkTrig(MsgType::Program, 6);
    tg.target = Target::SpecGestEnable;
    tg.action = Action::Toggle;
    t.addOrReplace(tg);
    for (int i = 0; i < 28; ++i) // fill to the 32-mapping cap
    {
      Mapping dum;
      dum.trigger = mkTrig(MsgType::Cc, (uint8_t)(30 + i));
      dum.target = Target::GestSlot7;
      dum.action = Action::OneShot;
      t.addOrReplace(dum);
    }
    for (int i = 0; i < 2000; ++i)
    {
      // Same-packet batch: pedal move + footswitch fire together.
      d.handleMessage(mkMsg(MsgType::Cc, 0, 7, (uint8_t)(i & 127)));
      d.handleMessage(mkMsg(MsgType::Program, 0, 10, 0));
      d.handleMessage(mkMsg(MsgType::NoteOn, 0, 60, 100));
      d.handleMessage(mkMsg(MsgType::NoteOff, 0, 60, 0));
      d.handleMessage(mkMsg(MsgType::Program, 0, 6, 0));
    }
    TDM_CHECK(s.presses[1] == 2000 && s.releases[1] == 2000, "midi burst momentary balanced");
    TDM_CHECK(s.oneShots[0] == 2000 && s.oneShots[7] == 0, "midi burst oneshot exact, dummies silent");
    TDM_CHECK(!s.bools[(int)Target::SpecGestEnable], "midi burst toggle parity");
    TDM_CHECK(s.cont[(int)Target::SpecExpression] == (float)(1999 & 127) / 127.0f,
              "midi burst sweep latest wins");
    d.handleMessage(mkMsg(MsgType::Cc, 0, 57, 127)); // last table entry fires
    TDM_CHECK(s.oneShots[7] == 1, "midi full table routes");
  }
}
