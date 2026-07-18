#ifndef RX_RENDER_RENDERER_H_
#define RX_RENDER_RENDERER_H_

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "core/export.h"
#include "core/math.h"
#include "core/window.h"
#include "render/atmosphere/aerial_perspective.h"
#include "render/atmosphere/clouds.h"
#include "render/atmosphere/cloudscape.h"
#include "render/atmosphere/environment.h"
#include "render/atmosphere/froxel_fog.h"
#include "render/atmosphere/lightning.h"
#include "render/atmosphere/precip_occlusion.h"
#include "render/atmosphere/precip_volume.h"
#include "render/atmosphere/precipitation.h"
#include "render/atmosphere/surface_weather.h"
#include "render/atmosphere/volumetric_fog.h"
#include "render/core/bindless.h"
#include "render/core/dynamic_resolution.h"
#include "render/core/render_graph.h"
#include "render/core/settings.h"
#include "render/geometry/fur.h"
#include "render/geometry/gaussian.h"
#include "render/geometry/hair_strands.h"
#include "render/geometry/imposters.h"
#include "render/geometry/instance_store.h"
#include "render/geometry/ocean_fft.h"
#include "render/geometry/particle_emitters.h"
#include "render/geometry/particles.h"
#include "render/geometry/fluid_sim.h"
#include "render/geometry/fluid_surface.h"
#include "render/geometry/shore_wetting.h"
#include "render/geometry/water.h"
#include "render/geometry/water_caustics.h"
#include "render/geometry/water_field.h"
#include "render/geometry/wboit.h"
#include "render/gi/ddgi.h"
#include "render/gi/denoiser_nrd.h"
#include "render/gi/denoiser_rr.h"
#include "render/gi/light_grid.h"
#include "render/gi/local_shadows.h"
#include "render/gi/path_tracer.h"
#include "render/gi/raytracing.h"
#include "render/gi/rcgi.h"
#include "render/gi/recon_path_tracer.h"
#include "render/gi/restir_di.h"
#include "render/gi/rt_instance_cull.h"
#include "render/gi/sdf_clipmap.h"
#include "render/gi/sdf_scene.h"
#include "render/gi/shadow.h"
#include "render/gi/shadow_trace.h"
#include "render/pipeline/gpu_cull.h"
#include "render/pipeline/material_system.h"
#include "render/pipeline/mesh_pipeline.h"
#include "render/pipeline/meshlet.h"
#include "render/pipeline/virtual_geometry.h"
#include "render/post/antialiasing.h"
#include "render/post/bloom.h"
#include "render/post/depth_of_field.h"
#include "render/post/exposure.h"
#include "render/post/frame_generation.h"
#include "render/post/motion_blur.h"
#include "render/post/overdraw.h"
#include "render/post/post.h"
#include "render/post/ui_blur.h"
#include "render/post/upscaler.h"
#include "render/post/vrs_rate.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"
#include "render/screenspace/ambient_occlusion.h"
#include "render/screenspace/reflection_trace.h"
#include "render/screenspace/ssao.h"
#include "render/screenspace/ssgi.h"
#include "render/screenspace/ssr.h"
#include "render/texturing/virtual_texture.h"
#include "render/util/gpu_profiler.h"

namespace rx::render {

// Backend-specific device requests for apps that record their own GPU passes
// through the FrameView scene hooks. Ignored on non-Vulkan backends. rx already
// enables the whole core / Vulkan 1.1-1.3 feature set the driver reports (so
// shaderInt8/16/64, storage 8/16-bit, scalar block layout,
// shaderDrawParameters, multiDrawIndirect, drawIndirectCount,
// fragmentStoresAndAtomics, samplerAniso are on when supported) plus
// mesh-shader and ray-query when present; this only covers extra device
// extensions rx does not itself request.
struct VulkanDeviceExtras {
  // Enabled if the adapter advertises them; the granted set is reported in
  // Renderer::caps()->extra_extensions.
  base::Vector<std::string> extensions;
};

struct RendererDesc {
  Backend backend = Backend::kAuto;
  bool enable_validation = false;
  AntiAliasingMode aa_mode = AntiAliasingMode::kTaa;
  UpscalerKind upscaler = UpscalerKind::kNone;
  RayTracingSettings raytracing;
  bool enable_raytracing = true;
  // Build the software-GI (SDF clipmap trace) infrastructure at startup. This
  // is an IMMUTABLE availability decision that MUST be set before Initialize:
  // the CPU mesh geometry that voxelises the SDFs is not retained past upload,
  // so the path cannot be backfilled by a later live toggle. Env RX_SDF /
  // RX_RCGI_SW, or a non-RT RX_RCGI request, force it on too; hosted apps use
  // this field for the programmatic equivalent (OnInitialize runs after
  // renderer init, too late).
  bool software_gi = false;
  VulkanDeviceExtras vulkan;
};

// Phase at which an app's own GPU pass is recorded into rx's frame. Selects the
// FrameView hook and the state guarantees the SceneHookContext documents.
enum class ScenePhase : u8 {
  kOpaque,      // after rx opaque+sky, before transparency/atmosphere resolve
  kTransparent, // after rx transparency, before post/tonemap
};

// Handed to a FrameView scene hook. Everything is at RENDER resolution (before
// upscaling and post). rx's render graph has already transitioned the color and
// depth images to their attachment layouts; the hook opens its OWN dynamic-
// rendering section (ctx.cmd->BeginRendering / EndRendering, LoadOp::kLoad to
// preserve rx's content) and may run compute (e.g. GPU culling) on ctx.cmd
// BEFORE beginning that section - a single hook does compute-then-raster, the
// app inserting its own MemoryBarrier between them for its own buffers. Raw
// Vulkan handles behind these come from render/rhi/vulkan_interop.h
// (GetVkImage / GetVkImageView / GetVkFormat). Hooks fire on the Vulkan backend
// only.
struct SceneHookContext {
  ScenePhase phase = ScenePhase::kOpaque;
  CommandList *cmd = nullptr;
  Device *device = nullptr;

