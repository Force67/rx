// Morph target acceptance test, in two parts.
//
// CPU: imports the Khronos AnimatedMorphCube (CC0, committed at
// test/data/AnimatedMorphCube.glb) and checks the imported targets and weight
// animation, plus target naming/lookup and STEP sampling through a tiny
// generated glTF (the cube ships no targetNames).
//
// GPU: packs the delta buffer exactly like Renderer::UploadMesh
// ([target][vertex] 36-byte position/normal/tangent triples), runs the shared
// morph.hlsli accumulation (what mesh.vs executes before skinning) in a
// compute shader on an offscreen device and compares the result against a CPU
// reference. Vulkan only - the shader reads through buffer device addresses
// like the skinned path - and skips cleanly when no driver is present.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "anim/morph.h"
#include "asset/gltf_loader.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

#include "shaders/morph_apply_cs_hlsl.h"

using namespace rx;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "morph_test: FAIL: %s\n", msg);
  return 1;
}

// One triangle with two named position-morph targets and a 2-key STEP weight
// animation. Buffer: positions @0, target deltas @36/@72 ((0,0,1) each vertex
// / (1,0,0) on vertex 0), times [0,1] @108, weight rows (0.25, 0.75), (1, 0)
// @116.
constexpr char kNamedTargetsGltf[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{
    "primitives": [{
      "attributes": {"POSITION": 0},
      "targets": [{"POSITION": 1}, {"POSITION": 2}]
    }],
    "extras": {"targetNames": ["smile", "frown"]}
  }],
  "animations": [{
    "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}],
    "samplers": [{"input": 3, "output": 4, "interpolation": "STEP"}]
  }],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 4, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 36},
    {"buffer": 0, "byteOffset": 108, "byteLength": 8},
    {"buffer": 0, "byteOffset": 116, "byteLength": 16}
  ],
  "buffers": [{"byteLength": 132, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgD8AAIA+AABAPwAAgD8AAAAA"}]
})";

bool Near(f32 a, f32 b, f32 tolerance = 1e-5f) { return std::abs(a - b) <= tolerance; }

int TestNamedTargets() {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rx_morph_named_targets.gltf";
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) return Fail("cannot write the generated gltf");
  std::fwrite(kNamedTargetsGltf, 1, sizeof(kNamedTargetsGltf) - 1, file);
  std::fclose(file);

  asset::GltfScene scene;
  bool loaded = asset::LoadGltfScene(path.string(), &scene);
  std::filesystem::remove(path);
  if (!loaded || scene.meshes.size() != 1) return Fail("generated gltf did not load");

  const asset::Mesh& mesh = scene.meshes[0];
  if (mesh.morph_targets.size() != 2) return Fail("expected 2 named targets");
  if (mesh.morph_targets[0].name != "smile" || mesh.morph_targets[1].name != "frown") {
    return Fail("targetNames not imported");
  }
  if (mesh.FindMorphTarget(asset::MakeAssetId("frown").hash) != 1 ||
      mesh.FindMorphTarget(asset::MakeAssetId("scowl").hash) != -1) {
    return Fail("FindMorphTarget lookup by name hash");
  }
  if (mesh.morph_targets[0].position_deltas.size() != 9 ||
      !Near(mesh.morph_targets[0].position_deltas[2], 1.0f) ||
      !Near(mesh.morph_targets[1].position_deltas[0], 1.0f)) {
    return Fail("generated target deltas");
  }

  if (mesh.morph_animations.size() != 1) return Fail("expected 1 weight animation");
  const asset::MorphAnimation& track = mesh.morph_animations[0];
  if (!track.step || !Near(track.duration, 1.0f)) return Fail("STEP track import");
  base::Vector<f32> weights;
  anim::SampleMorphWeights(track, 0.5f, &weights);  // STEP holds the first key
  if (weights.size() != 2 || !Near(weights[0], 0.25f) || !Near(weights[1], 0.75f)) {
    return Fail("STEP sampling should hold the previous key");
  }
  anim::SampleMorphWeights(track, 1.0f, &weights);
  if (!Near(weights[0], 1.0f) || !Near(weights[1], 0.0f)) {
    return Fail("sampling at the last key");
  }
  return 0;
}

