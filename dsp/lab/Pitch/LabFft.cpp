// LabFft: iterative decimation-in-time radix-2 FFT. See LabFft.h.

#include "dsp/lab/Pitch/LabFft.h"

#include <cmath>
#include <stdexcept>

namespace tdm
{
namespace lab
{
void LabFft::init(int size)
{
  if (size < 64 || (size & (size - 1)) != 0)
    throw std::invalid_argument("LabFft: size must be a power of two >= 64");
  size_ = size;
  log2_ = 0;
  for (int n = size; n > 1; n >>= 1)
    ++log2_;
  rev_.assign(static_cast<size_t>(size), 0);
  for (int i = 0; i < size; ++i)
  {
    int r = 0;
    for (int b = 0; b < log2_; ++b)
      if (i & (1 << b))
        r |= 1 << (log2_ - 1 - b);
    rev_[static_cast<size_t>(i)] = r;
  }
  tw_.assign(static_cast<size_t>(size / 2), std::complex<float>(0.0f, 0.0f));
  const double step = -2.0 * 3.14159265358979 / static_cast<double>(size);
  for (int k = 0; k < size / 2; ++k)
    tw_[static_cast<size_t>(k)] = std::complex<float>(static_cast<float>(std::cos(step * k)),
                                                      static_cast<float>(std::sin(step * k)));
}

void LabFft::forward(std::complex<float>* io) const
{
  const int n = size_;
  for (int i = 0; i < n; ++i)
  {
    const int r = rev_[static_cast<size_t>(i)];
    if (r > i)
    {
      const std::complex<float> t = io[i];
      io[i] = io[r];
      io[r] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    const int half = len >> 1;
    const int twStep = n / len;
    for (int base = 0; base < n; base += len)
    {
      for (int j = 0; j < half; ++j)
      {
        const std::complex<float> w = tw_[static_cast<size_t>(j * twStep)];
        const std::complex<float> u = io[base + j];
        const std::complex<float> v = io[base + j + half] * w;
        io[base + j] = u + v;
        io[base + j + half] = u - v;
      }
    }
  }
}

void LabFft::inverse(std::complex<float>* io) const
{
  const int n = size_;
  for (int i = 0; i < n; ++i)
  {
    const int r = rev_[static_cast<size_t>(i)];
    if (r > i)
    {
      const std::complex<float> t = io[i];
      io[i] = io[r];
      io[r] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    const int half = len >> 1;
    const int twStep = n / len;
    for (int base = 0; base < n; base += len)
    {
      for (int j = 0; j < half; ++j)
      {
        const std::complex<float> w = std::conj(tw_[static_cast<size_t>(j * twStep)]);
        const std::complex<float> u = io[base + j];
        const std::complex<float> v = io[base + j + half] * w;
        io[base + j] = u + v;
        io[base + j + half] = u - v;
      }
    }
  }
  const float inv = 1.0f / static_cast<float>(n);
  for (int i = 0; i < n; ++i)
    io[i] *= inv;
}
} // namespace lab
} // namespace tdm