  // Color target. kOpaque: rx's scene color (opaque + sky), HDR-linear RGBA16F.
  // kTransparent: the composited scene the app blends its translucents over.
  const GpuImage *color = nullptr;
  TextureView color_view;
  Format color_format = Format::kRGBA16Float;
  // Reversed-Z hardware depth (D32, clear 0 = far) with rx geometry already in
  // it. Depth-test GREATER_OR_EQUAL to interleave with rx geometry; writing it
  // (kOpaque) lets the app's geometry occlude rx's downstream draws too.
  const GpuImage *depth = nullptr;
  TextureView depth_view;
  Format depth_format = Format::kD32Float;
  // kOpaque only (null in kTransparent): the R32F depth copy rx's depth-aware
  // passes (sky/fog/aerial/SSAO/SSR) sample - the depth attachment itself is
  // never sampled (nvidia compression). Write it as a second color attachment
  // (SV_Position.z) so those passes respect the app's opaque geometry; skip it
  // and rx treats those pixels as background behind the effects.
  const GpuImage *depth_export = nullptr;
  TextureView depth_export_view;
  Format depth_export_format = Format::kR32Float;

  Extent2D extent{};  // render resolution
  u32 frame_slot = 0; // 0..frames_in_flight-1, index per-slot resources
  u32 frames_in_flight = 1;

  // Exactly the matrices rx uses this frame, column-major, UN-jittered (jitter
  // is applied in-shader). Under TAA/upscaling add the jitter in the vertex
  // shader (clip.xy += jitter * clip.w) to stay pixel-aligned with rx geometry;
  // leave it out otherwise.
  Mat4 view = Mat4::Identity();
  Mat4 proj = Mat4::Identity();
  Mat4 view_proj = Mat4::Identity();
  f32 jitter[2] = {0, 0}; // NDC units (already 2*pixel/dimension)
  f32 near_plane = 0.1f;  // reversed-Z, infinite far
  Vec3 camera_pos{};
};

struct CameraPose {
  Vec3 eye{0, 0, 3};
  Vec3 target{};
  f32 fov_y = 1.0472f; // 60 degrees
};

// What the simulation hands the renderer each frame. The engine extracts
// this from the ECS, keeping the renderer free of gameplay types. The
// previous frame's transform feeds motion vectors; for static or newly
// spawned objects it equals transform.
struct DrawItem {
  u64 mesh = 0; // AssetId hash of an uploaded mesh
  Mat4 transform = Mat4::Identity();
  Mat4 prev_transform = Mat4::Identity();
  // Index of this mesh's first bone in FrameView::bone_matrices, -1 = static.
  // Only meaningful for skinned meshes.
  i32 skin_offset = -1;
  // Range in FrameView::morph_weights holding this draw's active morph target
  // weights, -1 = none. Only meaningful for meshes with morph targets; apps
  // should skip zero weights so idle targets cost nothing.
  i32 morph_offset = -1;
  u32 morph_count = 0;
  // Packed rgb8 tint (0xRRGGBB) modulating this draw's albedo, 0 = untinted.
  // A per-instance tint the app can use to distinguish otherwise-identical
  // actors (e.g. team/faction colouring).
  u32 tint = 0;
  // Entity id written to the pick target on a RequestPick frame. 0 = not
  // pickable (the id readback returns 0 for background and unpickable draws).
  u32 pick_id = 0;
};

// A world-space debug line segment with a packed rgba8 (0xRRGGBBAA) color.
// Filled per frame into FrameView::debug_lines (depth-tested against the scene)
// or debug_lines_overlay (drawn on top). Editor gizmos, bounds and grids ride
// this path without the app recording its own GPU pass.
struct DebugLine {
  Vec3 a;
  Vec3 b;
  u32 rgba = 0xffffffff;
};

// The entity id under a requested pixel, read back from the pick target. See
// Renderer::RequestPick / TakePickResult.
struct PickResult {
  u32 pick_id = 0;
};

struct FrameView {
  CameraPose camera;
  f32 frame_delta_seconds = 1.0f / 60.0f; // upscalers want real frame time
  // World-space rect (min_x, min_z, max_x, max_z) covering the fully streamed
  // terrain cells. Distant terrain-LOD draws sink their vertices inside it so
  // the coarse proxy never bridges above the real land. All zeros = disabled.
  f32 detail_rect[4] = {0, 0, 0, 0};
  base::Vector<DrawItem> draws;
  // Projected decals this frame (world-space boxes, clustered with the lights).
  base::Vector<Decal> decals;
  // Dynamic omni lights this frame, accumulated in the forward lighting pass.
  base::Vector<PointLight> lights;
  // Bone palette for every skinned draw this frame, concatenated; each skinned
  // DrawItem indexes its run by skin_offset. Column-major model-space matrices.
  base::Vector<Mat4> bone_matrices;
  // Active morph target weights for every morphed draw this frame,
  // concatenated; each DrawItem indexes its run by morph_offset/morph_count.
  base::Vector<MorphWeight> morph_weights;
  // Live billboard particles for this frame (engine-simulated). Drawn lit and
  // soft-faded over the resolved scene before reconstruction.
  base::Vector<ParticleInstance> particles;
  bool particles_emissive =
      false; // route the set through HDR additive blending
  // gpu-simulated particle fountain: when count > 0, the renderer steps the
  // simulation on the gpu (compute) and draws it, instead of the cpu particles.
  u32 gpu_particle_count = 0;
  Vec3 gpu_particle_emitter{};
  u32 gpu_particle_mode = 0;         // 0 ember fountain, 1 fire
  f32 gpu_particle_radius = 0.3f;    // fire emitter disk radius
  f32 gpu_particle_intensity = 1.0f; // fire emissive scale
  // shell-fur ball: when enabled, the fur pass draws a fuzzy sphere here.
  bool fur_ball = false;
  Vec3 fur_position{};
  // order-independent transparency instances (wboit demo).
  base::Vector<WboitInstance> oit;
  // 3D gaussian splats: non-triangle primitives, projected and alpha blended
  // over the resolved scene.
  base::Vector<GaussianInstance> gaussians;
  // Object disturbances written into the persistent water field this frame:
  // boat wakes, bobbing props. Each injects a ripple impulse + foam splat at a
  // world position, scaled by the object's motion. Bounded; empty = no-op.
  base::Vector<WaterDisturbance> water_disturbances;
  // Optional heightfield fluid solver (settings.fluid_sim). When fluid_domain
  // is set the renderer steps the GPU water+lava sim over it this frame;
  // fluid_sources feeds springs/vents/drains (bounded, capped at 64). Null
  // domain leaves the sim idle. Non-owning: valid for the RenderFrame call.
  const FluidDomainDesc *fluid_domain = nullptr;
  base::Vector<FluidSource> fluid_sources;
  // Recorded inside the final ui pass with the backbuffer bound as the
  // color attachment. hud_draw (the libultragui HUD/menu) records first, then
  // ui_draw (the debug ImGui overlay) on top.
  std::function<void(CommandList &)> hud_draw;
  std::function<void(CommandList &)> ui_draw;

