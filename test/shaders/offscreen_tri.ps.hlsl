// Solid red, so the readback can tell a covered centre pixel (red) from the
// cleared background (blue) by channel.
float4 main() : SV_Target {
  return float4(1.0, 0.0, 0.0, 1.0);
}
