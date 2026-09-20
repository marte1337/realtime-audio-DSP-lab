#pragma once

// LabFft: minimal self-contained radix-2 FFT for dsp/lab prototypes.
//
// No third-party dependency (Eigen is available for NAM but a Cooley-Tukey
// loop is smaller, dependency-free, and fast enough at lab frame sizes).
// Buffers are caller-owned; init() precomputes tables once (off-RT), the
// transforms themselves allocate nothing and are deterministic.

#include <complex>
#include <vector>

namespace tdm
{
namespace lab
{
class LabFft
{
public:
  LabFft() = default;
  explicit LabFft(int size) { init(size); }

  // Off-RT: size must be a power of two >= 64. Throws std::invalid_argument
  // otherwise. Precomputes the bit-reversal table and twiddles.
  void init(int size);

  int size() const { return size_; }

  // In-place forward DFT: X[k] = sum_n x[n] e^(-j 2 pi k n / N).
  // io must hold size() elements. No allocation.
  void forward(std::complex<float>* io) const;

  // In-place inverse DFT with 1/N scaling:
  // x[n] = (1/N) sum_k X[k] e^(+j 2 pi k n / N). No allocation.
  void inverse(std::complex<float>* io) const;

private:
  int size_ = 0;
  int log2_ = 0;
  std::vector<int> rev_; // bit-reversed indices
  std::vector<std::complex<float>> tw_; // e^(-j 2 pi k / N), k = 0..N/2-1
};
} // namespace lab
} // namespace tdm