  // App-provided GPU passes recorded into the scene, depth-interleaved with
  // rx's own geometry (a game with its own GPU-driven pipeline: compute cull,
  // multi- draw-indirect, mesh/ray-query passes, ...). scene_opaque fires after
  // rx's opaque+sky and before transparency/atmosphere; scene_transparent after
  // rx transparency and before post/tonemap. Each runs inside a first-class
  // render- graph pass (so barriers are handled) and only when set, on the
  // Vulkan backend. Zero cost when unset: no pass is added. See
  // SceneHookContext.
  std::function<void(const SceneHookContext &)> scene_opaque;
  std::function<void(const SceneHookContext &)> scene_transparent;

  // Debug line lists for this frame (non-owning; valid for the RenderFrame
  // call). debug_lines are depth-tested against the resolved scene depth;
  // overlay lines draw on top. Both are drawn just before the UI pass. Empty =
  // no line pass.
  std::span<const DebugLine> debug_lines;
  std::span<const DebugLine> debug_lines_overlay;

  // Backdrop blur: when a frosted (backdrop-blur) widget is present, the UI
  // sets needs_blur so the renderer captures + blurs the backbuffer before the
  // ui pass and writes the result here for hud_draw to bind.
  // blur_source/sampler are filled by the renderer inside the ui pass, just
  // before hud_draw runs.
  bool needs_blur = false;
  // Filled by the renderer during the (const) frame record, hence mutable.
  mutable TextureView blur_source;
  mutable SamplerHandle blur_sampler;

  // Back to defaults, but the gather lists keep their capacity: a FrameView
  // held across frames (app::Host does) stops re-allocating every list every
  // frame. New container members must be added to the move-dance here.
  void Clear() {
    FrameView fresh;
    fresh.draws = std::move(draws);
    fresh.decals = std::move(decals);
    fresh.lights = std::move(lights);
    fresh.bone_matrices = std::move(bone_matrices);
    fresh.particles = std::move(particles);
    fresh.oit = std::move(oit);
    fresh.gaussians = std::move(gaussians);
    fresh.draws.clear();
    fresh.decals.clear();
    fresh.lights.clear();
    fresh.bone_matrices.clear();
    fresh.particles.clear();
    fresh.oit.clear();
    fresh.gaussians.clear();
    *this = std::move(fresh);
  }
};

class RX_RENDER_EXPORT Renderer {
public:
  Renderer();
  ~Renderer();

  bool Initialize(const RendererDesc &desc, Window &window);
  void RenderFrame(const FrameView &view);
  void Shutdown();
  void WaitIdle();

