#pragma once

// Dependency-free test asserts: failures are collected, never thrown,
// so one bad check cannot abort the rest of the suite.

#include <cmath>
#include <string>
#include <vector>

namespace tdm_test
{
inline std::vector<std::string>& failures()
{
  static std::vector<std::string> f;
  return f;
}

inline int& checkCount()
{
  static int n = 0;
  return n;
}

inline void checkImpl(bool cond, const char* file, int line, const std::string& msg)
{
  ++checkCount();
  if (!cond)
    failures().push_back(std::string(file) + ":" + std::to_string(line) + " " + msg);
}

inline bool allFinite(const float* p, int n)
{
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(p[i]))
      return false;
  return true;
}

inline float peakAbs(const float* p, int n)
{
  float m = 0.0f;
  for (int i = 0; i < n; ++i)
  {
    const float a = std::fabs(p[i]);
    if (a > m)
      m = a;
  }
  return m;
}
} // namespace tdm_test

#define TDM_CHECK(cond, msg) tdm_test::checkImpl((cond), __FILE__, __LINE__, (msg))
#define TDM_CHECK_CLOSE(a, b, tol, msg)                                                                            \
  tdm_test::checkImpl(std::fabs((a) - (b)) <= (tol), __FILE__, __LINE__,                                            \
                      std::string(msg) + " got=" + std::to_string(a) + " want=" + std::to_string(b))
