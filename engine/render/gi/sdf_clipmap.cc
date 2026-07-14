#include "render/gi/sdf_clipmap.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/sdf_scene.h"
#include "render/rhi/bindings.h"
#include "shaders/sdf_clear_cs_hlsl.h"
#include "shaders/sdf_compose_cs_hlsl.h"
#include "shaders/sdf_debug_cs_hlsl.h"

namespace rx::render {
namespace {

// Mirrors SdfGlobals in shaders/gi/sdf_trace.hlsli.
struct SdfGlobals {
  f32 clip_origin[SdfClipmap::kClips][4];  // xyz world min, w voxel size
  f32 clip_params[4];                      // x res, y clips, z total_z, w far
  f32 camera_pos[4];
};

struct ClearPush {
  u32 dims[4];    // x res, y clip index
  f32 params[4];  // x far distance
};

struct ComposePush {
  Mat4 inv_transform;
  f32 box_min[4];      // xyz mesh-local volume min, w mesh voxel size
  u32 mesh_res[4];     // xyz mesh resolution, w clip index
  f32 clip_origin[4];  // xyz clip world min, w clip voxel size
  f32 albedo[4];
  f32 emissive[4];
  f32 misc[4];  // x conservative world scale
};

struct DebugPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 params[4];  // x inv_w, y inv_h, z tmax
  u32 misc[4];    // x mode
};

// Per-clip work captured into the compose execute lambda.
struct ClipJob {
  u32 clip;
  f32 origin[3];
  f32 voxel;
};

f32 ClipVoxel(u32 clip) {
  return (SdfClipmap::kBaseExtent * static_cast<f32>(1u << clip)) / SdfClipmap::kRes;
}

}  // namespace

SdfClipmap::~SdfClipmap() {
  for (PipelineHandle* p : {&clear_pipeline_, &compose_pipeline_, &debug_pipeline_}) {
    if (*p) device_.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage* img : {&distance_, &albedo_, &emissive_}) {
    if (*img) device_.DestroyImage(*img);
    *img = {};
  }
  for (GpuBuffer& b : globals_buffers_) {
    if (b) device_.DestroyBuffer(b);
  }
}