  // Android lifecycle: the surface is lost when the activity's window goes away
  // (background) and rebound when it returns. DestroySurface tears down the
  // swapchain + surface; RecreateSurface rebinds to the current window and
  // rebuilds the swapchain. RenderFrame is a no-op while the surface is gone.
  void DestroySurface();
  void RecreateSurface();
  bool has_surface() const { return swapchain_ != nullptr; }

  // Saves the next presented frame as png. Also armed by the
  // RX_SCREENSHOT env var ("path.png:seconds") for headless captures.
  void CaptureScreenshot(const std::string &path);

  // Editor picking. RequestPick arms an entity-id pass for the next frame that
  // rasterizes the opaque draw list into an R32_UINT target and reads back the
  // single pixel at (x, y) in output pixels. The result arrives asynchronously
  // (1-2 frames later); poll TakePickResult, which returns and clears the
  // pending result when it is ready. pick_id 0 means background/unpickable.
  void RequestPick(u32 x, u32 y);
  std::optional<PickResult> TakePickResult();

  // Makes a mesh drawable, keyed by its asset id. Materials referenced by
  // submeshes should be uploaded first. No-op without a device. id_salt
  // namespaces the mesh/BLAS key so two content domains with colliding asset
  // paths (e.g. two games that both ship "meshes/...") do not overwrite each
  // other; entities must carry the salted id in their Renderable. Zero (the
  // default/primary domain) leaves the key unchanged.
  bool UploadMesh(const asset::Mesh &mesh, u64 id_salt = 0);
  // Replaces only lod-0 vertices for a mesh uploaded with dynamic_vertices.
  // Topology and vertex count must match. The old buffer retires after
  // in-flight frames finish, so terrain brushes do not force a device-wide
  // idle. Like RemoveDynamicMesh, MUST be called between frames (app update),
  // never from inside RenderFrame: the retirement ring slot math assumes
  // frame_index_ has not yet advanced past the frame being recorded.
  bool UpdateDynamicMesh(const asset::Mesh &mesh, u64 id_salt = 0);
  // Restores RT participation after a batch of live dynamic updates. This can
  // submit a BLAS build, so editors call it once at stroke boundaries.
  bool SyncDynamicMeshRayTracing(const asset::Mesh &mesh, u64 id_salt = 0);
  // Retires a dynamic mesh's GPU buffers after in-flight frames complete.
  // Static meshes may own RT/SDF registrations and are intentionally rejected.
  bool RemoveDynamicMesh(asset::AssetId mesh, u64 id_salt = 0);
  // Persistent, opaque static instance groups. A group is mesh-homogeneous and
  // should cover one spatial streaming unit (for example one world cell), which
  // gives the renderer group-level frustum culling and one hardware-instanced
  // draw per material/LOD instead of one draw and ECS entity per placement.
  // Updates preserve object motion by treating overlapping array indices as
  // stable identities; newly appended indices spawn with zero object velocity.
  InstanceGroupHandle CreateInstanceGroup(u64 mesh,
                                          std::span<const Mat4> transforms);
  bool UpdateInstanceGroup(InstanceGroupHandle handle,
                           std::span<const Mat4> transforms);
  void DestroyInstanceGroup(InstanceGroupHandle handle);
  // Same per-domain salt as UploadMesh; it must match so a mesh's submesh
  // material references resolve to this domain's materials/textures.
  bool UploadTexture(const asset::Texture &texture, u64 id_salt = 0);
  bool UploadMaterial(const asset::Material &material, u64 id_salt = 0);
  // Builds + uploads a mesh for the mesh-shader meshlet path (the --demo
  // meshlet scene draws it instead of the normal raster geometry).
  void UploadMeshletMesh(const asset::Mesh &mesh);
  // Builds the cluster-DAG LOD hierarchy and activates the virtual-geometry
  // demo pass (--demo vgeo).
  void UploadVirtualGeometryMesh(const asset::Mesh &mesh);
  // World transforms the virtual-geometry mesh draws with (default: one
  // identity instance). The gpu culls every cluster of every instance.
  void SetVirtualGeometryInstances(std::span<const Mat4> transforms);
  // Planar world-xz-projected albedo for the virtual-geometry resolve: a full
  // RGBA8 mip chain (size x size at mip 0, levels concatenated).
  void SetVirtualGeometryAlbedo(ByteSpan rgba_mips, u32 size, f32 world_to_uv);
  // Interior volumes for RCGI leak hardening (Phase 3 item 9b): world-space AABBs
  // the game forwards (interior cell bounds / building interiors). RCGI classifies
  // probes and gather samples indoor/outdoor against these and refuses to blend
  // across the boundary, killing the outdoor-probe-through-a-doorway leak. Cheap;
  // forward every frame or on change. Empty span disables classification.
  void SetInteriorVolumes(std::span<const InteriorVolume> volumes);
  // Seeds simulated hair strands on a head sphere (--demo strands).
  void SeedHairStrands(const Vec3 &head_center, f32 head_radius, u32 strands,
                       f32 length);
  // Builds simulated guide strands from a real hair mesh and places the groom
  // via `transform` (later: a head bone). Returns a handle (0 = failure). The
  // groom-local frame has the scalp at the origin, engine units, Y-up.
  u32 CreateHairGroom(const asset::Mesh &hair_mesh, const GroomParams &params,
                      const Mat4 &transform);
  // Same, from an already-built groom (procedural test grooms, callers that
  // also feed the groom data to the physics strand sim).
  u32 CreateHairGroom(const GroomData &data, const GroomParams &params,
                      const Mat4 &transform);
  void SetHairGroomTransform(u32 id, const Mat4 &transform);
  // This frame's simulated node positions (world xyz, strand-major), read
  // back from the physics strand groom; see app::HairStrandBinding.
  void SetHairGroomPoints(u32 id, const f32 *positions, u32 count);
  void SetHairGroomTint(u32 id, const Vec3 &tint);
  void DestroyHairGroom(u32 id);
  // World-space head collision sphere of a groom, for aligning a head mesh.
  bool HairGroomHead(u32 id, Vec3 *center, f32 *radius);
  // Bakes an octahedral imposter of the mesh and sets the distant instances
  // drawn as billboards (--demo imposters).
  void BakeImposter(const asset::Mesh &mesh,
                    std::span<const ImposterPass::Instance> instances);

