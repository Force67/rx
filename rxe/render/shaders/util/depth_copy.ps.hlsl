// Rebuilds a single-sampled hardware depth buffer from the resolved raw-depth
// export, so the post-resolve raster passes (sky, water, transparency, debug
// overlays) keep their depth test in kMsaa mode. Paired with fullscreen.vs.
[[vk::binding(0, 0)]] Texture2D depth_src : register(t0, space0);

float main(float4 pos : SV_Position) : SV_Depth {
  return depth_src.Load(int3(int2(pos.xy), 0)).r;
}
