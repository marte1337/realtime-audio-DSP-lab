#include "dsp/WavFile.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tdm
{
namespace
{
uint16_t readU16LE(const char* p)
{
  return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) | (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8));
}

uint32_t readU32LE(const char* p)
{
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0]))
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

int32_t readS24LE(const char* p)
{
  int32_t v = static_cast<int32_t>(static_cast<uint8_t>(p[0]) | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8)
                                   | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16));
  if (v & 0x800000)
    v |= ~0xFFFFFF;
  return v;
}

void writeU16LE(std::ofstream& f, uint16_t v)
{
  const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
  f.write(b, 2);
}

void writeU32LE(std::ofstream& f, uint32_t v)
{
  const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                     static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
  f.write(b, 4);
}
} // namespace

MonoWav loadWavMono(const std::string& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("WAV: cannot open " + path);

  char riff[12];
  f.read(riff, 12);
  if (!f || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
    throw std::runtime_error("WAV: not a RIFF/WAVE file: " + path);

  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;
  std::vector<char> audioBytes;
  bool haveFmt = false, haveData = false;

  while (f && !haveData)
  {
    char chunk[8];
    f.read(chunk, 8);
    if (!f)
      break;
    const uint32_t size = readU32LE(chunk + 4);
    if (std::memcmp(chunk, "fmt ", 4) == 0)
    {
      if (size < 16)
        throw std::runtime_error("WAV: corrupt fmt chunk: " + path);
      char fmt[16];
      f.read(fmt, 16);
      if (!f)
        throw std::runtime_error("WAV: truncated fmt chunk: " + path);
      audioFormat = readU16LE(fmt);
      numChannels = readU16LE(fmt + 2);
      sampleRate = readU32LE(fmt + 4);
      bitsPerSample = readU16LE(fmt + 14);
      if (size > 16)
        f.seekg(static_cast<std::streamoff>(size - 16), std::ios::cur);
      haveFmt = true;
    }
    else if (std::memcmp(chunk, "data", 4) == 0)
    {
      audioBytes.resize(size);
      f.read(audioBytes.data(), static_cast<std::streamsize>(size));
      if (!f)
        throw std::runtime_error("WAV: truncated data chunk: " + path);
      haveData = true;
    }
    else
    {
      f.seekg(static_cast<std::streamoff>(size), std::ios::cur);
    }
  }

  if (!haveFmt || !haveData)
    throw std::runtime_error("WAV: missing fmt/data chunk: " + path);
  if (numChannels < 1 || numChannels > 2)
    throw std::runtime_error("WAV: only mono/stereo supported: " + path);
  if (sampleRate == 0)
    throw std::runtime_error("WAV: invalid sample rate: " + path);

  const bool isFloat = (audioFormat == 3);
  const bool isPcm = (audioFormat == 1);
  if (!isFloat && !isPcm)
    throw std::runtime_error("WAV: only PCM/float32/float64 supported: " + path);

  size_t bytesPerSample = 0;
  if (isFloat)
  {
    if (bitsPerSample != 32 && bitsPerSample != 64)
      throw std::runtime_error("WAV: only 32/64-bit float supported: " + path);
    bytesPerSample = static_cast<size_t>(bitsPerSample / 8);
  }
  else
  {
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)
      throw std::runtime_error("WAV: only 8/16/24/32-bit PCM supported: " + path);
    bytesPerSample = static_cast<size_t>(bitsPerSample / 8);
  }

  const size_t frameBytes = bytesPerSample * numChannels;
  if (frameBytes == 0 || audioBytes.size() % frameBytes != 0)
    throw std::runtime_error("WAV: corrupt data size: " + path);
  const size_t numFrames = audioBytes.size() / frameBytes;

  MonoWav out;
  out.sampleRate = static_cast<double>(sampleRate);
  out.sourceChannels = numChannels;
  out.samples.resize(numFrames);
  const char* p = audioBytes.data();
  for (size_t i = 0; i < numFrames; ++i)
  {
    double acc = 0.0;
    for (int c = 0; c < numChannels; ++c)
    {
      const char* s = p + (i * numChannels + static_cast<size_t>(c)) * bytesPerSample;
      double v = 0.0;
      if (isFloat)
      {
        if (bitsPerSample == 32)
        {
          float fv = 0.0f;
          std::memcpy(&fv, s, 4);
          v = fv;
        }
        else
        {
          double dv = 0.0;
          std::memcpy(&dv, s, 8);
          v = dv;
        }
      }
      else if (bitsPerSample == 8)
        v = (static_cast<double>(static_cast<uint8_t>(s[0])) - 128.0) / 128.0;
      else if (bitsPerSample == 16)
        v = readU16LE(s) <= 0x7FFF ? readU16LE(s) / 32768.0
                                   : (static_cast<int>(readU16LE(s)) - 65536) / 32768.0;
      else if (bitsPerSample == 24)
        v = readS24LE(s) / 8388608.0;
      else
        v = static_cast<double>(static_cast<int32_t>(readU32LE(s))) / 2147483648.0;
      acc += v;
    }
    out.samples[i] = static_cast<float>(acc / numChannels);
  }
  return out;
}

void writeWavFloat32(const std::string& path, const float* const* channels, int numChannels, int numFrames,
                     double sampleRate)
{
  if (numChannels < 1 || numFrames < 0 || sampleRate <= 0.0)
    throw std::runtime_error("WAV write: invalid parameters");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f)
    throw std::runtime_error("WAV write: cannot open " + path);
  const uint32_t sr = static_cast<uint32_t>(sampleRate);
  const uint32_t dataBytes = static_cast<uint32_t>(numFrames) * static_cast<uint32_t>(numChannels) * 4u;
  f.write("RIFF", 4);
  writeU32LE(f, 36 + dataBytes);
  f.write("WAVEfmt ", 8);
  writeU32LE(f, 16);
  writeU16LE(f, 3); // float
  writeU16LE(f, static_cast<uint16_t>(numChannels));
  writeU32LE(f, sr);
  writeU32LE(f, sr * static_cast<uint32_t>(numChannels) * 4u);
  writeU16LE(f, static_cast<uint16_t>(numChannels * 4));
  writeU16LE(f, 32);
  f.write("data", 4);
  writeU32LE(f, dataBytes);
  for (int i = 0; i < numFrames; ++i)
    for (int c = 0; c < numChannels; ++c)
    {
      const float v = channels[c][i];
      f.write(reinterpret_cast<const char*>(&v), 4);
    }
  if (!f)
    throw std::runtime_error("WAV write: failed writing " + path);
}
} // namespace tdm