  // Live tunables. Mutate freely; RenderFrame diffs against the applied
  // state and reconfigures, including full upscaler swaps.
  RenderSettings &settings() { return settings_; }
  // Points the clustered decal system at an uploaded texture (the atlas).
  void SetDecalAtlas(asset::AssetId texture, asset::AssetId normal_atlas = {});

  const DeviceCaps *caps() const;
  Device *device() { return device_.get(); }
  Format swapchain_format() const;
  u32 swapchain_image_count() const;
  u32 render_width() const { return render_width_; }
  u32 render_height() const { return render_height_; }
  u32 output_width() const { return output_width_; }
  u32 output_height() const { return output_height_; }
  bool upscaler_active() const { return upscaler_ != nullptr; }
  // True when RX_RCGI was set on the command line/env: hosted presets must let
  // it win in both directions (force on OR force off) over the tier default.
  bool rcgi_env_overridden() const { return rcgi_env_overridden_; }
  u32 mesh_count() const { return static_cast<u32>(meshes_.size()); }
  size_t instance_group_count() const { return instances_.group_count(); }
  size_t instance_count() const { return instances_.instance_count(); }
  const MaterialSystem *materials() const { return material_system_.get(); }

  // Per-pass GPU timings from the last resolved frame, for the debug overlay.
  const base::Vector<GpuProfiler::PassTiming> &pass_timings() const {
    return profiler_.results();
  }
  f32 gpu_frame_ms() const { return profiler_.total_ms(); }
  // Dynamic-resolution factor currently applied on top of render_scale
  // (1 while the controller is off or inactive).
  f32 dynamic_resolution_scale() const { return applied_dynamic_scale_; }
  u32 path_trace_samples() const { return path_tracer_.accumulated_samples(); }

  // Last compiled frame graph, for the debug inspector (passes, transient
  // resources, barrier and memory totals).
  const RenderGraph::Stats &graph_stats() const { return graph_.stats(); }
  // Stats collection costs per-pass string copies each frame; the debug UI
  // turns it on only while its inspector is visible.
  void set_graph_stats_enabled(bool enabled) {
    graph_.set_stats_enabled(enabled);
  }
  // Drops app-provided pass callbacks retained by the last compiled frame.
  // Call only after the device is idle.
  void ClearFrameCallbacks();

  // Opaque indirect draw counts for the debug overlay: total submitted vs the
  // count that survived gpu frustum culling (one frame stale).
  u32 draws_total() const { return cull_total_commands_; }
  u32 draws_visible() const { return cull_visible_; }

  // Mesh-shader meshlet counts (0 total when no meshlet mesh is loaded): total
  // clusters vs the count that survived gpu frustum + cone cluster culling.
  u32 meshlets_total() const { return meshlet_.meshlet_count(); }
  u32 meshlets_visible() const { return meshlet_visible_; }

private:
  static constexpr u32 kFramesInFlight = Device::kMaxFramesInFlight;
  static constexpr Format kSceneColorFormat = Format::kRGBA16Float;
  static constexpr Format kMotionFormat = Format::kRG16Float;
  // Oct normal in rg, material roughness in b (denoiser guides + the
  // reflection trace need real roughness), a free.
  static constexpr Format kNormalFormat = Format::kRGBA16Float;
  static constexpr Format kDepthFormat = Format::kD32Float;

  // Per frame-in-flight host-visible buffers. Command recording, sync and the
  // transient descriptor pools live inside the rhi Device's frame ring.
  struct FrameResources {
    GpuBuffer globals; // host visible FrameGlobals
    GpuBuffer
        bone_palette; // host visible skinning matrices, read by device address
    GpuBuffer
        morph_weights; // host visible MorphWeight pairs, read by device address
    GpuBuffer lights;  // host visible PointLight array
    GpuBuffer decals;  // host visible Decal array
  };
  // Max bones across all skinned draws in one frame.
  static constexpr u32 kMaxFrameBones = 8192;
  // Max active morph target weights across all morphed draws in one frame.
  static constexpr u32 kMaxFrameMorphWeights = 4096;
  static constexpr u32 kMaxFrameLights = 256;

