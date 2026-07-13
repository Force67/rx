// Persistent concurrent binary triangle tree for planar ocean geometry.
// Phase 0 resets a changed surface, phase 1 atomically splits/merges leaves,
// phase 2 updates only dirty vertex and indirect-command slots.

struct PushData {
  column_major float4x4 local_to_clip;
  float4 bounds;             // min xz, max xz
  float4 camera_height_time; // local camera xz, height, time
  float4 metrics;            // target edge px, render width/height
  uint4 control;             // phase, nodes/tree, max depth, budget
};
[[vk::push_constant]] ConstantBuffer<PushData> push : register(b0, space0);

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> states : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> counters : register(u1, space0);

// asset::Vertex is deliberately packed to 52 bytes on the CPU/vertex-input
// side. A StructuredBuffer float3 layout becomes 64-byte std430 in SPIR-V,
// so write the packed ABI explicitly instead of relying on structure stride.
[[vk::binding(2, 0)]] RWByteAddressBuffer vertices : register(u2, space0);

struct DrawCommand {
  uint vertex_count;
  uint instance_count;
  uint first_vertex;
  uint first_instance;
};
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawCommand> commands : register(u3, space0);

static const uint kLeaf = 1u;
static const uint kSplit = 2u;
static const uint kDirty = 4u;

void DecodeNode(uint global_node, out uint tree, out uint heap, out uint level) {
  tree = global_node / push.control.y;
  heap = global_node - tree * push.control.y + 1u;
  level = (uint)firstbithigh(heap);
}

// Longest-edge bisection. The implicit heap path is the complete CBT: no
// per-node triangle coordinates need to be persisted.
void TriangleForNode(uint tree, uint heap, uint level, out float2 a, out float2 b,
                     out float2 c) {
  float2 lo = push.bounds.xy;
  float2 hi = push.bounds.zw;
  if (tree == 0u) {
    a = lo;
    b = hi;
    c = float2(lo.x, hi.y);
  } else {
    a = hi;
    b = lo;
    c = float2(hi.x, lo.y);
  }
  for (uint depth = level; depth > 0u; --depth) {
    uint child = (heap >> (depth - 1u)) & 1u;
    float2 midpoint = (a + b) * 0.5;
    if (child == 0u) {
      float2 old_a = a;
      a = c;
      b = old_a;
      c = midpoint;
    } else {
      float2 old_b = b;
      a = old_b;
      b = c;
      c = midpoint;
    }
  }
}

float2 ScreenPoint(float2 p) {
  float4 clip = mul(push.local_to_clip, float4(p.x, push.camera_height_time.z, p.y, 1.0));
  if (clip.w <= 1e-4) return float2(-1e5, -1e5);
  return (clip.xy / clip.w * 0.5 + 0.5) * push.metrics.yz;
}

float RefinementMetric(float2 a, float2 b, float2 c) {
  float2 sa = ScreenPoint(a), sb = ScreenPoint(b), sc = ScreenPoint(c);
  float edge = max(length(sa - sb), max(length(sb - sc), length(sc - sa)));
  float2 center = (a + b + c) / 3.0;

  // Analytic short-wave compression proxy. It follows the same directions and
  // time evolution as the Gerstner fallback, so steep moving crests receive
  // triangles even before shading displacement runs in the vertex shader.
  float crest = 0.0;
  crest += max(sin(dot(center, float2(0.780869, 0.624695)) * 1.70 + push.camera_height_time.w * 4.08), 0.0);
  crest += max(sin(dot(center, float2(-0.286206, -0.958164)) * 1.35 + push.camera_height_time.w * 3.20), 0.0);
  crest *= 0.5;

  // Projection supplies the main distance weighting. This extra local term
  // keeps geometry dense immediately around the camera at grazing angles.
  float distance_to_eye = length(center - push.camera_height_time.xy);
  float near_weight = 1.0 + saturate(18.0 / max(distance_to_eye, 1.0));
  return edge * near_weight * (1.0 + crest * 1.5);
}

void ResetNode(uint node) {
  states[node] = 0u;
  if (node == 0u) {
    states[0] = kLeaf | kDirty;
    counters[0] = 2u;
  }
  if (node == push.control.y) states[node] = kLeaf | kDirty;
}