int TestAnimatedMorphCube(const char* path) {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(path, &scene)) return Fail("AnimatedMorphCube did not load");

  const asset::Mesh* cube = nullptr;
  for (const asset::Mesh& mesh : scene.meshes) {
    if (!mesh.morph_targets.empty()) cube = &mesh;
  }
  if (!cube) return Fail("no mesh with morph targets");
  if (cube->morph_targets.size() != 2) return Fail("expected 2 targets on the cube");

  const size_t verts = cube->lods[0].vertices.size();
  f32 magnitude = 0;
  for (const asset::MorphTarget& target : cube->morph_targets) {
    if (target.position_deltas.size() != verts * 3) return Fail("position delta size");
    if (target.normal_deltas.size() != verts * 3) return Fail("normal delta size");
    if (target.tangent_deltas.size() != verts * 3) return Fail("tangent delta size");
    for (f32 d : target.position_deltas) magnitude = std::max(magnitude, std::abs(d));
  }
  if (magnitude <= 0) return Fail("all position deltas are zero");

  if (cube->morph_animations.size() != 1) return Fail("expected 1 weight animation");
  const asset::MorphAnimation& track = cube->morph_animations[0];
  const u32 keys = static_cast<u32>(track.times.size());
  if (keys < 2 || track.weights.size() != keys * 2 || track.duration <= 0) {
    return Fail("weight track shape");
  }

  base::Vector<f32> weights;
  anim::SampleMorphWeights(track, track.times[0], &weights);
  if (weights.size() != 2 || !Near(weights[0], track.weights[0]) ||
      !Near(weights[1], track.weights[1])) {
    return Fail("sampling at the first key");
  }
  // Midpoint of an interior key pair against a manual lerp.
  u32 key = keys / 2;
  f32 mid = (track.times[key] + track.times[key + 1]) * 0.5f;
  f32 t = (mid - track.times[key]) / (track.times[key + 1] - track.times[key]);
  anim::SampleMorphWeights(track, mid, &weights);
  for (u32 i = 0; i < 2; ++i) {
    f32 a = track.weights[key * 2 + i];
    f32 b = track.weights[(key + 1) * 2 + i];
    if (!Near(weights[i], a + (b - a) * t)) return Fail("linear interpolation between keys");
  }
  anim::SampleMorphWeights(track, track.duration + 1.0f, &weights);
  if (!Near(weights[0], track.weights[(keys - 1) * 2])) return Fail("clamping past the last key");
  return 0;
}