  bool CreateFrameResources();
  void DestroyFrameResources();
  void RecreateSwapchain();
  // Whether the swapchain should request an HDR format: the hdr_output setting
  // gated on the OS actually compositing the window in HDR (Window::
  // hdr_enabled). A capable-but-disabled display keeps SDR.
  bool WantHdrSwapchain() const;
  void UpdateRenderResolution();
  void ResizeSizedPasses();
  void ApplySettings();
  bool CreateUpscalerForSettings();
  void BuildFrameGraph(FrameResources &frame, u32 image_index,
                       const FrameView &view);
  // Records the frame's opaque casters depth-only with ShadowPass's caster
  // pipelines (static/skinned/instanced, masked + opaque variants). Shared by
  // the sun cascade render and the precipitation sky-occlusion map.
  void RecordDepthOnlyScene(CommandList &cmd, const Mat4 &light_view_proj,
                            const FrameResources &frame, const FrameView &view);
  // Builds the blas + bindless geometry for grass-like (no_rt) meshes uploaded
  // while path tracing was off, so enabling it later still gets the
  // alpha-tested foliage into the tlas. Idempotent (skips already-built
  // meshes).
  bool EnsureRayTracingGeometry();
  // Lazily builds the BLAS + bindless mesh record for a non-zero distance LOD
  // of an already-uploaded RT mesh (RX_RT_LOD_NEAR). Idempotent; returns the
  // LOD's bindless index (custom_index for the TLAS instance) or kInvalidIndex
  // when the LOD has no RT geometry / cannot be built (caller falls back to
  // LOD0). Called at frame-build time, so a one-time build stall is acceptable.
  u32 EnsureLodRtGeometry(u64 mesh_key, GpuMesh &mesh, u32 lod);

