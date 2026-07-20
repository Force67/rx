#include "audio/thunder_synth.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using namespace rx;

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition) return;
  std::fprintf(stderr, "thunder_synth_test: FAIL: %s\n", message);
  ++failures;
}

std::vector<float> Decode(audio::Decoder &decoder, u32 block_size) {
  std::vector<float> samples;
  std::vector<float> block(block_size);
  for (;;) {
    u32 read = decoder.Read(block.data(), block_size);
    if (read == 0) break;
    samples.insert(samples.end(), block.begin(), block.begin() + read);
  }
  return samples;
}

void TestInvalidInputs() {
  Check(!audio::MakeThunder(0, 1, 1.0f, 100.0f), "zero sample rate is rejected");
  Check(!audio::MakeThunder(std::numeric_limits<u32>::max(), 1, 1.0f, 100.0f),
        "unreasonable sample rates are rejected");
  Check(!audio::MakeThunder(48000, 1, std::numeric_limits<f32>::quiet_NaN(), 100.0f),
        "non-finite energy is rejected");
  Check(!audio::MakeThunder(48000, 1, 1.0f,
                            std::numeric_limits<f32>::infinity()),
        "non-finite distance is rejected");
}

void TestFiniteDeterministicStream() {
  auto a = audio::MakeThunder(12000, 0x1234u, 0.8f, 750.0f);
  auto b = audio::MakeThunder(12000, 0x1234u, 0.8f, 750.0f);
  Check(a && b, "valid thunder decoders are created");
  if (!a || !b) return;
  std::vector<float> lhs = Decode(*a, 127);
  std::vector<float> rhs = Decode(*b, 509);
  Check(lhs == rhs, "stream output is deterministic across mixer block sizes");
  Check(lhs.size() == a->frame_count(), "decoder emits its advertised frame count");
  bool finite = true;
  bool non_silent = false;
  for (float sample : lhs) {
    finite &= std::isfinite(sample) && std::abs(sample) <= 0.951f;
    non_silent |= std::abs(sample) > 1e-4f;
  }
  Check(finite, "all samples remain finite and bounded");
  Check(non_silent, "positive energy produces an audible signal");
  Check(!lhs.empty() && std::abs(lhs.back()) < 1e-4f,
        "the end-of-stream fade reaches silence");
}

void TestZeroEnergyIsSilent() {
  auto decoder = audio::MakeThunder(8000, 4u, 0.0f, 0.0f);
  Check(static_cast<bool>(decoder), "zero energy still yields a finite decoder");
  if (!decoder) return;
  std::vector<float> samples = Decode(*decoder, 256);
  bool silent = true;
  for (float sample : samples)
    silent &= sample == 0.0f;
  Check(silent, "zero energy produces silence rather than a loud rumble floor");
}

} // namespace

int main() {
  TestInvalidInputs();
  TestFiniteDeterministicStream();
  TestZeroEnergyIsSilent();
  if (failures == 0) {
    std::printf("thunder_synth_test: OK\n");
    return 0;
  }
  std::fprintf(stderr, "thunder_synth_test: %d failure(s)\n", failures);
  return failures;
}
