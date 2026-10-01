#pragma once

#include <cstdio>
#include <string>

// BufferRequest: pure (hardware-free) decision logic for the live-host
// --buffer flag. CoreAudio interaction itself cannot run without a device,
// so the testable contract lives here: given a requested size and the
// device-reported [minimum, maximum] range, decide apply vs reject, and
// phrase the read-back report. The engine owns all HAL calls and always
// reports device-read ACTUALS, never the request.
//
// Semantics:
// - requested == 0 means "flag omitted": leave the device alone (no-op).
// - in-range requests (inclusive) are applied, then verified by read-back.
// - out-of-range requests are rejected before touching the device.
// - a read-back that differs from the request is reported, never claimed.

namespace tdm_host
{
struct BufferAsk
{
  bool apply; // set the property when true
  bool ok; // false => reject before touching the device
  const char* note; // short reason for logs/tests
};

inline BufferAsk checkBufferRequest(unsigned requested, unsigned minimum, unsigned maximum)
{
  if (requested == 0)
    return {false, true, "omitted"};
  if (minimum > maximum)
    return {false, false, "invalid device range"};
  if (requested < minimum)
    return {false, false, "below device minimum"};
  if (requested > maximum)
    return {false, false, "above device maximum"};
  return {true, true, "in range"};
}

// Read-back report line: names the device role, what was asked, and what
// the device actually runs. Empty note keeps the line short.
inline std::string bufferReportLine(const char* role, unsigned requested, unsigned actual,
                                    const std::string& note)
{
  char buf[192];
  if (requested == 0)
    std::snprintf(buf, sizeof(buf), "%s buffer: requested=default actual=%u", role, actual);
  else if (note.empty())
    std::snprintf(buf, sizeof(buf), "%s buffer: requested=%u actual=%u", role, requested, actual);
  else
    std::snprintf(buf, sizeof(buf), "%s buffer: requested=%u actual=%u (%s)", role, requested, actual,
                  note.c_str());
  return buf;
}
} // namespace tdm_host