  RendererDesc desc_;
  RenderSettings settings_;
  Window *window_ = nullptr;
  // The HDR request the current swapchain was built with; when WantHdrSwapchain
  // diverges (OS toggle flipped, setting changed) the frame loop rebuilds.
  bool swapchain_hdr_request_ = false;
  std::unique_ptr<Device> device_;
  std::unique_ptr<Swapchain> swapchain_;
  std::unique_ptr<TransientPool> transient_pool_;
  std::unique_ptr<BindlessRegistry> bindless_;
  base::Vector<u32> retired_bindless_meshes_[kFramesInFlight];
  std::unique_ptr<MaterialSystem> material_system_;
  std::unique_ptr<EnvironmentSystem> environment_;
  std::unique_ptr<DdgiSystem> ddgi_;
  std::unique_ptr<RcgiSystem>
      rcgi_; // idTech8-style radiance-cached GI (RX_RCGI), lazily created
  bool rcgi_create_failed_ = false; // lazy creation failed once; do not retry
  bool rcgi_sw_unavailable_logged_ =
      false; // logged the "no startup SDF path" notice once
  bool rcgi_env_overridden_ =
      false;             // RX_RCGI was set explicitly (wins over preset both ways)
  LightGrid light_grid_; // world-space light grid feeding the rcgi cache
  base::Vector<InteriorVolume>
      interior_volumes_; // forwarded to rcgi each active frame (item 9b)
  // SDF software-trace infrastructure (RX_SDF / software_gi): per-mesh SDFs +
  // global clipmap. Both null unless the path was enabled at startup, so with
  // it off nothing is generated/allocated. `sdf_available_` is the IMMUTABLE
  // startup availability bit, decided once in Initialize and gated on creation
  // success -- separate from any live RenderSettings toggle, so applying a
  // quality preset can never turn the seeded software path off (see
  // RendererDesc::software_gi).
  bool sdf_available_ = false;
  std::unique_ptr<SdfScene> sdf_scene_;
  std::unique_ptr<SdfClipmap> sdf_clipmap_;
  std::unique_ptr<WaterPass> water_;
  std::unique_ptr<FluidSurfacePass> fluid_surface_;
  std::unique_ptr<MeshPipeline> mesh_pipeline_;
  std::unique_ptr<PostPass> post_;
  std::unique_ptr<UiBlurPass>
      ui_blur_; // frosted-glass backdrop blur for the UI
  base::UnorderedMap<u64, GpuMesh> meshes_;
  FrameResources frames_[kFramesInFlight];
  // Per-slot persistent sets, rewritten each frame once the slot's fence fired:
  // frame globals (uniform + tlas + hi-z) and the two environment-set variants
  // (the scene and transparent passes bind different ao / opaque-color views).
  BindingSetHandle globals_sets_[kFramesInFlight];
  BindingSetHandle env_scene_sets_[kFramesInFlight];
  BindingSetHandle env_transparent_sets_[kFramesInFlight];
  BindingSetHandle env_prepass_sets_[kFramesInFlight]; // dummies + ocean maps
  std::unique_ptr<Upscaler> upscaler_;
  // FSR3 frame generation (RX_FRAMEGEN): lazily created when the FSR3
  // upscaler is active (its dilated guides are reused); the present-rate
  // counters feed the periodic log line.
  std::unique_ptr<FrameGenerator> framegen_;
  bool framegen_attempted_ = false;
  bool framegen_was_active_ = false;
  bool fg_active_frame_ =
      false; // this frame interpolates; BuildFrameGraph adds the hudless copy
  u32 fg_presents_ = 0;
  u32 fg_engine_frames_ = 0;
  f64 fg_log_time_ = 0.0;
  std::unique_ptr<RayTracingContext> raytracing_;
  // Solid-angle + distance culling of realtime TLAS instances, persistent
  // across frames (time-sliced sweep state per instance group).
  RtInstanceCuller rt_cull_;
  RenderGraph graph_;
  TaaPass taa_;
  RtaoPass rtao_;
  ReflectionTrace reflection_trace_;
  MotionBlurPass motion_blur_;
  DepthOfFieldPass dof_;
  LocalShadows local_shadows_;
  FroxelFog froxel_fog_;
  bool local_shadows_active_ = false; // faces assigned this frame
  VrsRatePass vrs_;
  RestirDi restir_di_;
  VirtualTexture virtual_texture_;
  bool vrs_active_ = false; // rate image attached to this frame's scene pass
  PipelineHandle light_cluster_pipeline_;
  PipelineHandle contact_shadow_pipeline_;
  PipelineHandle cloud_shadow_pipeline_;
  PipelineHandle sss_pipeline_;
  SamplerHandle sss_sampler_;
  GpuBuffer cluster_counts_;
  GpuBuffer cluster_indices_;
  GpuBuffer decal_cluster_indices_;
  // Decal atlas: set once by the engine/demo via SetDecalAtlas (asset id of an
  // uploaded texture); empty binds white.
  TextureView decal_atlas_view_;
  TextureView decal_normal_atlas_view_;
  SsaoPass ssao_;
  SsrPass ssr_;
  SsgiPass ssgi_;
  ShadowPass shadow_;
#if defined(RX_HAS_NRD)
  NrdDenoiser nrd_;
  ShadowTracePass shadow_trace_;
#endif
#if defined(RX_HAS_DLSS)
  // DLSS Ray Reconstruction: learned denoiser replacing the SVGF chain in the
  // recon path-traced mode when the dlssd snippet is available. Lazy-init on
  // first use (its feature memory is not free).
  RrDenoiser rr_;
  bool rr_init_attempted_ = false;
#endif
  BloomPass bloom_;
  ExposurePass exposure_;
  GpuProfiler profiler_;
  PathTracer path_tracer_;
  ReconPathTracer recon_path_tracer_;
  VolumetricFog volumetric_fog_;
  AerialPerspective aerial_perspective_;
  Clouds clouds_;
  // The opt-in textured cloud model. Heavier resources (3D noise bakes, the
  // half-res history) than clouds_, so it initializes lazily on the first
  // frame RenderSettings::cloudscape is set.
  Cloudscape cloudscape_;
  bool cloudscape_init_tried_ = false;
  bool cloudscape_ready_ = false;
  Precipitation precipitation_;
  PrecipOcclusion precip_occlusion_;
  PrecipVolume precip_volume_;
  LightningSystem lightning_;
  bool precip_occlusion_active_ = false;  // sky map valid + consumers may sample it
  SurfaceWeather surface_weather_;
  ParticleSystem particles_;
  // CPU pools for the NIF particle emitters (fires, smoke, mist), fed from
  // mesh_emitters_ by the draw list each frame. No GPU state to shut down.
  ParticleEmitterSim emitter_sim_;
  base::UnorderedMap<u64, base::Vector<asset::ParticleEmitter>> mesh_emitters_;
  GaussianSplat gaussians_;
  FurPass fur_;
  WboitPass wboit_;
  OverdrawPass overdraw_;
  GpuCull gpu_cull_;
  MeshletPass meshlet_;
  VirtualGeometryPass vgeo_;
  HairStrands hair_;
  OceanFft ocean_;
  WaterField water_field_;
  FluidSim fluid_sim_;
  ShoreWetting shore_wetting_;
  WaterCaustics water_caustics_;
  ImposterPass imposters_;
  InstanceStore instances_;
  bool fft_ocean_active_ = false;     // maps valid + flag set this frame
  bool water_field_active_ = false;   // ring field valid + flag set this frame
  bool fluid_sim_active_ = false;     // fluid solver configured + domain this frame
  bool shore_wetting_active_ = false; // shore wetting field valid this frame
  bool water_caustics_active_ =
      false;              // caustic map valid + flag set this frame
  GpuImage ms_dummy_hiz_; // 1x1 fallback bound to the mesh-shader cull when
                          // occlusion is off
  Mat4 pt_prev_view_proj_ = Mat4::Identity();
  f32 pt_prev_sig_ = 0; // lighting signature; change resets accumulation
  u64 scene_revision_ = 0;
  u64 pt_prev_scene_revision_ = 0;
  bool pt_was_active_ = false;
  // Which path-trace mode ran last frame (0 reference, 1 nrd-denoised, 2 recon,
  // -1 none). Switching mode must reset accumulation: each mode reprojects its
  // own history buffers, which the other modes never wrote.
  int pt_prev_mode_ = -1;
  // A no_rt (foliage) mesh was uploaded while path tracing was off, so it has
  // no blas yet; EnsureRayTracingGeometry catches it up when path tracing turns
  // on.
  bool rt_foliage_dirty_ = false;
  bool rt_geometry_dirty_ = false;

