#include "audio/thunder_synth.h"
#include "base/containers/vector.h"
#include "base/numeric_limits.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

using namespace rx;

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition) return;
  ::fprintf(stderr, "thunder_synth_test: FAIL: %s\n", message);
  ++failures;
}

base::Vector<float> Decode(audio::Decoder &decoder, u32 block_size) {
  base::Vector<float> samples;
  base::Vector<float> block(block_size);
  for (;;) {
    u32 read = decoder.Read(block.data(), block_size);
    if (read == 0) break;
    samples.insert(samples.end(), block.begin(), block.begin() + read);
  }
  return samples;
}

void TestInvalidInputs() {
  Check(!audio::MakeThunder(0, 1, 1.0f, 100.0f), "zero sample rate is rejected");
  Check(!audio::MakeThunder(base::MinMax<u32>::max(), 1, 1.0f, 100.0f),
        "unreasonable sample rates are rejected");
  Check(!audio::MakeThunder(48000, 1, NAN, 100.0f),
        "non-finite energy is rejected");
  Check(!audio::MakeThunder(48000, 1, 1.0f,
                            INFINITY),
        "non-finite distance is rejected");
}

void TestFiniteDeterministicStream() {
  auto a = audio::MakeThunder(12000, 0x1234u, 0.8f, 750.0f);
  auto b = audio::MakeThunder(12000, 0x1234u, 0.8f, 750.0f);
  Check(a && b, "valid thunder decoders are created");
  if (!a || !b) return;
  base::Vector<float> lhs = Decode(*a, 127);
  base::Vector<float> rhs = Decode(*b, 509);
  Check(lhs == rhs, "stream output is deterministic across mixer block sizes");
  Check(lhs.size() == a->frame_count(), "decoder emits its advertised frame count");
  bool finite = true;
  bool non_silent = false;
  for (float sample : lhs) {
    finite &= ::isfinite(sample) && ::abs(sample) <= 0.951f;
    non_silent |= ::abs(sample) > 1e-4f;
  }
  Check(finite, "all samples remain finite and bounded");
  Check(non_silent, "positive energy produces an audible signal");
  Check(!lhs.empty() && ::abs(lhs.back()) < 1e-4f,
        "the end-of-stream fade reaches silence");
}

void TestZeroEnergyIsSilent() {
  auto decoder = audio::MakeThunder(8000, 4u, 0.0f, 0.0f);
  Check(static_cast<bool>(decoder), "zero energy still yields a finite decoder");
  if (!decoder) return;
  base::Vector<float> samples = Decode(*decoder, 256);
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
    ::printf("thunder_synth_test: OK\n");
    return 0;
  }
  ::fprintf(stderr, "thunder_synth_test: %d failure(s)\n", failures);
  return failures;
}
