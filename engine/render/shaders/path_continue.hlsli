#ifndef RX_PATH_CONTINUE_HLSLI_
#define RX_PATH_CONTINUE_HLSLI_

bool ContinuePath(inout float3 throughput, inout uint rng) {
  float survival = min(max(throughput.r, max(throughput.g, throughput.b)) / 0.01, 1.0);
  if (survival >= 1.0) return true;
  if (!(survival > 0.0) || Rand(rng) >= survival) return false;
  throughput /= survival;
  return true;
}

#endif