  // Settings already in effect, diffed against settings_ each frame.
  UpscalerKind applied_upscaler_ = UpscalerKind::kNone;
  UpscalerQuality applied_quality_ = UpscalerQuality::kQuality;
  f32 applied_render_scale_ = 1.0f;
  // Dynamic resolution: the controller decides drs_.scale(), the applied copy
  // is what the current targets were sized with; diverging triggers the same
  // resize path as a render_scale change.
  DynamicResolution drs_;
  f32 applied_dynamic_scale_ = 1.0f;
  // kMsaa: the sample count the mesh pipelines were built with (1 = the
  // standard single-sampled path). Diverging from the settings-derived value
  // rebuilds them through a device idle, like an upscaler swap.
  u32 applied_msaa_samples_ = 1;
  PipelineHandle msaa_resolve_pipeline_; // sample-0 guide resolve (compute)
  PipelineHandle depth_copy_pipeline_;   // rebuilds 1x hw depth post-resolve
  AntiAliasingMode applied_aa_ = AntiAliasingMode::kTaa;
  bool applied_vsync_ = false;
  // Sun state baked into the environment maps; differing means regenerate.
  Vec3 applied_sun_direction_{};
  f32 applied_sun_intensity_ = -1;
  Vec3 applied_sun_color_{};
  bool environment_dirty_ = true;
  // Last frame's aurora bake strength; a fade to zero re-bakes once so the
  // sky/IBL do not keep the final green cubemap after the aurora turns off.
  f32 prev_env_aurora_ = 0.0f;

  // Editor debug-line pass: a line-list pipeline (lazily built) drawing
  // FrameView::debug_lines/overlay from per-frame host-visible vertex buffers.
  void BuildDebugLinePipelines();
  void DrawDebugLines(CommandList &cmd, const FrameView &view,
                      const Mat4 &view_proj, Extent2D extent);
  PipelineHandle debug_line_pipeline_;         // depth-tested
  PipelineHandle debug_line_overlay_pipeline_; // always on top
  GpuBuffer debug_line_vbo_[kFramesInFlight];  // host-visible, one per slot
  u32 debug_line_vbo_capacity_[kFramesInFlight] = {}; // in vertices

  // Editor picking: an R32_UINT id pass over the opaque draws, read back at the
  // requested pixel. A request arms the id pass for the next rendered frame;
  // the readback is synchronous within that frame and the result is queued for
  // TakePickResult (a rare editor operation, so the stall is acceptable).
  void RenderPickPass(const FrameView &view);
  bool pick_requested_ = false;
  u32 pick_x_ = 0, pick_y_ = 0;
  bool pick_result_ready_ = false;
  u32 pick_result_id_ = 0;
  PipelineHandle pick_pipeline_;
  GpuImage pick_id_image_;    // R32_UINT, render resolution
  GpuImage pick_depth_image_; // D32, render resolution
  u32 pick_image_w_ = 0, pick_image_h_ = 0;

  void WriteBackbufferPng(const std::string &path, u32 image_index);
  void WriteScreenshot(u32 image_index);
  void DumpFgImage(const GpuImage &image, ResourceState state, bool bgra,
                   const char *path);
  void WriteHdr(); // reads back the captured linear hdr buffer to a .hdr file

  std::string screenshot_path_;
  f64 screenshot_at_ = -1; // seconds; <0 means immediately when armed

  // Frame-burst capture (RX_SEQ=prefix:startsec:count[:stride]) for stitching
  // an animation clip from the inbuilt framebuffer capture.
  std::string seq_prefix_;
  f64 seq_at_ = -1;
  int seq_count_ = 0;
  int seq_written_ = 0;
  int seq_stride_ = 1;
  int seq_frame_ctr_ = 0;

  // Linear-hdr frame export (radiance .hdr). RX_HDR=<path>[:seconds].
  std::string hdr_path_;
  f64 hdr_at_ = -1;
  bool hdr_pending_ =
      false; // the copy pass ran this frame; read it back after submit
  u32 hdr_width_ = 0, hdr_height_ = 0;
  GpuBuffer hdr_readback_; // host-visible rgba32f, one float4 per pixel
  PipelineHandle hdr_pipeline_;
  Mat4 prev_view_proj_ = Mat4::Identity();
  Mat4 prev_view_ = Mat4::Identity();
  Mat4 prev_proj_ = Mat4::Identity();
  f32 prev_jitter_[2] = {0, 0};
  f64 time_seconds_ = 0;
  bool has_prev_frame_ = false;
  bool rt_available_ = false;
  bool rcgi_force_software_ =
      false; // RX_RCGI_SW: force the SDF software tracer
  u32 frame_index_ = 0;
  u32 cull_total_commands_ = 0; // opaque indirect draws this frame
  u32 cull_visible_ = 0; // survivors from the last completed cull (fence-safe)
  u32 meshlet_visible_ =
      0; // survivors of the last meshlet cluster cull (fence-safe)
  u32 render_width_ = 0;
  u32 render_height_ = 0;
  u32 output_width_ = 0;
  u32 output_height_ = 0;
};

} // namespace rx::render

#endif // RX_RENDER_RENDERER_H_