// Packs deltas like Renderer::UploadMesh and base vertices in the same triple
// layout, evaluates weights {0: 0.65, 1: 0.35} on the GPU through morph.hlsli
// and compares against the CPU accumulation.
int TestGpuEvaluation(const char* path) {
  using namespace rx::render;
  if (const char* rhi = std::getenv("RX_RHI")) {
    if (std::strcmp(rhi, "vulkan") != 0) {
      std::printf("morph_test: gpu section is vulkan-only (bda compute), skipping\n");
      return 0;
    }
  }

  asset::GltfScene scene;
  if (!asset::LoadGltfScene(path, &scene)) return Fail("AnimatedMorphCube did not load");
  const asset::Mesh& mesh = scene.meshes[0];
  const asset::MeshLod& lod = mesh.lods[0];
  const u32 verts = static_cast<u32>(lod.vertices.size());
  const u32 targets = static_cast<u32>(mesh.morph_targets.size());

  DeviceDesc desc;
  desc.backend = Backend::kVulkan;
  desc.request_raytracing = false;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");
  if (device->is_stub()) {
    std::printf("morph_test: no vulkan driver, skipping the gpu section\n");
    return 0;
  }
  std::printf("morph_test: device '%s'\n", device->caps().adapter_name.c_str());

  // Delta buffer in the GpuMesh::morph_deltas layout.
  std::vector<f32> deltas(static_cast<size_t>(targets) * verts * 9, 0.0f);
  for (u32 t = 0; t < targets; ++t) {
    const asset::MorphTarget& target = mesh.morph_targets[t];
    for (u32 v = 0; v < verts; ++v) {
      f32* out = &deltas[(static_cast<size_t>(t) * verts + v) * 9];
      std::memcpy(out, &target.position_deltas[v * 3], sizeof(f32) * 3);
      if (!target.normal_deltas.empty()) {
        std::memcpy(out + 3, &target.normal_deltas[v * 3], sizeof(f32) * 3);
      }
      if (!target.tangent_deltas.empty()) {
        std::memcpy(out + 6, &target.tangent_deltas[v * 3], sizeof(f32) * 3);
      }
    }
  }
  std::vector<f32> base(static_cast<size_t>(verts) * 9);
  for (u32 v = 0; v < verts; ++v) {
    const asset::Vertex& vertex = lod.vertices[v];
    std::memcpy(&base[v * 9 + 0], vertex.position, sizeof(f32) * 3);
    std::memcpy(&base[v * 9 + 3], vertex.normal, sizeof(f32) * 3);
    std::memcpy(&base[v * 9 + 6], vertex.tangent, sizeof(f32) * 3);
  }
  struct Pair {
    u32 target;
    f32 weight;
  };
  const Pair pairs[2] = {{0, 0.65f}, {1, 0.35f}};

  auto bytes = [](const void* data, size_t size) {
    return ByteSpan(static_cast<const u8*>(data), size);
  };
  GpuBuffer delta_buffer = device->CreateBufferWithData(
      bytes(deltas.data(), deltas.size() * sizeof(f32)),
      kBufferUsageStorage | kBufferUsageDeviceAddress);
  GpuBuffer weight_buffer = device->CreateBufferWithData(
      bytes(pairs, sizeof(pairs)), kBufferUsageStorage | kBufferUsageDeviceAddress);
  GpuBuffer base_buffer = device->CreateBufferWithData(
      bytes(base.data(), base.size() * sizeof(f32)), kBufferUsageStorage);
  GpuBuffer out_buffer =
      device->CreateBuffer(base.size() * sizeof(f32), kBufferUsageStorage, true);
  if (!delta_buffer.address || !weight_buffer.address || !out_buffer.mapped) {
    return Fail("buffer creation");
  }

  ComputePipelineDesc pd;
  pd.shader = RX_SHADER(k_morph_apply_cs_hlsl);
  pd.sets.push_back({.slots = {{0, BindingType::kByteBuffer}, {1, BindingType::kStorageBuffer}}});
  struct Push {
    rx::u64 delta_address;
    rx::u64 weight_address;
    u32 weight_count;
    u32 vertex_count;
  } push{delta_buffer.address, weight_buffer.address, 2, verts};
  pd.push_constant_size = sizeof(Push);
  pd.debug_name = "morph_apply";
  PipelineHandle pipeline = device->CreateComputePipeline(pd);
  if (!pipeline) return Fail("CreateComputePipeline returned null");

  CommandList* cmd = device->BeginFrame(0);
  if (!cmd) return Fail("BeginFrame returned null");
  cmd->BindPipeline(pipeline);
  cmd->BindTransient(0, {Bind::ByteBuffer(0, base_buffer), Bind::StorageBuffer(1, out_buffer)});
  cmd->Push(push);
  cmd->Dispatch((verts + 63) / 64, 1, 1);
  cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kHostRead);
  device->SubmitFrame(cmd);
  device->WaitIdle();

  const f32* result = static_cast<const f32*>(out_buffer.mapped);
  f32 worst = 0;
  for (u32 v = 0; v < verts; ++v) {
    for (u32 c = 0; c < 9; ++c) {
      f32 expected = base[v * 9 + c];
      for (const Pair& pair : pairs) {
        expected += pair.weight * deltas[(static_cast<size_t>(pair.target) * verts + v) * 9 + c];
      }
      worst = std::max(worst, std::abs(result[v * 9 + c] - expected));
    }
  }
  std::printf("morph_test: gpu vs cpu max error %g\n", worst);
  if (worst > 1e-5f) return Fail("gpu evaluation diverges from the cpu reference");

  device->DestroyPipeline(pipeline);
  device->DestroyBuffer(delta_buffer);
  device->DestroyBuffer(weight_buffer);
  device->DestroyBuffer(base_buffer);
  device->DestroyBuffer(out_buffer);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Fail("usage: morph_test <AnimatedMorphCube.glb>");
  if (int rc = TestNamedTargets()) return rc;
  if (int rc = TestAnimatedMorphCube(argv[1])) return rc;
  if (int rc = TestGpuEvaluation(argv[1])) return rc;
  std::printf("morph_test: PASS\n");
  return 0;
}