bool SdfClipmap::Initialize() {
  const TextureUsageFlags usage = kTextureUsageStorage | kTextureUsageSampled;
  const u32 total_z = kRes * kClips;
  distance_ = device_.CreateImage3D(Format::kR16Float, kRes, kRes, total_z, usage);
  albedo_ = device_.CreateImage3D(Format::kRGBA8Unorm, kRes, kRes, total_z, usage);
  emissive_ = device_.CreateImage3D(Format::kRGBA8Unorm, kRes, kRes, total_z, usage);
  if (!distance_ || !albedo_ || !emissive_) {
    RX_WARN("sdf clipmap volumes unavailable (no 3d storage image support)");
    return false;
  }

  sampler_ = device_.GetSampler({.min_filter = Filter::kLinear,
                                 .mag_filter = Filter::kLinear,
                                 .mip_filter = Filter::kNearest,
                                 .address_u = AddressMode::kClampToEdge,
                                 .address_v = AddressMode::kClampToEdge,
                                 .address_w = AddressMode::kClampToEdge,
                                 .max_lod = 0.0f});
  if (!sampler_) return false;

  for (GpuBuffer& b : globals_buffers_) {
    b = device_.CreateBuffer(sizeof(SdfGlobals), kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }

  clear_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_sdf_clear_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(ClearPush),
      .debug_name = "sdf_clear",
  });
  compose_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_sdf_compose_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(ComposePush),
      .debug_name = "sdf_compose",
  });
  debug_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_sdf_debug_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kUniformBuffer},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampler}}}},
      .push_constant_size = sizeof(DebugPush),
      .debug_name = "sdf_debug",
  });
  if (!clear_pipeline_ || !compose_pipeline_ || !debug_pipeline_) {
    RX_ERROR("sdf clipmap pipeline creation failed");
    return false;
  }

  // Prime the volumes to kGeneral (the compose pass clears each clip it touches;
  // the first frame recomposes all four, so no explicit clear is needed here).
  device_.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier b[3] = {
        Transition(distance_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(albedo_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(emissive_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(b);
  });
  volumes_initialized_ = true;
  return true;
}

Vec3 SdfClipmap::SnapOrigin(const Vec3& camera, u32 clip) const {
  f32 extent = kBaseExtent * static_cast<f32>(1u << clip);
  f32 voxel = extent / kRes;
  // Snap the clip origin on a granularity of 8 voxels, NOT 1. The RCGI cascades
  // this clipmap mirrors snap their origin to their PROBE SPACING -- `floor((cam -
  // half)/spacing)*spacing` -- and cascade N's probe spacing is exactly 8x this
  // clip's voxel (RCGI base spacing 2 m * 2^N vs clip voxel kBaseExtent/kRes * 2^N
  // = 0.25 m * 2^N; ratio 8). Matching the snap granularity keeps the 256 m clip-3
  // and 240 m cascade-3 volumes coincident so every cascade probe -- including the
  // outer faces -- lands inside this clip's PHYSICAL voxel extent; snapping finer
  // (per-voxel) let the two origins drift by up to a probe spacing, pushing outer
  // probes several metres outside the coarsest clip and injecting sky / false hits.
  //
  // The formulas are otherwise identical to RCGI's (same camera eye, std::floor,
  // per-axis) except the half offset: clip half = extent/2 = 64 voxels = 8 snaps,
  // vs RCGI half = (kProbesPerAxis-1)*spacing/2 = 7.5 spacings. With snap == spacing
  // this leaves the two shared origins differing by only 0 or +1 snap, so the outer
  // cascade probe lands at most exactly on a clip face (handled by the zero-margin
  // physical-extent origin classification in sdf_trace.hlsli). The two also update
  // on different cadences -- RCGI re-snaps only the round-robin *current* cascade
  // each frame (its blended origin lags for the other three), while this clipmap
  // re-snaps every clip that moved -- so during fast motion the origins can
  // transiently disagree by up to one snap; the first-sample-after-entry backface
  // backstop in sdf_trace.hlsli is the safety net for that window.
  const f32 snap = voxel * 8.0f;
  f32 half = extent * 0.5f;
  return Vec3{std::floor((camera.x - half) / snap) * snap,
             std::floor((camera.y - half) / snap) * snap,
             std::floor((camera.z - half) / snap) * snap};
}

void SdfClipmap::WriteGlobals(u32 frame_index, const Vec3& camera) {
  SdfGlobals g{};
  for (u32 c = 0; c < kClips; ++c) {
    g.clip_origin[c][0] = clip_origin_[c].x;
    g.clip_origin[c][1] = clip_origin_[c].y;
    g.clip_origin[c][2] = clip_origin_[c].z;
    g.clip_origin[c][3] = ClipVoxel(c);
  }
  g.clip_params[0] = static_cast<f32>(kRes);
  g.clip_params[1] = static_cast<f32>(kClips);
  g.clip_params[2] = static_cast<f32>(kRes * kClips);
  g.clip_params[3] = kFarDistance;
  g.camera_pos[0] = camera.x;
  g.camera_pos[1] = camera.y;
  g.camera_pos[2] = camera.z;
  std::memcpy(globals_buffers_[frame_index % 2].mapped, &g, sizeof(g));
}

void SdfClipmap::AddComposeToGraph(RenderGraph& graph, const SdfScene& scene,
                                   base::Vector<Instance> instances, const Vec3& camera,
                                   u32 frame_index) {
  if (!volumes_initialized_) return;

  const u32 current = frame_index % kClips;
  base::Vector<ClipJob> jobs;
  for (u32 c = 0; c < kClips; ++c) {
    Vec3 snapped = SnapOrigin(camera, c);
    bool changed = !clips_valid_ || snapped.x != clip_origin_[c].x ||
                   snapped.y != clip_origin_[c].y || snapped.z != clip_origin_[c].z;
    if (changed || c == current) {
      clip_origin_[c] = snapped;
      ClipJob j{};
      j.clip = c;
      j.origin[0] = snapped.x;
      j.origin[1] = snapped.y;
      j.origin[2] = snapped.z;
      j.voxel = ClipVoxel(c);
      jobs.push_back(j);
    }
  }
  clips_valid_ = true;
  WriteGlobals(frame_index, camera);
  if (jobs.empty()) return;

  const SdfScene* scene_ptr = &scene;
  graph.AddPass(
      "sdf_compose", [](RenderGraph::PassBuilder&) {},
      [this, scene_ptr, jobs = std::move(jobs), instances = std::move(instances)](PassContext& ctx) {
        CommandList* cmd = ctx.cmd;
        // Order this frame's writes after any prior-frame reads of the volumes.
        cmd->MemoryBarrier(BarrierScope::kComputeRead, BarrierScope::kComputeWrite);

        const u32 groups = SdfClipmap::kRes / 4;
        for (const ClipJob& j : jobs) {
          ClearPush cp{};
          cp.dims[0] = SdfClipmap::kRes;
          cp.dims[1] = j.clip;
          cp.params[0] = SdfClipmap::kFarDistance;
          cmd->BindPipeline(clear_pipeline_);
          cmd->BindTransient(0, {Bind::Storage(0, distance_), Bind::Storage(1, albedo_),
                                 Bind::Storage(2, emissive_)});
          cmd->Push(cp);
          cmd->Dispatch(groups, groups, groups);
          cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

          for (const Instance& inst : instances) {
            const SdfScene::MeshSdf* mesh = scene_ptr->Find(inst.mesh_key);
            if (!mesh) continue;

            // Conservative local->world distance scale: the transform's minimum
            // axis scale is a safe lower bound on world distance (so the sphere
            // trace never overshoots). The design doc names the max axis; the
            // minimum is the correct conservative choice for non-overshoot -- see
            // SDF_TRACE.md S1 notes.
            const f32* m = inst.transform.m;
            f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
            f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
            f32 min_scale = std::min(sx, std::min(sy, sz));

            ComposePush pp{};
            pp.inv_transform = Inverse(inst.transform);
            pp.box_min[0] = mesh->box_min[0];
            pp.box_min[1] = mesh->box_min[1];
            pp.box_min[2] = mesh->box_min[2];
            pp.box_min[3] = mesh->voxel;
            pp.mesh_res[0] = mesh->res[0];
            pp.mesh_res[1] = mesh->res[1];
            pp.mesh_res[2] = mesh->res[2];
            pp.mesh_res[3] = j.clip;
            pp.clip_origin[0] = j.origin[0];
            pp.clip_origin[1] = j.origin[1];
            pp.clip_origin[2] = j.origin[2];
            pp.clip_origin[3] = j.voxel;
            std::memcpy(pp.albedo, mesh->albedo, sizeof(f32) * 3);
            std::memcpy(pp.emissive, mesh->emissive, sizeof(f32) * 3);
            pp.misc[0] = min_scale;

            cmd->BindPipeline(compose_pipeline_);
            cmd->BindTransient(0, {Bind::Storage(0, distance_), Bind::Storage(1, albedo_),
                                   Bind::Storage(2, emissive_),
                                   Bind::StorageBuffer(3, mesh->sdf, 0, mesh->sdf.size)});
            cmd->Push(pp);
            cmd->Dispatch(groups, groups, groups);
            // RMW ordering across overlapping instance dispatches.
            cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          }
        }
        // Make the composed volumes visible to later readers (debug / S2 trace).
        cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
      });
}

void SdfClipmap::AddDebugPass(RenderGraph& graph, ResourceHandle lit, Extent2D extent,
                              const Mat4& inv_view_proj, const Vec3& camera, u32 mode,
                              u32 frame_index) {
  if (!volumes_initialized_ || lit == kInvalidResource) return;
  graph.AddPass(
      "sdf_debug",
      [lit](RenderGraph::PassBuilder& b) { b.Write(lit, ResourceUsage::kStorageWrite); },
      [this, lit, extent, inv_view_proj, camera, mode, frame_index](PassContext& ctx) {
        const GpuImage& out = ctx.graph->image(lit);
        DebugPush push{};
        push.inv_view_proj = inv_view_proj;
        push.camera_pos[0] = camera.x;
        push.camera_pos[1] = camera.y;
        push.camera_pos[2] = camera.z;
        push.params[0] = 1.0f / static_cast<f32>(extent.width);
        push.params[1] = 1.0f / static_cast<f32>(extent.height);
        push.params[2] = kBaseExtent * static_cast<f32>(1u << (kClips - 1));  // coarsest extent
        push.misc[0] = mode;
        ctx.cmd->BindPipeline(debug_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, out), Bind::Uniform(1, globals_buffers_[frame_index % 2], 0,
                                                     sizeof(SdfGlobals)),
                InGeneral(Bind::Sampled(2, distance_)), InGeneral(Bind::Sampled(3, albedo_)),
                InGeneral(Bind::Sampled(4, emissive_)), Bind::Sampler(5, sampler_)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(out.extent);
      });
}

}  // namespace rx::render
