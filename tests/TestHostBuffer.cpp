#include "tests/Assert.h"

#include <cstdio>
#include <string>

#include "host/BufferRequest.h"

void runHostBufferTests()
{
  using tdm_host::BufferAsk;
  using tdm_host::bufferReportLine;
  using tdm_host::checkBufferRequest;
  { // Omitted flag (0) is always a no-op, regardless of range.
    const BufferAsk a = checkBufferRequest(0, 32, 4096);
    TDM_CHECK(!a.apply && a.ok, "hostbuffer omitted no-op");
    const BufferAsk b = checkBufferRequest(0, 0, 0);
    TDM_CHECK(!b.apply && b.ok, "hostbuffer omitted no-op degenerate range");
  }
  { // In-range requests apply, boundaries inclusive.
    for (unsigned r : {32u, 33u, 128u, 4095u, 4096u})
    {
      const BufferAsk a = checkBufferRequest(r, 32, 4096);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "hostbuffer apply in-range %u", r);
      TDM_CHECK(a.apply && a.ok, msg);
    }
  }
  { // Out-of-range requests reject before touching the device.
    const BufferAsk lo = checkBufferRequest(31, 32, 4096);
    TDM_CHECK(!lo.apply && !lo.ok, "hostbuffer reject below minimum");
    const BufferAsk hi = checkBufferRequest(4097, 32, 4096);
    TDM_CHECK(!hi.apply && !hi.ok, "hostbuffer reject above maximum");
    const BufferAsk inv = checkBufferRequest(128, 512, 32);
    TDM_CHECK(!inv.apply && !inv.ok, "hostbuffer reject invalid range");
  }
  { // Scarlett-class range sanity: 128 accepted on a 32..2048 device.
    const BufferAsk a = checkBufferRequest(128, 32, 2048);
    TDM_CHECK(a.apply && a.ok, "hostbuffer scarlett 128 applies");
  }
  { // Report lines always carry requested AND actual (never the request
    // alone), with the mismatch note only when present.
    const std::string exact = bufferReportLine("input", 128, 128, "");
    TDM_CHECK(exact.find("requested=128") != std::string::npos
                  && exact.find("actual=128") != std::string::npos,
              "hostbuffer report exact");
    const std::string snap = bufferReportLine("output", 128, 256, "device snapped to 256");
    TDM_CHECK(snap.find("requested=128") != std::string::npos
                  && snap.find("actual=256") != std::string::npos
                  && snap.find("snapped") != std::string::npos,
              "hostbuffer report mismatch");
    const std::string dflt = bufferReportLine("input", 0, 512, "");
    TDM_CHECK(dflt.find("requested=default") != std::string::npos
                  && dflt.find("actual=512") != std::string::npos,
              "hostbuffer report omitted");
  }
}