void SplitNode(uint node) {
  uint tree, heap, level;
  DecodeNode(node, tree, heap, level);
  uint state = states[node];
  float2 a, b, c;
  TriangleForNode(tree, heap, level, a, b, c);
  float metric = RefinementMetric(a, b, c);

  if ((state & kLeaf) != 0u && level < push.control.z && metric > push.metrics.x * 1.15) {
    uint old_count;
    InterlockedAdd(counters[0], 1u, old_count);
    if (old_count >= push.control.w) {
      InterlockedAdd(counters[0], 0xffffffffu);
      return;
    }
    uint original;
    InterlockedCompareExchange(states[node], state, kSplit | kDirty, original);
    if (original != state) {
      InterlockedAdd(counters[0], 0xffffffffu);
      return;
    }
    uint child0 = tree * push.control.y + heap * 2u - 1u;
    states[child0] = kLeaf | kDirty;
    states[child0 + 1u] = kLeaf | kDirty;
  }
}

void MergeNode(uint node) {
  uint tree, heap, level;
  DecodeNode(node, tree, heap, level);
  uint state = states[node];
  if ((state & kSplit) == 0u) return;
  float2 a, b, c;
  TriangleForNode(tree, heap, level, a, b, c);
  float metric = RefinementMetric(a, b, c);
  // One sibling owner merges a complete leaf pair. The lower threshold is
  // deliberate hysteresis: a camera or crest crossing a boundary cannot make
  // the topology ping-pong every frame.
  if (metric < push.metrics.x * 0.62 || counters[0] > push.control.w) {
    uint child0 = tree * push.control.y + heap * 2u - 1u;
    if (child0 + 1u >= (tree + 1u) * push.control.y) return;
    uint left = states[child0], right = states[child0 + 1u];
    if ((left & kLeaf) == 0u || (right & kLeaf) == 0u) return;
    uint original;
    InterlockedCompareExchange(states[node], state, kLeaf | kDirty, original);
    if (original != state) return;
    states[child0] = kDirty;
    states[child0 + 1u] = kDirty;
    InterlockedAdd(counters[0], 0xffffffffu);
  }
}

void EmitNode(uint node) {
  uint state = states[node];
  if ((state & kDirty) == 0u) return;
  if ((state & kLeaf) != 0u) {
    uint tree, heap, level;
    DecodeNode(node, tree, heap, level);
    float2 a, b, c;
    TriangleForNode(tree, heap, level, a, b, c);

    // Conservative sub-pixel overlap closes floating-point/T-junction pinholes
    // where differently refined dyadic edges meet. The displacement is
    // world-position driven, so overlapping samples still describe one surface.
    float2 center = (a + b + c) / 3.0;
    a = center + (a - center) * 1.00002;
    b = center + (b - center) * 1.00002;
    c = center + (c - center) * 1.00002;
    float2 p[3] = {a, b, c};
    [unroll]
    for (uint i = 0u; i < 3u; ++i) {
      uint offset = (node * 3u + i) * 52u;
      vertices.Store3(offset + 0u,
                      asuint(float3(p[i].x, push.camera_height_time.z, p[i].y)));
      vertices.Store3(offset + 12u, asuint(float3(0.0, 1.0, 0.0)));
      vertices.Store4(offset + 24u, asuint(float4(1.0, 0.0, 0.0, 1.0)));
      vertices.Store2(offset + 40u, asuint(p[i] / 8.0));
      vertices.Store(offset + 48u, 0xffffffffu);
    }
  }
  states[node] = state & ~kDirty;
}

void CompactNode(uint node) {
  if ((states[node] & kLeaf) == 0u) return;
  uint slot;
  InterlockedAdd(counters[1], 1u, slot);
  if (slot >= push.control.w) return;
  DrawCommand draw = {3u, 1u, node * 3u, 0u};
  commands[slot] = draw;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  uint node_count = push.control.y * 2u;
  uint node = dispatch_id.x;
  if (node >= node_count) return;
  if (push.control.x == 0u) ResetNode(node);
  else if (push.control.x == 1u) MergeNode(node);
  else if (push.control.x == 2u) SplitNode(node);
  else if (push.control.x == 3u) EmitNode(node);
  else if (push.control.x == 4u) {
    if (node == 0u) counters[1] = 0u;
  } else if (push.control.x == 5u) {
    CompactNode(node);
  } else if (node == 0u) {
    counters[1] = min(counters[1], push.control.w);
  }
}
