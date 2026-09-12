[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> result : register(u1, space0);

[numthreads(1, 1, 1)]
void main() {
  RayDesc ray;
  ray.Origin = float3(0, 0, -1);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = 0.0;
  ray.TMax = 10.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
  query.Proceed();
  result[0] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT
                  ? query.CommittedRayT() : -1.0;
}
