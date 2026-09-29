#include "rxe/render/core/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/check.h>
#include <base/option.h>

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "base/time/time.h"
#include "foundation/algorithm/sort.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include "rxe/asset/primitives.h"
#include "rxe/asset/texture_compress.h"
#include "rxe/asset/exr_write.h"
#include "rxe/render/core/renderer_internal.h"

namespace rx::render {
namespace internal {

// Alpha-tested vegetation in the rays (AC Shadows opaque-approximation).
// RX_RT_VEG shrinks each masked mesh's realtime stand-in by its baked average
// opacity so realtime diffuse GI / AO / shadow rays get correct-on-average
// foliage occlusion; 0 forces the stand-in to full size (today's force-opaque
// behavior). RX_RT_VEG_ANYHIT switches specular reflections to a bounded
// real-alpha any-hit test (the approximation reads wrong in sharp reflections).
base::Option<bool> RtVegOpt{"rt.veg", true, "RX_RT_VEG"};

} // namespace internal
namespace {

// Render timed captures into an offscreen image instead of the presented
// backbuffer. Forced on automatically when the compositor starves the
// swapchain; set explicitly for deterministic captures on a headless or
// unattended desktop, where the window may never be composited.
base::Option<bool> CaptureOffscreen{"screenshot.offscreen", false,
                                    "RX_CAPTURE_OFFSCREEN"};

// Probe-based diffuse GI, on by default; an override makes it bisectable
// when indirect light is suspected of tinting a scene.
base::Option<bool> DdgiOpt{"ddgi", true, "RX_DDGI"};

// A/B debug: use the M1 per-pixel cascade resolve instead of the M2 gather
// chain.
base::Option<bool> RcgiProbesOnlyOpt{"rcgi.probes_only", false,
                                     "RX_RCGI_PROBES_ONLY"};

// Phase 3 leak/occlusion hardening. All default on; each isolates one fix for
// A/B (interior ambient miss + volume classify; probe relocation; probe AO).
base::Option<bool> RcgiInteriorOpt{"rcgi.interior", true, "RX_RCGI_INTERIOR"};

base::Option<bool> RcgiRelocateOpt{"rcgi.relocate", true, "RX_RCGI_RELOCATE"};

base::Option<bool> RcgiProbeAoOpt{"rcgi.probe_ao", true, "RX_RCGI_PROBE_AO"};

// RCGI final-gather resolution scale: 2 = half res (default), 4 = quarter res
// (opt-in; AC Shadows shipped quarter-res diffuse on consoles). The denoise
// radius widens automatically at quarter to keep the cornell/interior clean.
base::Option<int> RcgiGatherScaleOpt{"rcgi.gather_scale", 2,
                                     "RX_RCGI_GATHER_SCALE"};

// Material-ID denoiser mask (item 22): reject cross-class neighbours in the
// RCGI spatial/temporal filters. On by default; env for A/B.
base::Option<bool> RcgiDenoiseMaskOpt{"rcgi.denoise_mask", true,
                                      "RX_RCGI_DENOISE_MASK"};

base::Option<int> SdfDebugOpt{"sdf.debug", 0, "RX_SDF_DEBUG"};

base::Option<float> VgeoError{"vgeo.error", 1.0f, "RX_VGEO_ERROR"};

// 0 shaded, 1 cluster tint, 2 lod tint, 3 sw/hw raster path.
base::Option<int> VgeoDebug{"vgeo.debug", 1, "RX_VGEO_DEBUG"};

base::Option<bool> RtVegAnyHitOpt{"rt.veg.anyhit", true, "RX_RT_VEG_ANYHIT"};

// Phase 4 specular reflection quality/perf levers (AC Shadows adoption).
// Raytraced specular for opaque surfaces; overridable so it can be bisected
// against the rest of the ray tracing path.
base::Option<bool> ReflOpt{"refl", true, "RX_REFL"};

base::Option<bool> RtaoOpt{"rtao", true, "RX_RTAO"};

base::Option<bool> ReflHalfOpt{"refl.half", true, "RX_REFL_HALF"};

base::Option<bool> ReflShSkipOpt{"refl.sh_skip", true, "RX_REFL_SH_SKIP"};

base::Option<float> ReflShSkipRough{"refl.sh_skip.rough", 0.45f,
                                    "RX_REFL_SH_SKIP_ROUGH"};

base::Option<bool> ReflFogOpt{"refl.fog", true, "RX_REFL_FOG"};

// RT scene scalability (AC Shadows §2). RX_RT_CULL drops small distant
// instances from the realtime TLAS by projected solid angle beyond
// RX_RT_CULL_START metres, threshold RX_RT_CULL_ANGLE (angular radius). RX_RT_
// LOD_NEAR keeps LOD0 in the rays within that radius (raster/RT agree in the
// screen-trace range) and lets distance LODs into the BLAS past it. RX_RT_
// ASYNC_TLAS builds the per-frame TLAS on the async compute queue. All are
// realtime-only: the path tracer keeps every instance at LOD0.
base::Option<bool> RtCullOpt{"rt.cull", true, "RX_RT_CULL"};

base::Option<float> RtCullAngle{"rt.cull.angle", 0.004f, "RX_RT_CULL_ANGLE"};

base::Option<float> RtCullStart{"rt.cull.start", 40.0f, "RX_RT_CULL_START"};

base::Option<float> RtLodNear{"rt.lod.near", 64.0f, "RX_RT_LOD_NEAR"};

base::Option<bool> RtAsyncTlasOpt{"rt.async.tlas", true, "RX_RT_ASYNC_TLAS"};

// RX_RT_SKIN=0 drops every skinned actor back out of the acceleration
// structure, which is what the engine did before compute skinning existed. The
// A/B is how the path's real cost is measured: everything else about the frame
// is unchanged, so the difference is the dispatch, the refit and the extra
// instance.
base::Option<bool> RtSkinOpt{"rt.skin", true, "RX_RT_SKIN"};

// Debug/verification only: force the ripple obstacle boundary off while leaving
// the shoreline-wetting shading on, so an A/B isolates ripples reflecting off
// the island from the wet-sand shading. Not wired to the ini/UI on purpose.
base::Option<bool> WaterObstacleOpt{"water.obstacle", true,
                                    "RX_WATER_OBSTACLE"};

// Debug: horizontal fake velocity in pixels, to exercise the blur from a
// static camera (screenshot testing).
base::Option<double> MotionBlurDebugVel{"motion.blur.debug.vel", 0.0,
                                        "RX_MOTION_BLUR_DEBUG_VEL"};

// Debug: force the tonemap's output transfer (1 pq, 2 scrgb) on an SDR
// swapchain, so the encode math is testable on displays with no HDR path.
base::Option<int> HdrForceTransfer{"hdr.force.transfer", 0,
                                   "RX_HDR_FORCE_TRANSFER"};

// Distance-based hierarchical lod: coarser geometry the further a mesh is from
// the camera. Switches roughly every few bounding radii; clamps to the
// coarsest.
u32 SelectLod(const gpu::GpuMesh &mesh, f32 distance) {
  u32 lod_count = 1u + static_cast<u32>(mesh.lods.size());
  if (lod_count <= 1)
    return 0;
  f32 unit = rx::Max(mesh.bounds_radius, 0.25f) * 2.5f;
  u32 lod = static_cast<u32>(distance / rx::Max(unit, 0.5f));
  return lod < lod_count ? lod : lod_count - 1;
}

f32 InstanceGroupDistance(const InstanceStore::Group &group, const Vec3 &eye) {
  const Vec3 delta = eye - group.bounds_center;
  return rx::Max(::sqrtf(Dot(delta, delta)) - group.bounds_radius, 0.0f) /
         group.lod_scale;
}

// Gribb-Hartmann frustum planes (left,right,bottom,top,near) from a
// column-major view_proj, normalized so a point is inside when dot(n,p)+d >= 0.
// Far is skipped.
void ExtractFrustumPlanes(const Mat4 &vp, f32 out[5][4]) {
  const f32 *m = vp.m;
  auto row = [&](int r, int c) { return m[c * 4 + r]; };
  f32 p[5][4] = {
      {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2),
       row(3, 3) + row(0, 3)},
      {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2),
       row(3, 3) - row(0, 3)},
      {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2),
       row(3, 3) + row(1, 3)},
      {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2),
       row(3, 3) - row(1, 3)},
      {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
  };
  for (int i = 0; i < 5; ++i) {
    f32 len =
        ::sqrtf(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
    if (len < 1e-8f)
      len = 1.0f;
    for (int c = 0; c < 4; ++c)
      out[i][c] = p[i][c] / len;
  }
}

// World-space sphere vs the normalized frustum planes; outside if it falls
// beyond any plane. Lets the cpu skip a draw entirely (no dispatch) for
// off-screen instances, so cpu cost tracks visible instances rather than
// streamed ones.
bool SphereOutsideFrustum(const f32 planes[5][4], const Vec3 &c, f32 r) {
  for (int i = 0; i < 5; ++i) {
    if (planes[i][0] * c.x + planes[i][1] * c.y + planes[i][2] * c.z +
            planes[i][3] <
        -r) {
      return true;
    }
  }
  return false;
}

} // namespace

void Renderer::RenderFrame(const FrameView &view) {
  static const MemoryCategory kRenderCategory = RegisterMemoryCategory("render");
  MemoryCategoryScope mem_scope(kRenderCategory);
  if (!device_ || device_->is_stub() || !swapchain_)
    return;

  // Advance the frame clock before anything can bail out of the frame. The
  // timed captures and the rate-limited logs below key off it, so leaving it
  // until after the acquire froze time whenever frames were skipped, a
  // screenshot armed for t=45s then never came due.
  time_seconds_ += view.frame_delta_seconds;

  // Queue the frame's decal stamps before anything can bail out too. Unlike the
  // other FrameView lists these are one-shot EVENTS, not per-frame state: the
  // app will not resubmit them, so a stamp landing on a frame that skips
  // (swapchain out of date, acquire timeout) would be lost for good. Queuing
  // only appends to the receiver's journal; the bake still happens in the graph.
  for (const DecalStamp &stamp : view.decal_stamps)
    decal_baker_.Stamp(stamp);

  // Wayland surfaces report an undefined currentExtent, so the driver never
  // flags the swapchain out-of-date on a window resize (unlike X11). Poll the
  // window each frame and recreate when it no longer matches the swapchain, so
  // the rendered output tracks the window the same way imgui's DisplaySize does
  // - otherwise the overlay is drawn/hit-tested against a stale size and clicks
  // stop landing on widgets after a resize.
  if (window_) {
    u32 w = window_->width(), h = window_->height();
    if (w != 0 && h != 0 && (w != output_width_ || h != output_height_))
      RecreateSwapchain();
    // The effective HDR request tracks the live OS state (system toggle
    // flipped, window moved to an SDR monitor, hdr_output changed): rebuild
    // when it diverges from what the current swapchain was built with.
    // Comparing against the request (not the achieved color space) avoids a
    // recreate loop when the surface cannot satisfy it.
    if (WantHdrSwapchain() != swapchain_hdr_request_)
      RecreateSwapchain();
  }

  ApplySettings();
  if (view.camera_cut) {
    has_prev_frame_ = false;
    pt_was_active_ = false;
    taa_.Reset();
  }

  u32 slot = frame_index_ % kFramesInFlight;
  // Waits on the slot's fence, resets its command allocator and transient
  // descriptor pool, and begins recording.
  gpu::CommandList *cmd = device_->BeginFrame(slot);
  if (bindless_) {
    for (u32 index : retired_bindless_meshes_[slot])
      bindless_->ReleaseMesh(index);
    retired_bindless_meshes_[slot].clear();
  }

  u32 image_index = 0;
  // Offscreen: drive the whole capture run off the swapchain, so the result
  // never depends on the compositor ever showing the window. Requested up
  // front, or latched once an acquire timed out below; retrying it every
  // frame would burn the full timeout each time, and the run needs its
  // warm-up frames. Acquiring here would be wrong as well as pointless:
  // nothing presents the image back, so the swapchain would run dry.
  // A windowless renderer has no surface at all, so every one of its frames
  // goes this way whether or not a capture is due: the warm-up frames still
  // have to run, or the one capture that is due comes out black.
  if ((offscreen_only_ || ((CaptureOffscreen.get() || swapchain_starved_) &&
                           CaptureArmed())) &&
      EnsureCaptureImage())
    capture_offscreen_ = true;
  if (offscreen_only_ && !capture_offscreen_)
    return; // no capture image, nowhere to render
  gpu::AcquireResult acquired =
      capture_offscreen_ ? gpu::AcquireResult::kOk : swapchain_->Acquire(slot, &image_index);
  if (acquired == gpu::AcquireResult::kOutOfDate) {
    RecreateSwapchain();
    return;
  }
  if (acquired == gpu::AcquireResult::kTimeout) {
    // The compositor is holding every image (window unmapped/occluded). With a
    // capture armed, keep driving the whole frame offscreen instead of just
    // the one frame the capture is due on: the engine needs its warm-up frames
    // (sky/atmosphere bakes, temporal history, streamed uploads) or the
    // capture comes out black. Without one, skip the frame rather than wedging
    // the loop on an unbounded wait, and do not burn the GPU on a window
    // nobody is compositing.
    if (!CaptureArmed() || !EnsureCaptureImage()) {
      if (time_seconds_ - acquire_timeout_log_time_ >= 1.0) {
        RX_WARN("swapchain acquire timed out; the compositor is not releasing "
                "images (window unmapped or occluded) - skipping frames");
        acquire_timeout_log_time_ = time_seconds_;
      }
      return;
    }
    capture_offscreen_ = true;
    swapchain_starved_ = true;
    acquired = gpu::AcquireResult::kOk;  // render this frame, offscreen
  }
  if (acquired != gpu::AcquireResult::kOk && acquired != gpu::AcquireResult::kSuboptimal)
    return;

  // Frame generation: acquire a second image for the interpolated present.
  bool fg_frame = false;
  u32 interp_index = 0;
#if defined(RX_HAS_FSR3)
  // Frame generation presents a second swapchain image, which the offscreen
  // capture path has not acquired; skip it for that frame.
  if (!capture_offscreen_ && settings_.frame_generation &&
      !settings_.path_trace &&
      swapchain_->color_space() == gpu::ColorSpace::kSrgbNonlinear && upscaler_ &&
      upscaler_->kind() == UpscalerKind::kFsr3) {
    if (!framegen_ && !framegen_attempted_) {
      framegen_attempted_ = true;
      framegen_ = CreateFrameGenerator(
          *device_, {.display_width = swapchain_->extent().width,
                     .display_height = swapchain_->extent().height,
                     .render_width = render_width_,
                     .render_height = render_height_});
      if (!framegen_)
        RX_WARN("framegen: unavailable, presenting real frames only");
    }
    if (framegen_) {
      gpu::AcquireResult second = swapchain_->AcquireSecond(slot, &interp_index);
      fg_frame =
          second == gpu::AcquireResult::kOk || second == gpu::AcquireResult::kSuboptimal;
    }
  }
#endif

  // Texture streaming: flush the retire ring (safe now - BeginFrame waited the
  // slot fence) and run the promote/demote policy before any pass records, so
  // the whole frame binds one consistent generation of material sets.
  if (material_system_) {
    material_system_->BeginFrame(frame_index_);
    material_system_->UpdateStreaming(frame_index_);
  }

  transient_pool_->BeginFrame();
  graph_.Reset();
  fg_active_frame_ = fg_frame;
  BuildFrameGraph(frames_[slot], image_index, view);
  if (!graph_.Compile(*device_, *transient_pool_))
    return;

  profiler_.SetDetail(settings_.gpu_pass_timings);
  profiler_.BeginFrame(*cmd, slot);
  graph_.SetPassHooks(
      [this](gpu::CommandList &c, const char *name) {
        profiler_.BeginPass(c, name);
      },
      [this](gpu::CommandList &c) { profiler_.EndPass(c); });

  PassContext ctx;
  ctx.cmd = cmd;
  ctx.device = device_.Get_UseOnlyIfYouKnowWhatYouareDoing();
  ctx.graph = &graph_;
  // With per-pass detail off the whole frame gets one bracket so
  // gpu_frame_ms() (dynamic resolution's input) stays fed.
  profiler_.BeginFrameTotal(*cmd);
  // With async passes the graph splits the frame into segments; the returned
  // list is the final one and the only valid argument for SubmitFrame.
  gpu::CommandList *final_cmd = graph_.Execute(ctx);
  profiler_.EndFrameTotal(*final_cmd);

  const bool screenshot_due =
      !screenshot_path_.empty() && time_seconds_ >= screenshot_at_;
  const bool sequence_due = !seq_prefix_.empty() && seq_written_ < seq_count_ &&
                            time_seconds_ >= seq_at_;
  bool dump_due = false;
  if (const char *dump = ::getenv("RX_FRAMEGEN_DUMP")) {
    u64 dump_frame = ::strtoull(dump, nullptr, 10);
    dump_due = fg_frame &&
               (frame_index_ == dump_frame || frame_index_ == dump_frame + 1);
  }
  bool capture_ready = capture_offscreen_;
  auto copy_capture = [&] {
    if (!(screenshot_due || (sequence_due && seq_frame_ctr_ % seq_stride_ == 0) ||
          dump_due) || !EnsureCaptureImage())
      return;
    const gpu::GpuImage &backbuffer = swapchain_->image(image_index);
    gpu::TextureBarrier pre[] = {
        gpu::Transition(backbuffer, gpu::ResourceState::kPresent, gpu::ResourceState::kCopySrc),
        gpu::Transition(capture_image_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst)};
    final_cmd->TextureBarriers(pre);
    final_cmd->CopyTexture(backbuffer, capture_image_);
    gpu::TextureBarrier post[] = {
        gpu::Transition(backbuffer, gpu::ResourceState::kCopySrc, gpu::ResourceState::kPresent),
        gpu::Transition(capture_image_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kCopySrc)};
    final_cmd->TextureBarriers(post);
    capture_ready = true;
  };

  gpu::PresentResult presented;
#if defined(RX_HAS_FSR3)
  Fsr3SharedResources fg_shared;
  if (fg_frame && upscaler_ && upscaler_->fsr3_shared(&fg_shared)) {
    const gpu::GpuImage &backbuffer = swapchain_->image(image_index);
    const gpu::GpuImage &target = swapchain_->image(interp_index);
    // The graph's final barrier left the backbuffer in PRESENT; bring it back
    // for the interpolation dispatch to sample.
    {
      gpu::TextureBarrier to_read = gpu::Transition(backbuffer, gpu::ResourceState::kPresent,
                                          gpu::ResourceState::kShaderReadCompute);
      final_cmd->TextureBarriers(base::Span(&to_read, 1));
    }
    FrameGenInputs fin;
    fin.backbuffer = &backbuffer;
    fin.dilated_depth = fg_shared.dilated_depth;
    fin.dilated_motion = fg_shared.dilated_motion;
    fin.recon_prev_depth = fg_shared.recon_prev_depth;
    fin.frame_delta_seconds = view.frame_delta_seconds;
    fin.camera_near = 0.1f;
    fin.camera_fov_y = view.camera.fov_y;
    fin.frame_id = frame_index_;
    fin.reset = !framegen_was_active_;
    bool interpolated = framegen_->Record(*final_cmd, fin);
    framegen_was_active_ = interpolated;

    if (interpolated) {
      const gpu::GpuImage &interp = framegen_->interpolated();
      gpu::TextureBarrier pre[] = {
          gpu::Transition(interp, gpu::ResourceState::kGeneral, gpu::ResourceState::kCopySrc),
          gpu::Transition(target, gpu::ResourceState::kUndefined,
                     gpu::ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(interp, target);
      gpu::TextureBarrier mid[] = {
          gpu::Transition(interp, gpu::ResourceState::kCopySrc, gpu::ResourceState::kGeneral),
          gpu::Transition(target, gpu::ResourceState::kCopyDst,
                     gpu::ResourceState::kColorTarget)};
      final_cmd->TextureBarriers(mid);
      // Re-draw the UI onto the generated frame (the interpolation sourced the
      // pre-UI copy). Both backends replay retained draw data, so recording
      // them twice per frame is safe; blur_source was filled by the ui pass.
      if (view.hud_draw || view.ui_draw) {
        gpu::ColorAttachment ui_color{.view = target.view, .load = gpu::LoadOp::kLoad};
        final_cmd->BeginRendering(
            {.extent = target.extent, .colors = base::Span(&ui_color, 1)});
        if (view.hud_draw)
          view.hud_draw(*final_cmd);
        if (view.ui_draw)
          view.ui_draw(*final_cmd);
        final_cmd->EndRendering();
      }
      gpu::TextureBarrier post[] = {gpu::Transition(target, gpu::ResourceState::kColorTarget,
                                          gpu::ResourceState::kPresent),
                               gpu::Transition(backbuffer,
                                          gpu::ResourceState::kShaderReadCompute,
                                          gpu::ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    } else {
      // Dispatch failed: duplicate the real frame so the acquired image still
      // presents something sane.
      gpu::TextureBarrier pre[] = {gpu::Transition(backbuffer,
                                         gpu::ResourceState::kShaderReadCompute,
                                         gpu::ResourceState::kCopySrc),
                              gpu::Transition(target, gpu::ResourceState::kUndefined,
                                         gpu::ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(backbuffer, target);
      gpu::TextureBarrier post[] = {
          gpu::Transition(backbuffer, gpu::ResourceState::kCopySrc,
                     gpu::ResourceState::kPresent),
          gpu::Transition(target, gpu::ResourceState::kCopyDst, gpu::ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    }
    copy_capture();
    presented = device_->SubmitFrameGen(final_cmd, *swapchain_, interp_index,
                                        image_index);
    fg_presents_ += 2;

    // Debug: RX_FRAMEGEN_DUMP=<frame> writes real frame N, the interpolated
    // N->N+1 midpoint and real frame N+1 as pngs in the working directory.
    if (const char *dump = ::getenv("RX_FRAMEGEN_DUMP")) {
      u64 dump_frame = ::strtoull(dump, nullptr, 10);
      if (frame_index_ == dump_frame && capture_ready) {
        DumpFgImage(capture_image_, gpu::ResourceState::kCopySrc,
                    true, "fg_dump_real0.png");
      } else if (frame_index_ == dump_frame + 1 && capture_ready) {
        DumpFgImage(framegen_->interpolated(), gpu::ResourceState::kGeneral, false,
                    "fg_dump_interp.png");
        DumpFgImage(framegen_->hudless(), gpu::ResourceState::kShaderReadCompute,
                    false, "fg_dump_hudless.png");
        DumpFgImage(capture_image_, gpu::ResourceState::kCopySrc,
                    true, "fg_dump_real1.png");
      }
    }
  } else
#endif
  if (capture_offscreen_) {
    // No image was acquired, so there is nothing to present: complete the
    // frame through the swapchainless overload (signals the slot fence) and
    // let the capture below read the offscreen image.
    device_->SubmitFrame(final_cmd);
    presented = gpu::PresentResult::kOk;
    framegen_was_active_ = false;
  } else {
    copy_capture();
    presented = device_->SubmitFrame(final_cmd, *swapchain_, image_index);
    framegen_was_active_ = false;
    fg_presents_ += 1;
  }
  instances_.OnFrameSubmitted(*device_);

  // Present-rate accounting: the observable proof that generation runs.
  ++fg_engine_frames_;
  if (settings_.frame_generation && time_seconds_ - fg_log_time_ >= 2.0) {
    if (fg_log_time_ > 0.0) {
      f64 span = time_seconds_ - fg_log_time_;
      RX_INFO("framegen: {:.0f} engine fps -> {:.0f} presented fps",
              fg_engine_frames_ / span, fg_presents_ / span);
    }
    fg_log_time_ = time_seconds_;
    fg_engine_frames_ = 0;
    fg_presents_ = 0;
  }

  if (screenshot_due && capture_ready) {
    WriteScreenshot();
  }
  if (sequence_due) {
    if (seq_frame_ctr_ % seq_stride_ == 0 && capture_ready) {
      char path[512];
      ::snprintf(path, sizeof(path), "%s_%04d.png", seq_prefix_.c_str(),
                    seq_written_);
      WriteBackbufferPng(path);
      ++seq_written_;
    }
    ++seq_frame_ctr_;
  }
  if (hdr_pending_) {
    WriteHdr();
    hdr_pending_ = false;
  }

  capture_offscreen_ = false;
  // With the run's captures written, go back to presenting: a still-starved
  // swapchain then just skips frames instead of rendering for nobody.
  if (!CaptureArmed())
    swapchain_starved_ = false;

  if (presented == gpu::PresentResult::kOutOfDate) {
    RecreateSwapchain();
  }

  // Editor picking runs as a standalone synchronous submit after the frame (a
  // rare operation, so the stall is acceptable) so it never perturbs the main
  // frame graph. It reuses the meshes/transforms this frame presented.
  if (pick_requested_)
    RenderPickPass(view);

  ++frame_index_;
}

void Renderer::RecordDepthOnlyScene(gpu::CommandList &cmd,
                                    const Mat4 &light_view_proj,
                                    const FrameResources &frame,
                                    const FrameView &view) {
  gpu::BindingSetHandle bound_material{};
  // All the shadow caster pipelines share one layout, so pushes and set binds
  // persist across the per-submesh variant switches below. Bind the masked
  // static permutation up front so the matrix push always has a pipeline.
  gpu::PipelineHandle bound_pipeline = shadow_.pipeline();
  cmd.BindPipeline(bound_pipeline);
  // Every caster pipeline shares this layout, so the arena binds once for the
  // whole depth-only pass; the per-draw base below selects the record.
  cmd.BindTransient(ShadowPass::kDrawRecordSet,
                    {gpu::Bind::StorageBuffer(0, frame.draw_records)});
  cmd.PushConstants(&light_view_proj, sizeof(Mat4),
                    ShadowPass::kLightMatrixOffset);
  for (const DrawItem &item : view.draws) {
    const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
    // no_rt skips grass-like fill geometry, but skinned actors are
    // no_rt only to stay out of the tlas; they still cast shadows.
    // dynamic_vertices meshes always cast, even if a game marks them no_rt.
    if (!mesh || mesh->all_blend ||
        (mesh->no_rt && !mesh->skinned && !mesh->dynamic_vertices))
      continue;
    // Skinned casters run the bone-blended vertex stage so the
    // shadow tracks the animated pose, not the bind pose.
    bool draw_skinned = mesh->skinned && item.skin_offset >= 0 &&
                        static_cast<bool>(shadow_.skinned_pipeline());
    // The per-draw head sits at offset 0; the cascade matrix pushed above it
    // outlives every caster.
    ShadowPass::DrawPush draw_push{};
    draw_push.draw_index = static_cast<u32>(&item - view.draws.data()) + 1u;
    if (draw_skinned) {
      draw_push.skin_offset = static_cast<u32>(item.skin_offset);
      draw_push.bone_address = frame.bone_palette.address;
    }
    cmd.PushConstants(&draw_push, sizeof(draw_push));
    cmd.BindVertexBuffer(0, mesh->vertices);
    if (draw_skinned) {
      cmd.BindVertexBuffer(1, mesh->skinning);
    }
    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
      if (submesh.blend)
        continue;
      // Opaque casters draw depth-only (no fragment, early-Z stays);
      // masked ones bind the alpha-test fragment + its material set.
      gpu::PipelineHandle pipeline =
          draw_skinned ? shadow_.skinned_pipeline(submesh.alpha_mask)
                       : shadow_.pipeline(submesh.alpha_mask);
      if (!(pipeline == bound_pipeline)) {
        cmd.BindPipeline(pipeline);
        bound_pipeline = pipeline;
      }
      if (submesh.alpha_mask) {
        gpu::BindingSetHandle material = material_system_->set(submesh.material);
        if (!(material == bound_material)) {
          cmd.BindSet(0, material);
          bound_material = material;
        }
      }
      cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
    }
  }
  f32 shadow_planes[5][4];
  ExtractFrustumPlanes(light_view_proj, shadow_planes);
  for (const InstanceStore::Group &group : instances_.groups()) {
    if (!group.alive)
      continue;
    if (group.cullable &&
        SphereOutsideFrustum(shadow_planes, group.bounds_center,
                             group.bounds_radius))
      continue;
    const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
    if (!mesh || mesh->all_blend || (mesh->no_rt && !mesh->dynamic_vertices))
      continue;
    cmd.BindVertexBuffer(0, mesh->vertices);
    cmd.BindVertexBuffer(1, group.buffer);
    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
      if (submesh.blend)
        continue;
      const gpu::PipelineHandle pipeline =
          shadow_.instanced_pipeline(submesh.alpha_mask);
      if (!(pipeline == bound_pipeline)) {
        cmd.BindPipeline(pipeline);
        bound_pipeline = pipeline;
      }
      if (submesh.alpha_mask) {
        const gpu::BindingSetHandle material =
            material_system_->set(submesh.material);
        if (!(material == bound_material)) {
          cmd.BindSet(0, material);
          bound_material = material;
        }
      }
      cmd.DrawIndexed(submesh.index_count,
                      static_cast<u32>(group.transforms.size()),
                      submesh.index_offset, 0, 0);
    }
  }
}

u32 Renderer::PrevSkinOffset(const DrawItem &item) const {
  if (prev_bone_base_ == 0 || item.prev_skin_offset < 0)
    return static_cast<u32>(item.skin_offset);
  return prev_bone_base_ + static_cast<u32>(item.prev_skin_offset);
}

void Renderer::BuildFrameGraph(FrameResources &frame, u32 image_index,
                               const FrameView &view) {
  u32 frame_slot = frame_index_ % kFramesInFlight;
  bool rt_shadows = rt_available_ && settings_.rt_shadows;
  bool rtao_active = rt_available_ && rtao_.available() && settings_.rtao && RtaoOpt.get();
  // RCGI takes over the indirect-diffuse path (DDGI + SSGI) when on. It is
  // available with hardware ray query OR the software SDF clipmap tracer.
  bool sdf_ready = sdf_clipmap_ && sdf_clipmap_->ready() && sdf_available_;
  bool rcgi_active = rcgi_ && settings_.rcgi && settings_.ibl &&
                     bindless_ != nullptr && (rt_available_ || sdf_ready);
  // Software mode: forced by a non-ray-query device or RX_RCGI_SW (A/B on RT
  // hardware). Needs the SDF clipmap; the world side then traces the clipmap
  // and the resolve is forced to the probes-only path (the M2 gather is
  // ray-query).
  bool rcgi_software =
      rcgi_active && sdf_ready && (!rt_available_ || rcgi_force_software_);
  bool rcgi_probes_only = RcgiProbesOnlyOpt || rcgi_software;
  bool ddgi_active = ddgi_ && settings_.ddgi && DdgiOpt.get() && settings_.ibl && !rcgi_active;
  bool reflections_active =
      rt_available_ && settings_.rt_reflections && ReflOpt.get() && bindless_ != nullptr;
  // The ray-query fragment variant serves both shadows and reflections.
  bool use_rt_frag = rt_shadows || reflections_active;
  if (rt_available_ && bindless_ && settings_.path_trace &&
      settings_.path_trace_recon && !settings_.path_trace_reference)
    recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
  bool path_scene_moved = false;
  bool path_trace =
      rt_available_ && bindless_ != nullptr && settings_.path_trace &&
      (path_tracer_.available() ||
       (settings_.path_trace_recon && !settings_.path_trace_reference &&
        recon_path_tracer_.available()));
  bool rcgi_world = rcgi_active && !path_trace;
  if (rcgi_ && !rcgi_world)
    rcgi_->RequestReset();
  // Set by the raster path when the 3D precipitation volume draws; the
  // post-resolve screen-space streak fallback is skipped that frame.
  bool precip_volume_drawn = false;
  // kMsaa: the prepass + opaque scene render multisampled and resolve before
  // everything downstream, which then runs single-sampled exactly as kNone.
  // ApplySettings already rebuilt the mesh pipelines at this sample count.
  const bool msaa = applied_msaa_samples_ > 1 && !path_trace;
  const u32 msaa_samples = msaa ? applied_msaa_samples_ : 1;
  // Scene pass consumes last frame's rate image; the rebuild pass below the
  // transparents keeps it fresh. Wireframe wants exact per-pixel lines.
  // The VRS rate image cannot attach to a multisampled pass here.
  vrs_active_ = settings_.vrs && vrs_.available() && !path_trace &&
                !settings_.wireframe && !msaa;
  // Foliage uploaded before path tracing was enabled has no blas (it was
  // excluded from the realtime tlas). Build it now so alpha-tested vegetation
  // appears when path tracing is toggled on at runtime, not only when set
  // before content load.
  if (rt_geometry_dirty_ || (path_trace && rt_foliage_dirty_)) {
    const bool ready = EnsureRayTracingGeometry();
    rt_geometry_dirty_ = !ready;
    if (path_trace && ready)
      rt_foliage_dirty_ = false;
  }
  bool fog_active = rt_available_ && settings_.fog && !path_trace;
  // Ambient occlusion technique: ray-traced + NRD-denoised when available, else
  // the screen-space fallback so non-rt tiers (and forced low presets) keep ao.
  bool nrd_ao = false;
  bool nrd_shadow = false;
#if defined(RX_HAS_NRD)
  nrd_ao = rtao_active && nrd_.available();
  nrd_shadow = rt_shadows && shadow_trace_.available() && nrd_.available();
#endif
  // Denoised stochastic reflections need the NRD specular denoiser; without
  // it the rt fragment variant keeps its inline deterministic mirror ray.
  bool spec_refl_active = false;
#if defined(RX_HAS_NRD)
  spec_refl_active = reflections_active && reflection_trace_.available() &&
                     nrd_.available() && !path_trace;
#endif
  bool ss_ao = settings_.ssao && !nrd_ao && !path_trace;
  // Cascaded shadow maps: the raster sun-shadow path, used whenever ray-traced
  // shadows are not. The rt fragment variant traces its own shadow ray instead.
  bool csm_active = settings_.shadow_maps && !rt_shadows && !path_trace;
  // Screen-space reflections stand in for ray-traced reflections on raster
  // tiers.
  bool ssr_active = settings_.ssr && !path_trace && !reflections_active;
  // Screen-space gi stands in for the ddgi probe volume on raster tiers.
  bool ssgi_active =
      settings_.ssgi && !path_trace && !ddgi_active && !rcgi_active;

  // Transparent work is gathered up front: water forces a tlas (the water
  // pipeline statically binds it) and an opaque snapshot pass.
  struct TransparentDraw {
    const DrawItem *item;
    const gpu::GpuSubmesh *submesh;
    f32 distance_sq;
  };
  base::Vector<TransparentDraw> transparent;
  transparent.reserve(view.draws.size());
  bool any_water = false;
  const DrawItem *adaptive_water_item = nullptr;
  const gpu::GpuSubmesh *adaptive_water_submesh = nullptr;
  f32 adaptive_water_area = 0.0f;
  for (const DrawItem &item : view.draws) {
    const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
    if (!mesh)
      continue;
    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
      if (!submesh.blend)
        continue;
      f32 dx = item.transform.m[12] - view.camera.eye.x;
      f32 dy = item.transform.m[13] - view.camera.eye.y;
      f32 dz = item.transform.m[14] - view.camera.eye.z;
      transparent.push_back({&item, &submesh, dx * dx + dy * dy + dz * dz});
      if (submesh.water) {
        any_water = true;
        if (settings_.adaptive_water && water_ &&
            water_->adaptive_available() && mesh->planar_water) {
          f32 area = (mesh->water_bounds[2] - mesh->water_bounds[0]) *
                     (mesh->water_bounds[3] - mesh->water_bounds[1]);
          if (area > adaptive_water_area) {
            adaptive_water_area = area;
            adaptive_water_item = &item;
            adaptive_water_submesh = &submesh;
          }
        }
      }
    }
  }
  bool water_pipeline_active = any_water && water_ != nullptr;
  // Volumetric precipitation's optional per-particle sun rays consult the TLAS
  // from its vertex shader, so an active rainstorm must keep the TLAS built
  // and current even when every other ray-traced effect is disabled.
  const bool precip_volume_ready =
      settings_.weather.volumetric && settings_.weather.precipitation > 0.0f &&
      !settings_.interior && precip_volume_.available();
  const bool precip_rt = precip_volume_ready && settings_.weather.rt_shadows &&
                         rt_available_ && !path_trace;
  // When the tlas is consulted for shading, the rasterized surface must match
  // the blas (built at lod 0), or rays can hit geometry that the selected
  // raster lod did not draw. This includes water reflections and volumetric-fog
  // visibility, not only the opaque scene's ray-traced effects.
  bool force_lod0_for_tlas =
      rt_shadows || rtao_active || ddgi_active ||
      (rcgi_active && !rcgi_software) || reflections_active || path_trace ||
      (water_pipeline_active && settings_.water_reflections) || fog_active ||
      precip_rt;

  // Skinned actors that asked to be ray traced in their animated pose
  // (DrawItem::rt_skin). Their structures ping-pong per frame, which is what
  // keeps them compatible with the async tlas build below: the slot being refit
  // is never one a live tlas references. See render/gi/skinned_rt.h.
  base::Vector<SkinnedRayTracing::Request> skin_requests;
  skinned_rt_.BeginFrame();
  if (RtSkinOpt && raytracing_ && bindless_ && material_system_ &&
      skinned_rt_.available()) {
    for (const DrawItem &item : view.draws) {
      if (item.rt_skin == 0 || item.skin_offset < 0 || !frame.bone_palette ||
          static_cast<u32>(item.skin_offset) >= view.bone_matrices.size())
        continue;
      skin_requests.push_back({.handle = item.rt_skin,
                               .mesh_key = item.mesh,
                               .skin_offset =
                                   static_cast<u32>(item.skin_offset)});
    }
  }

  // Async TLAS build (RX_RT_ASYNC_TLAS): build this frame's slot on the compute
  // queue while graphics consumes the slot built last frame (same async-fork
  // discipline as the DDGI/RCGI world passes, validated clean); a full frame
  // elapses before the slot is read, which the frame fence guarantees. Needs a
  // second queue and one primed slot; three ping-pong slots cover two frames in
  // flight (RayTracingContext::kSlots). The path tracer keeps the synchronous
  // same-slot build for reference correctness.
  // The read slot (previous frame's build) is only safe when it holds a current
  // build: RT enabled after raster-only frames leaves it unbuilt, and a mesh
  // replace (WaitIdle + RemoveBlas) retires slots pointing at the freed BLAS.
  // TlasSlotTracker falls back to a synchronous build+read when invalid.
  bool want_async_tlas = RtAsyncTlasOpt && device_->caps().async_compute &&
                         settings_.async_compute && !path_trace &&
                         frame_index_ > 0;
  TlasSlotTracker::Selection tlas_sel =
      raytracing_ ? raytracing_->SelectTlasSlots(frame_index_, want_async_tlas)
                  : TlasSlotTracker{}.Select(frame_index_, false);
  bool async_tlas = tlas_sel.async;
  u32 tlas_build_slot = tlas_sel.build_slot;
  u32 tlas_slot = tlas_sel.read_slot;

  // The frame's globals set (uniform + optional tlas + optional hi-z) is
  // rewritten once per frame, from the first pass that needs it. The slot's
  // fence has fired, so its previous frame no longer reads the set.
  gpu::BindingSetHandle globals_set = globals_sets_[frame_slot];
  auto update_globals_set =
      [this, globals_set, tlas_slot](PassContext &ctx, ResourceHandle cull_hiz,
                                     bool ms_active, bool want_tlas) {
        base::Vector<gpu::BindingItem> items;
        items.push_back(
            gpu::Bind::Uniform(0, frames_[frame_index_ % kFramesInFlight].globals, 0,
                          sizeof(FrameGlobals)));
        items.push_back(gpu::Bind::StorageBuffer(
            3, frames_[frame_index_ % kFramesInFlight].draw_records));
        if (want_tlas && rt_available_ && raytracing_ &&
            raytracing_->tlas(tlas_slot)) {
          items.push_back(gpu::Bind::Accel(1, raytracing_->tlas(tlas_slot)));
        }
        if (ms_active) { // hi-z for the task-stage occlusion cull (real or
                         // fallback)
          gpu::TextureView hiz = cull_hiz != kInvalidResource
                                ? ctx.graph->image(cull_hiz).view
                                : ms_dummy_hiz_.view;
          items.push_back(gpu::Bind::SampledView(2, hiz));
        }
        device_->UpdateBindingSet(globals_set, base::Span(items.data(), items.size()));
      };

  // Water + transparency over an opaque base. A lambda (rather than inline) so
  // the path tracer, which otherwise skips the whole raster transparency path,
  // can composite water over its result too. Consumes `transparent` (moved into
  // the pass), so it runs at most once per frame. Returns the composited
  // colour.
  // Declared here rather than at the assignment below because add_water
  // captures by reference and its `transparent` pass samples the hair volume
  // too; a lambda only captures what is already in scope where it is written.
  ResourceHandle hair_front = kInvalidResource;
  ResourceHandle hair_layers = kInvalidResource;
  bool hair_volume_on = false;
  HairStrands::Frame hair_frame;

  auto add_water =
      [&](ResourceHandle scene_color, ResourceHandle depth,
          ResourceHandle depth_export, ResourceHandle motion,
          ResourceHandle sun_shadow, ResourceHandle shadow_atlas, bool csm_on,
          u32 shadow_slot, u32 water_tlas_slot, bool globals_written,
          ResourceHandle rcgi_irr = kInvalidResource) -> ResourceHandle {
    // Stable: equal distances keep submission order.
    rx::StableSort(transparent.data(), transparent.data() + transparent.size(),
                   [](const TransparentDraw &a, const TransparentDraw &b) {
                     return a.distance_sq > b.distance_sq;
                   });

    // Transparency renders into a copy of the opaque result and refracts by
    // sampling the original, which never returns to attachment layout
    // afterwards: re-attaching a sampled image corrupts its compression
    // metadata on nvidia (the depth export exists for the same reason).
    ResourceHandle composite =
        graph_.CreateTexture({.name = "composite",
                              .format = kSceneColorFormat,
                              .width = render_width_,
                              .height = render_height_});
    graph_.AddPass(
        "opaque_copy",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Read(scene_color, ResourceUsage::kSampledCompute);
          builder.Write(composite, ResourceUsage::kStorageWrite);
        },
        [this, scene_color, composite](PassContext &ctx) {
          water_->RecordCopy(ctx, scene_color, composite, render_width_,
                             render_height_);
        });

    ResourceHandle opaque_color = scene_color;
    ResourceHandle opaque_depth = depth_export;
    graph_.AddPass(
        "transparent",
        [&](RenderGraph::PassBuilder &builder) {
          if (hair_volume_on) {
            builder.Read(hair_front, ResourceUsage::kSampledFragment);
            builder.Read(hair_layers, ResourceUsage::kSampledFragment);
          }
          builder.Write(composite, ResourceUsage::kColorAttachment);
          builder.Write(motion, ResourceUsage::kColorAttachment);
          builder.Write(depth, ResourceUsage::kDepthAttachment);
          builder.Read(opaque_color, ResourceUsage::kSampledFragment);
          builder.Read(opaque_depth, ResourceUsage::kSampledFragment);
          if (sun_shadow != kInvalidResource)
            builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
          if (csm_on)
            builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
          if (rcgi_irr != kInvalidResource)
            builder.Read(rcgi_irr, ResourceUsage::kSampledFragment);
        },
        [this, composite, motion, depth, opaque_color, opaque_depth, sun_shadow,
         water_tlas_slot, use_rt_frag, ddgi_active, water_pipeline_active,
         csm_on, shadow_slot, shadow_atlas, globals_set, globals_written,
         update_globals_set, frame_slot, adaptive_water_item,
         adaptive_water_submesh, transparent = base::move(transparent), rcgi_irr,
         rcgi_world, &frame, &view](PassContext &ctx) {
          if (!globals_written) {
            update_globals_set(ctx, kInvalidResource, false,
                               /*want_tlas=*/true);
          }

          gpu::BindingSetHandle env_set = env_transparent_sets_[frame_slot];
          // The hair transmittance volume, so skin under a groom is shadowed
          // by the fibres over it (see HairStrands::AddTransmittanceToGraph).
          // Null params = no hair this frame, which the forward pass reads as
          // "nothing overhead" rather than as a black shadow map.
          EnvironmentSystem::HairVolumeBinding hair_env_binding;
          {
            const HairStrands::TransmittanceBinding hb = hair_.transmittance();
            hair_env_binding.front_depth = hb.front_depth;
            hair_env_binding.layers = hb.layers;
            hair_env_binding.params = hb.params;
          }
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active)
            ddgi_binding = ddgi_->binding(frame_index_);
          gpu::TextureView sun_shadow_view = sun_shadow != kInvalidResource
                                            ? ctx.graph->image(sun_shadow).view
                                            : gpu::TextureView{};
          gpu::TextureView rcgi_irr_view = rcgi_irr != kInvalidResource
                                          ? ctx.graph->image(rcgi_irr).view
                                          : gpu::TextureView{};
          EnvironmentSystem::RcgiWorldBinding rcgi_world_binding;
          RcgiSystem::IrradianceBinding rcgi_world_src;
          if (rcgi_world)
            rcgi_world_src = rcgi_->irradiance_binding(frame_index_);
          if (rcgi_world_src.valid) {
            rcgi_world_binding.irradiance = rcgi_world_src.irradiance;
            rcgi_world_binding.visibility = rcgi_world_src.visibility;
            rcgi_world_binding.globals = rcgi_world_src.globals;
            rcgi_world_binding.probe_meta = rcgi_world_src.probe_meta;
            rcgi_world_binding.interior_vols = rcgi_world_src.interior_vols;
          }
          environment_->WriteEnvSet(
              env_set, gpu::TextureView{}, ddgi_active ? &ddgi_binding : nullptr,
              csm_on ? ctx.graph->image(shadow_atlas).view : gpu::TextureView{},
              csm_on ? shadow_.cascade_buffer(shadow_slot) : gpu::GpuBuffer{},
              shadow_.cascade_buffer_size(),
              ctx.graph->image(opaque_color).view, sun_shadow_view,
              frame.lights, frame.lights.size, gpu::TextureView{}, cluster_counts_,
              cluster_indices_, frame.decals, decal_cluster_indices_,
              decal_atlas_view_,
              local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                    : gpu::GpuBuffer{},
              local_shadows_active_ ? local_shadows_.atlas().view
                                    : gpu::TextureView{},
              decal_normal_atlas_view_, gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, gpu::TextureView{}, gpu::TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : gpu::TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : gpu::TextureView{},
              water_field_active_ ? water_field_.ring_view(0) : gpu::TextureView{},
              water_field_active_ ? water_field_.ring_view(1) : gpu::TextureView{},
              water_field_active_ ? water_field_.params_buffer(frame_slot)
                                  : gpu::GpuBuffer{},
              gpu::TextureView{}, gpu::TextureView{}, rcgi_irr_view,
              rcgi_world_src.valid ? &rcgi_world_binding : nullptr,
              decal_baker_.available() ? decal_baker_.albedo_view()
                                       : gpu::TextureView{},
              decal_baker_.available() ? decal_baker_.fx_view()
                                       : gpu::TextureView{},
              decal_baker_.available() ? decal_baker_.tile_uv_buffer(frame_slot)
                                       : gpu::GpuBuffer{},
              hair_env_binding.params ? &hair_env_binding : nullptr);

          // Update the dominant planar surface before beginning rasterization.
          // Its CBT/vertex/indirect buffers persist inside WaterPass; this
          // dispatch only changes leaf slots whose LOD decision changed.
          if (adaptive_water_item && adaptive_water_submesh) {
            if (const gpu::GpuMesh *adaptive_mesh =
                    meshes_.find(adaptive_water_item->mesh)) {
              const f32 aspect = static_cast<f32>(render_width_) /
                                 static_cast<f32>(rx::Max(render_height_, 1u));
              Mat4 vp = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f) *
                        LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
              Vec3 camera_local = TransformPoint(
                  Inverse(adaptive_water_item->transform), view.camera.eye);
              AdaptiveWaterMesh::UpdateParams params;
              params.local_to_clip = vp * adaptive_water_item->transform;
              for (int k = 0; k < 4; ++k) params.bounds[k] = adaptive_mesh->water_bounds[k];
              params.camera_local = camera_local;
              params.height = adaptive_mesh->water_height;
              params.time = static_cast<f32>(time_seconds_);
              params.target_pixels = settings_.water_target_triangle_pixels;
              params.render_width = render_width_;
              params.render_height = render_height_;
              params.triangle_budget = settings_.water_triangle_budget;
              params.surface_key = adaptive_water_item->mesh;
              water_->UpdateAdaptive(*ctx.cmd, params);
            }
          }

          gpu::ColorAttachment colors[2];
          colors[0] = {.view = ctx.graph->image(composite).view,
                       .load = gpu::LoadOp::kLoad};
          colors[1] = {.view = ctx.graph->image(motion).view,
                       .load = gpu::LoadOp::kLoad};
          gpu::DepthAttachment depth_attachment{.view = ctx.graph->image(depth).view,
                                           .load = gpu::LoadOp::kLoad};
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = base::Span(colors, 2),
                                   .depth = &depth_attachment});

          // Effect-shader fire/glows use the additive blend pipeline; the same
          // unlit fragment shader serves both the alpha and additive effect
          // materials (it premultiplies coverage for the additive one).
          enum class Mode { kNone, kWater, kBlend, kBlendAdditive };
          Mode mode = Mode::kNone;
          gpu::BindingSetHandle bound_material{};
          const DrawItem *bound_item = nullptr;
          for (const TransparentDraw &draw : transparent) {
            const gpu::GpuMesh *mesh = meshes_.find(draw.item->mesh);
            if (!mesh)
              continue;
            bool as_water = draw.submesh->water && water_pipeline_active;
            bool adaptive_draw = as_water && draw.item == adaptive_water_item &&
                                 draw.submesh == adaptive_water_submesh;
            bool additive = draw.submesh->effect_additive;

            Mode wanted =
                as_water ? Mode::kWater
                         : (additive ? Mode::kBlendAdditive : Mode::kBlend);
            if (mode != wanted) {
              gpu::BindingSetHandle bindless_set =
                  bindless_ ? bindless_->set() : gpu::BindingSetHandle{};
              if (as_water) {
                water_->Bind(ctx, globals_set, env_set, bindless_->set(),
                             opaque_color, opaque_depth);
              } else if (additive) {
                mesh_pipeline_->BindBlendAdditive(
                    *ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag);
              } else {
                mesh_pipeline_->BindBlend(*ctx.cmd, globals_set, env_set,
                                          bindless_set, use_rt_frag);
              }
              mode = wanted;
              bound_material = {};
              bound_item = nullptr;
            }
            if (draw.item != bound_item) {
              MeshPushConstants push{};
              push.draw_index =
                  static_cast<u32>(draw.item - view.draws.data()) + 1u;
              // Same packing as the opaque site: low 24 bits the per-draw
              // tint, top byte the baked decal-layer tile.
              push.tint_packed =
                  (draw.item->tint & 0xffffffu) |
                  (decal_baker_.tile_slot(draw.item->decal_receiver) << 24);
              // The blend pipelines run the static vertex path, which still
              // applies morphs (only skinning needs the extra vertex stream).
              if (mesh->morph_target_count > 0 &&
                  draw.item->morph_offset >= 0 && draw.item->morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(draw.item->morph_offset);
                push.morph_count = draw.item->morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              if (as_water) {
                // The water pipeline shares the mesh push block. Push the whole
                // struct so detail_rect is zeroed: mesh.vs sinks vertices
                // inside a nonzero rect, and a stale one would sink the water
                // plane.
                ctx.cmd->PushConstants(&push, sizeof(push));
                if (!adaptive_draw) {
                  ctx.cmd->BindVertexBuffer(0, mesh->vertices);
                  ctx.cmd->BindIndexBuffer(mesh->indices, 0,
                                           gpu::IndexType::kUint32);
                }
              } else {
                mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
              }
              bound_item = draw.item;
            }
            gpu::BindingSetHandle material =
                material_system_->set(draw.submesh->material);
            if (!(material == bound_material)) {
              if (as_water) {
                water_->BindMaterial(*ctx.cmd, material);
              } else {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
              }
              bound_material = material;
            }
            if (adaptive_draw) {
              water_->DrawAdaptive(*ctx.cmd);
              // A following authored submesh may share this DrawItem; force it
              // to restore the source vertex/index buffers.
              bound_item = nullptr;
            } else {
              mesh_pipeline_->DrawSubmesh(*ctx.cmd, *draw.submesh);
            }
          }

          // Heightfield fluid surface (flowing water + lava). Drawn last in the
          // transparent pass over the same color+motion+depth targets: it binds
          // its own pipeline and a transient set wrapping the solver's GENERAL
          // images, so the mesh/water bindings above do not need restoring. The
          // solver's final barrier (fluid_sim.cc) made the state graphics-
          // readable. Depth write + reversed-z Greater resolve the lava-under-
          // water ordering regardless of instance raster order.
          if (fluid_sim_active_ && fluid_sim_.active() && fluid_surface_) {
            fluid_surface_->Draw(ctx, globals_set, env_set, fluid_sim_,
                                 frame_slot, static_cast<f32>(time_seconds_));
          }
          ctx.cmd->EndRendering();
        });
    return composite;
  };

  // Camera state for both this frame and reprojection. Jitter lives in the
  // projection, not the matrices used for motion vectors.
  f32 aspect =
      static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  // The ortho view shares the perspective's reversed-z clip space, so every
  // depth-aware pass below is unchanged.
  Mat4 proj;
  if (view.camera.ortho_height > 0.0f) {
    // A degenerate depth range collapses the matrix into a silently broken view.
    BASE_DCHECK(view.camera.ortho_far > view.camera.ortho_near,
                "ortho camera requires ortho_far > ortho_near");
    const f32 half_h = view.camera.ortho_height * 0.5f;
    const f32 half_w = half_h * aspect;
    proj = OrthographicReversedZ(-half_w, half_w, -half_h, half_h, view.camera.ortho_near,
                                 view.camera.ortho_far);
  } else {
    proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f);
  }
  Mat4 view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
  Mat4 view_proj = proj * view_mat;

  // Streaming feedback: touch the materials of frustum-visible draws only.
  // view.draws is the full submitted list (GPU culling happens later), so
  // without the sphere test every loaded material would read as hot and the
  // texture LRU would degenerate to "loaded".
  if (material_system_ && material_system_->streaming_active()) {
    f32 touch_planes[5][4];
    ExtractFrustumPlanes(view_proj, touch_planes);
    for (const DrawItem &item : view.draws) {
      const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
      if (!mesh)
        continue;
      const f32 *m = item.transform.m;
      const f32 *c = mesh->bounds_center;
      Vec3 wc{m[0] * c[0] + m[4] * c[1] + m[8] * c[2] + m[12],
              m[1] * c[0] + m[5] * c[1] + m[9] * c[2] + m[13],
              m[2] * c[0] + m[6] * c[1] + m[10] * c[2] + m[14]};
      f32 sx = ::sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
      f32 sy = ::sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
      f32 sz = ::sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
      f32 radius = mesh->bounds_radius * rx::Max(sx, rx::Max(sy, sz));
      if (radius > 0.0f && SphereOutsideFrustum(touch_planes, wc, radius))
        continue;
      for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
        material_system_->Touch(submesh.material, frame_index_);
      }
    }
    for (const InstanceStore::Group &group : instances_.groups()) {
      if (!group.alive ||
          (group.cullable &&
           SphereOutsideFrustum(touch_planes, group.bounds_center,
                                group.bounds_radius))) {
        continue;
      }
      const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
      if (!mesh)
        continue;
      for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
        material_system_->Touch(submesh.material, frame_index_);
      }
    }
  }

  bool temporal = settings_.aa_mode == AntiAliasingMode::kTaa ||
                  settings_.aa_mode == AntiAliasingMode::kUpscaler;
  f32 jitter_x = 0, jitter_y = 0;
  if (temporal) {
    u32 sample_count = taa_.settings().jitter_sample_count;
    if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      // FSR-style phase count grows with the scale factor squared.
      f32 scale =
          static_cast<f32>(output_width_) / static_cast<f32>(render_width_);
      sample_count = static_cast<u32>(::ceilf(8.0f * scale * scale));
    }
    JitterSequence::Sample(frame_index_, sample_count, &jitter_x, &jitter_y);
  }

  bool first_frame = !has_prev_frame_;

  FrameGlobals globals;
  globals.view_proj = view_proj;
  globals.prev_view_proj = has_prev_frame_ ? prev_view_proj_ : view_proj;
  globals.inv_view_proj = Inverse(view_proj);
  globals.jitter[0] = 2.0f * jitter_x / static_cast<f32>(render_width_);
  globals.jitter[1] = 2.0f * jitter_y / static_cast<f32>(render_height_);
  // Interior cells author their own lighting (XCLL/LGTM): the directional fill
  // rides the sun path with the authored colour/direction, and the sky-derived
  // sun/atmosphere/IBL are suppressed below.
  const bool interior = settings_.interior;
  Vec3 sun = Normalize(interior ? settings_.interior_directional_dir
                                : settings_.sun_direction);
  globals.sun_direction[0] = sun.x;
  globals.sun_direction[1] = sun.y;
  globals.sun_direction[2] = sun.z;
  // Lightning flashes the PER-FRAME direct light only (not settings_, so the
  // sun-change check never rebuilds the IBL cubemap): a brief bright blue-white
  // boost to the directional intensity, colour and ambient fill.
  const f32 flash = interior ? 0.0f : settings_.weather.lightning;
  if (interior) {
    globals.sun_direction[3] = settings_.interior_directional_intensity;
    globals.sun_color[0] = settings_.interior_directional_color.x;
    globals.sun_color[1] = settings_.interior_directional_color.y;
    globals.sun_color[2] = settings_.interior_directional_color.z;
    globals.sun_color[3] = 0.0f;
    globals.interior_ambient[0] = settings_.interior_ambient.x;
    globals.interior_ambient[1] = settings_.interior_ambient.y;
    globals.interior_ambient[2] = settings_.interior_ambient.z;
    globals.interior_fog_color0[0] = settings_.interior_fog_near_color.x;
    globals.interior_fog_color0[1] = settings_.interior_fog_near_color.y;
    globals.interior_fog_color0[2] = settings_.interior_fog_near_color.z;
    globals.interior_fog_color0[3] = settings_.interior_fog_near;
    globals.interior_fog_color1[0] = settings_.interior_fog_far_color.x;
    globals.interior_fog_color1[1] = settings_.interior_fog_far_color.y;
    globals.interior_fog_color1[2] = settings_.interior_fog_far_color.z;
    globals.interior_fog_color1[3] = settings_.interior_fog_far;
    globals.interior_fog_params[0] = settings_.interior_fog_power;
    globals.interior_fog_params[1] = settings_.interior_fog_max;
  } else {
    globals.sun_direction[3] = settings_.sun_intensity + flash * 9.0f;
    globals.sun_color[0] =
        settings_.sun_color.x + flash * (0.90f - settings_.sun_color.x);
    globals.sun_color[1] =
        settings_.sun_color.y + flash * (0.95f - settings_.sun_color.y);
    globals.sun_color[2] =
        settings_.sun_color.z + flash * (1.10f - settings_.sun_color.z);
    globals.sun_color[3] = settings_.ambient + flash * 0.5f;
  }
  globals.camera_position[0] = view.camera.eye.x;
  globals.camera_position[1] = view.camera.eye.y;
  globals.camera_position[2] = view.camera.eye.z;
  globals.camera_position[3] = settings_.ibl_intensity;
  globals.misc[0] = static_cast<f32>(render_width_);
  globals.misc[1] = static_cast<f32>(render_height_);
  globals.misc[2] = settings_.sun_angular_radius;
  globals.misc[3] = static_cast<f32>(frame_index_ % 4096);
  if (settings_.ibl && !interior)
    globals.flags |= kFrameFlagIbl;
  // With an authored dome the cubemap is a photograph of a real sky; the
  // procedural sun/moon/stars in sky.ps would sit on top of it as a second sky.
  if (environment_ && environment_->has_environment_map())
    globals.flags |= kFrameFlagAuthoredSky;
  if (nrd_ao || ss_ao)
    globals.flags |= kFrameFlagAoValid;
  if (csm_active && !interior)
    globals.flags |= kFrameFlagShadowMap;
  if (ddgi_active && !interior)
    globals.flags |= kFrameFlagDdgi;
  // RCGI composites in the IBL branch outdoors; indoors it now feeds the
  // interior branch instead (mesh.ps), lighting interiors with leak-free bounce
  // (its ray misses fall back to interior ambient) rather than a flat authored
  // term. Gated by RX_RCGI_INTERIOR so recreation's default (RCGI off) is
  // unchanged.
  if (rcgi_world && (!interior || RcgiInteriorOpt))
    globals.flags |= kFrameFlagRcgi;
  if (water_pipeline_active && settings_.water_reflections)
    globals.flags |= kFrameFlagWaterRt;
  if (rt_shadows && !interior)
    globals.flags |= kFrameFlagRtShadows;
  if (settings_.weather.aurora && !interior)
    globals.flags |= kFrameFlagAurora;
  // Aurora intensity rides the otherwise-unused pad_wind slot; sky.ps mirrors
  // it as `aurora.x` and night-gates it itself. Zero keeps the effect off.
  // pad_wind[1] carries the app's explicit night factor (moon-lit nights point
  // the "sun" downward, so the shader's elevation fallback reads as day there).
  globals.pad_wind[0] = (settings_.weather.aurora && !interior)
                            ? settings_.weather.aurora_intensity
                            : 0.0f;
  globals.pad_wind[1] = settings_.night;
  if (nrd_shadow && !interior)
    globals.flags |= kFrameFlagSigmaShadow;
  if (interior)
    globals.flags |= kFrameFlagInterior;
  if (reflections_active)
    globals.flags |= kFrameFlagReflections;
  if (spec_refl_active)
    globals.flags |= kFrameFlagSpecReflTex;
  globals.time = static_cast<f32>(time_seconds_);
  globals.debug_view = static_cast<u32>(settings_.debug_view);
  globals.reflection_cutoff = settings_.reflection_roughness_cutoff;
  globals.ao_ray_count =
      nrd_ao ? settings_.ao_rays : 0u; // rt ao rays, for the ray-count view

  // Dynamic point lights: copy into the host-visible frame buffer (capped).
  u32 light_count =
      rx::Min<u32>(static_cast<u32>(view.lights.size()), kMaxFrameLights);
  if (light_count > 0) {
    base::MemCopy(frame.lights.mapped, view.lights.data(),
                light_count * sizeof(PointLight));
  }
  // Active lightning strike: append the positioned flash light after the copy
  // so it clusters, claims local-shadow faces and fills the froxel volumetrics
  // like any other light (and ReSTIR/clustered reflections see the flash even
  // though the bolt itself is a raster overlay). The GLOBAL weather.lightning
  // sun/ambient boost above is separate; this adds locality on top.
  if (!interior && !path_trace) {
    light_count += lightning_.AppendLights(
        static_cast<PointLight *>(frame.lights.mapped) + light_count,
        kMaxFrameLights - light_count, settings_.weather);
  }
  // Local light shadows: the nearest casters claim atlas faces (writes each
  // claimed light's params.w in the mapped buffer, so it must follow the copy).
  local_shadows_active_ = false;
  if (settings_.local_shadows && !path_trace && light_count > 0) {
    local_shadows_.Assign(static_cast<PointLight *>(frame.lights.mapped),
                          light_count, view.camera.eye, frame_slot);
    local_shadows_active_ = local_shadows_.face_count() > 0;
  }
  u32 decal_count =
      rx::Min<u32>(static_cast<u32>(view.decals.size()), kMaxFrameDecals);
  if (decal_count > 0) {
    base::MemCopy(frame.decals.mapped, view.decals.data(),
                decal_count * sizeof(Decal));
  }
  globals.light_count = light_count;
  // Baked decal layers: queue this frame's stamps and gather the receivers that
  // are actually drawing, which is what the bake needs geometry and a pose from.
  decal_targets_.clear();
  if (decal_baker_.available()) {
    for (const DrawItem &item : view.draws) {
      if (item.decal_receiver == 0)
        continue;
      const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
      if (!mesh)
        continue;
      DecalBaker::Target target;
      target.receiver = item.decal_receiver;
      target.mesh = mesh;
      target.transform = item.transform;
      if (mesh->skinned && item.skin_offset >= 0) {
        target.bones = &frame.bone_palette;
        target.skin_offset = static_cast<u32>(item.skin_offset);
      }
      decal_targets_.push_back(target);
    }
    globals.decal_layer[0] = decal_baker_.tiles_per_row();
    globals.decal_layer[1] = decal_baker_.tile_uv();
    globals.decal_layer[2] = decal_baker_.tile_guard_uv();
  }
  // The FFT ocean, interaction field, shoreline wetting and caustics all
  // describe a water surface, and every water surface (sea, CBT sheet, lake)
  // reaches the renderer as a water-material submesh, so gate the whole family
  // on one being submitted. These features default on, and "available()" only
  // means the pipelines exist; without this gate every scene paid for the
  // sims, and worse, caustics modulated the sun on everything below the rest
  // height (y=0 by default): wavy grey mottling across dry ground.
  const bool scene_has_water = water_pipeline_active;
  fft_ocean_active_ = settings_.fft_ocean && ocean_.available() &&
                      !path_trace && !interior && scene_has_water;
  const bool fft_ocean_active = fft_ocean_active_;
  if (fft_ocean_active)
    globals.flags |= kFrameFlagFftOcean;
  water_field_active_ = settings_.water_field && water_field_.available() &&
                        !path_trace && !interior && scene_has_water;
  if (water_field_active_)
    globals.flags |= kFrameFlagWaterField;
  // The optional fluid solver runs whenever a domain is submitted; it is NOT
  // gated on scene_has_water (a lava-only scene carries no water material). It
  // IS gated on the surface draw being reachable: the fluid draws inside the
  // transparent pass, which needs water_ (only created with ray query); on a
  // non-RT device the solver would otherwise burn GPU every frame with no
  // visual output.
  fluid_sim_active_ = settings_.fluid_sim && fluid_sim_.available() &&
                      water_ != nullptr && fluid_surface_ != nullptr &&
                      !path_trace && view.fluid_domain != nullptr;
  // Shoreline wetting: snap the field to the camera and hand the shader its
  // origin/extent before the globals upload; the compute pass records below.
  shore_wetting_active_ = settings_.shore_wetting &&
                          shore_wetting_.available() && !path_trace &&
                          !interior && scene_has_water;
  if (shore_wetting_active_) {
    shore_wetting_.BeginFrame(view.camera.eye);
    shore_wetting_.FieldParams(globals.shore_field);
    globals.flags |= kFrameFlagShoreWetting;
  }
  // Physically-based water material + crest-SSS tunables ([water] settings),
  // read by water.ps and (for the caustic depth fade) mesh.ps/mesh_rt.ps.
  for (u32 i = 0; i < 3; ++i)
    globals.water_absorption[i] = settings_.water_absorption[i];
  globals.water_absorption[3] = settings_.water_absorption_scale;
  globals.water_material[0] = settings_.water_transmission;
  globals.water_material[1] = settings_.water_refl_foam_gain;
  globals.water_material[2] = settings_.water_sss_intensity;
  globals.water_material[3] = settings_.water_sss_exponent;
  globals.water_caustics[0] = settings_.water_caustic_intensity;
  globals.water_caustics[1] = settings_.water_rest_height;
  globals.water_caustics[2] = settings_.water_caustic_depth_fade;
  // Underwater caustics: gated on an actual water surface (scene_has_water),
  // not on the interaction field: the field defaults on in every scene, and
  // keying caustics off it painted the sun modulation onto dry ground below
  // y=0.
  water_caustics_active_ = settings_.water_caustics &&
                           water_caustics_.available() && !path_trace &&
                           !interior && scene_has_water;
  if (water_caustics_active_)
    globals.flags |= kFrameFlagWaterCaustics;
  // Skin blood-flow dynamics: advance the arterial pulse phase from the clock
  // and hand the perfusion/tension drivers to the skin pixel shader. The app
  // sets heart rate / global perfusion (exertion, blush, pallor) via settings.
  if (settings_.skin_dynamics) {
    constexpr f32 kTwoPi = 6.28318530718f;
    f32 phase = static_cast<f32>(time_seconds_) * settings_.skin_heart_rate * kTwoPi;
    globals.skin_dynamics[0] = ::fmodf(phase, kTwoPi);
    globals.skin_dynamics[1] = rx::Clamp(settings_.skin_perfusion, -0.5f, 0.5f);
    globals.skin_dynamics[2] = settings_.skin_pulse_amplitude;
    globals.skin_dynamics[3] = settings_.skin_tension_gain;
    globals.flags |= kFrameFlagSkinDynamics;
  }
  // Hybrid ReSTIR DI decision happens before the globals upload below; the
  // graph passes record later under the same flag.
  bool restir_active = settings_.restir_di && rt_available_ &&
                       restir_di_.available() && !path_trace &&
                       light_count > 0 && raytracing_ &&
                       raytracing_->tlas(tlas_slot);
  if (restir_active)
    globals.flags |= kFrameFlagRestirDi;
  {
    // Froxel slicing: exponential view-z between the near plane and 500 m.
    constexpr f32 kNear = 0.1f, kFar = 500.0f;
    f32 scale = static_cast<f32>(kClusterSlices) / ::log2f(kFar / kNear);
    globals.cluster_params[0] = scale;
    globals.cluster_params[1] = -::log2f(kNear) * scale;
    globals.cluster_params[2] =
        static_cast<f32>(render_width_) / static_cast<f32>(kClusterTilesX);
    globals.cluster_params[3] =
        static_cast<f32>(render_height_) / static_cast<f32>(kClusterTilesY);
  }
  base::MemCopy(frame.globals.mapped, &globals, sizeof(globals));
  prev_view_proj_ = view_proj;
  has_prev_frame_ = true;

  // Skinning palette for every skinned draw this frame, read by device address.
  if (!view.bone_matrices.empty() && frame.bone_palette.mapped) {
    u32 count = rx::Min<u32>(static_cast<u32>(view.bone_matrices.size()),
                              kMaxFrameBones);
#ifndef NDEBUG
    // The skinning path blends raw upper-3x3 blocks and carries normals with
    // them, which only holds for a similarity transform (see FrameView::
    // bone_matrices). Anisotropic scale in a bone shades wrong silently, so
    // say so once instead of leaving it to be found in a screenshot.
    static bool warned_anisotropic_bone = false;
    if (!warned_anisotropic_bone) {
      for (u32 i = 0; i < count && !warned_anisotropic_bone; ++i) {
        const f32* m = view.bone_matrices[i].m;
        const f32 sx = ::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        const f32 sy = ::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        const f32 sz = ::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        const f32 lo = rx::Min({sx, sy, sz});
        const f32 hi = rx::Max({sx, sy, sz});
        if (lo > 1e-6f && hi > lo * 1.01f) {
          RX_WARN(
              "bone {} carries anisotropic scale ({:.3f}/{:.3f}/{:.3f}); skinned "
              "normals assume translate*rotate*uniform-scale and will shade wrong",
              i, sx, sy, sz);
          warned_anisotropic_bone = true;
        }
      }
    }
#endif
    base::MemCopy(frame.bone_palette.mapped, view.bone_matrices.data(),
                count * sizeof(Mat4));
  }
  // Last frame's poses, in the palette's second half. Written every frame like
  // the first, so nothing in the ring is read across a frame boundary; a draw
  // whose app supplied no history keeps prev_skin_offset == skin_offset (see
  // PrevSkinOffset) and never reads this range at all.
  prev_bone_base_ = 0;
  if (!view.prev_bone_matrices.empty() && frame.bone_palette.mapped) {
    const u32 count = rx::Min<u32>(static_cast<u32>(view.prev_bone_matrices.size()),
                                    kMaxFrameBones);
    prev_bone_base_ = kMaxFrameBones;
    base::MemCopy(static_cast<u8*>(frame.bone_palette.mapped) +
                    static_cast<u64>(prev_bone_base_) * sizeof(Mat4),
                view.prev_bone_matrices.data(), count * sizeof(Mat4));
  }
  // Active morph target weights for every morphed draw, read by device address.
  if (!view.morph_weights.empty() && frame.morph_weights.mapped) {
    u32 count = rx::Min<u32>(static_cast<u32>(view.morph_weights.size()),
                              kMaxFrameMorphWeights);
    base::MemCopy(frame.morph_weights.mapped, view.morph_weights.data(),
                count * sizeof(MorphWeight));
  }
  // Per-draw transforms for every pass that walks view.draws this frame. One
  // arena, filled once: the prepass, scene, shadow, transparent and water
  // passes all index the same records with the base their push block carries.
  UploadDrawRecords(frame, view);

  ResourceHandle scene_color =
      graph_.CreateTexture({.name = "scene_color",
                            .format = kSceneColorFormat,
                            .width = render_width_,
                            .height = render_height_});
  // The g-buffer aux targets only exist for the raster path; the path tracer
  // writes scene_color directly, so leaving them uncreated keeps the transient
  // pool from allocating images no pass touches.
  ResourceHandle motion = kInvalidResource, depth = kInvalidResource;
  ResourceHandle normals = kInvalidResource, depth_export = kInvalidResource;
  ResourceHandle shadow_atlas = kInvalidResource;
  if (csm_active) {
    shadow_atlas = graph_.CreateTexture({.name = "shadow_atlas",
                                         .format = ShadowPass::kAtlasFormat,
                                         .width = shadow_.atlas_width(),
                                         .height = shadow_.atlas_height()});
  }
  if (!path_trace) {
    motion = graph_.CreateTexture({.name = "motion",
                                   .format = kMotionFormat,
                                   .width = render_width_,
                                   .height = render_height_});
    depth = graph_.CreateTexture({.name = "depth",
                                  .format = kDepthFormat,
                                  .width = render_width_,
                                  .height = render_height_});
    normals = graph_.CreateTexture({.name = "normals",
                                    .format = kNormalFormat,
                                    .width = render_width_,
                                    .height = render_height_});
    // Raw reversed z exported by the prepass; every depth consumer samples
    // this so the real depth attachment never changes layout mid frame
    // (sampling round trips corrupt its compression metadata on nvidia).
    depth_export = graph_.CreateTexture({.name = "depth_export",
                                         .format = gpu::Format::kR32Float,
                                         .width = render_width_,
                                         .height = render_height_});
  }
  // Skin diffuse export: third scene-pass attachment, diffused by the
  // screen-space sss blur after the opaque pass (raster path only).
  ResourceHandle skin_diffuse = kInvalidResource;
  if (!path_trace) {
    skin_diffuse =
        graph_.CreateTexture({.name = "skin_diffuse",
                              .format = MeshPipeline::kSkinDiffuseFormat,
                              .width = render_width_,
                              .height = render_height_});
  }

  // kMsaa: multisampled twins for the geometry window (prepass + scene). The
  // plain handles above keep flowing to every downstream consumer as the
  // resolved single-sampled versions; only the two geometry passes and the
  // resolve/depth-rebuild passes below touch these.
  ResourceHandle geom_depth = depth, geom_normals = normals,
                 geom_motion = motion, geom_depth_export = depth_export,
                 geom_scene = scene_color, geom_skin = skin_diffuse;
  if (msaa) {
    geom_depth = graph_.CreateTexture({.name = "depth_ms",
                                       .format = kDepthFormat,
                                       .width = render_width_,
                                       .height = render_height_,
                                       .samples = msaa_samples});
    geom_normals = graph_.CreateTexture({.name = "normals_ms",
                                         .format = kNormalFormat,
                                         .width = render_width_,
                                         .height = render_height_,
                                         .samples = msaa_samples});
    geom_motion = graph_.CreateTexture({.name = "motion_ms",
                                        .format = kMotionFormat,
                                        .width = render_width_,
                                        .height = render_height_,
                                        .samples = msaa_samples});
    geom_depth_export = graph_.CreateTexture({.name = "depth_export_ms",
                                              .format = gpu::Format::kR32Float,
                                              .width = render_width_,
                                              .height = render_height_,
                                              .samples = msaa_samples});
    geom_scene = graph_.CreateTexture({.name = "scene_color_ms",
                                       .format = kSceneColorFormat,
                                       .width = render_width_,
                                       .height = render_height_,
                                       .samples = msaa_samples});
    geom_skin =
        graph_.CreateTexture({.name = "skin_diffuse_ms",
                              .format = MeshPipeline::kSkinDiffuseFormat,
                              .width = render_width_,
                              .height = render_height_,
                              .samples = msaa_samples});
  }

  // Virtual texturing: drain last frame's page requests, stream pages, and
  // record this frame's atlas/indirection uploads + the feedback copy/reset.
  if (!path_trace)
    virtual_texture_.AddToGraph(graph_, frame_index_);

  // Baked decal layers: assign tiles to the receivers on screen and record the
  // stamps queued for them. Records nothing in a frame where nothing changed.
  decal_baker_.AddToGraph(graph_, base::Span(decal_targets_.data(), decal_targets_.size()),
                          frame_slot, frame_index_, decal_atlas_view_,
                          decal_normal_atlas_view_);

  // FFT ocean: evolve the spectrum and rebuild the displacement/normal maps
  // the water shaders sample this frame.
  if (fft_ocean_active)
    ocean_.AddToGraph(graph_, static_cast<f32>(time_seconds_));

  // The persistent foam/ripple field runs later, after the prepass, so its
  // local-interaction phase can read the opaque depth (see below, past the
  // depth export). It still samples the FFT ocean foam recorded just above.

  // Shoreline wetting: update the world-space wet field this frame. Follows the
  // ocean pass so it can read the freshly-built displacement map when active.
  if (shore_wetting_active_) {
    ShoreWetting::Params sp;
    sp.camera_eye = view.camera.eye;
    sp.time = static_cast<f32>(time_seconds_);
    sp.dt = view.frame_delta_seconds;
    sp.drying_time = settings_.shore_drying_time;
    for (u32 i = 0; i < 4; ++i)
      sp.island[i] = settings_.shore_island[i];
    sp.fft_active = fft_ocean_active;
    sp.ocean_displacement =
        fft_ocean_active ? ocean_.displacement_view() : gpu::TextureView{};
    shore_wetting_.AddToGraph(graph_, sp);
  }

  // Underwater caustics: refract a sun-ray grid through the surface onto a
  // reference receiver plane and build the energy-conserving caustic map the
  // opaque scene pass samples. Follows the ocean pass so it can read the fresh
  // displacement/normal maps when the FFT ocean is active.
  if (water_caustics_active_) {
    WaterCaustics::Params cp;
    cp.sun_travel =
        Normalize(Vec3{globals.sun_direction[0], globals.sun_direction[1],
                       globals.sun_direction[2]});
    cp.time = static_cast<f32>(time_seconds_);
    cp.rest_height = settings_.water_rest_height;
    cp.receiver_depth = settings_.water_caustic_receiver_depth;
    cp.fft_active = fft_ocean_active;
    cp.ocean_displacement =
        fft_ocean_active ? ocean_.displacement_view() : gpu::TextureView{};
    cp.ocean_normal =
        fft_ocean_active ? ocean_.normal_foam_view() : gpu::TextureView{};
    water_caustics_.AddToGraph(graph_, cp);
  }

  // Aurora contribution to the environment bake: the CPU-side night factor
  // (the same smoothstep sky.ps applies to its screen-space copy) zeroes the
  // term whenever the sun is up, so a daylight scene never re-bakes for it.
  f32 env_aurora = 0.0f;
  if (settings_.weather.aurora && !interior) {
    f32 night = settings_.night;
    if (night < 0.0f) { // legacy fallback: infer from the sun's elevation
      f32 to_sun_y = -applied_sun_direction_.y;
      night = rx::Clamp((0.04f - to_sun_y) / 0.14f, 0.0f, 1.0f);
      night = night * night * (3.0f - 2.0f * night);
    }
    env_aurora =
        settings_.weather.aurora_intensity * rx::Clamp(night, 0.0f, 1.0f);
  }
  // An active aurora writhes: refresh the cubemap whenever its 0.4 s animation
  // step ticks over, so the curtains also move in the IBL and reflections. The
  // moment it fades to zero the environment re-bakes once more, so switching
  // the aurora off does not leave the last green bake in the IBL forever.
  constexpr f64 kAuroraBakeStep = 0.4;
  if (env_aurora > 0.0f &&
      ::floor(time_seconds_ / kAuroraBakeStep) !=
          ::floor((time_seconds_ - view.frame_delta_seconds) /
                     kAuroraBakeStep)) {
    environment_dirty_ = true;
  }
  if (env_aurora <= 0.0f && prev_env_aurora_ > 0.0f)
    environment_dirty_ = true;
  prev_env_aurora_ = env_aurora;
  if (environment_dirty_ && (settings_.ibl || settings_.sky)) {
    environment_dirty_ = false;
    env_baked_sun_direction_ = applied_sun_direction_;
    env_baked_sun_intensity_ = applied_sun_intensity_;
    env_baked_sun_color_ = applied_sun_color_;
    Vec3 env_sun = applied_sun_direction_;
    f32 env_intensity = applied_sun_intensity_;
    Vec3 env_color = applied_sun_color_;
    f32 env_time = static_cast<f32>(time_seconds_);
    graph_.AddPass(
        "env_update", [](RenderGraph::PassBuilder &) {},
        [this, env_sun, env_intensity, env_color, env_aurora,
         env_time](PassContext &ctx) {
          environment_->RecordUpdate(*ctx.cmd, env_sun, env_intensity,
                                     env_color, env_aurora, env_time);
        });
  }

  // RCGI contributes to the TLAS build only in hardware mode; the software SDF
  // path has no ray query and (on non-ray-query devices) no raytracing context.
  if (rt_shadows || rtao_active || ddgi_active ||
      (rcgi_active && !rcgi_software) || water_pipeline_active ||
      reflections_active || path_trace || fog_active || precip_rt) {
    // Solid-angle instance culling + distance LOD are realtime-only shaping of
    // the ray-traced scene; the path tracer keeps every instance at LOD0 for
    // reference correctness. RX_RT_CULL / RX_RT_LOD_NEAR gate them
    // independently.
    const bool rt_cull_on = RtCullOpt && !path_trace;
    // RT LOD is independent of the raster force_lod0_for_tlas knob (which keeps
    // the rasterizer at LOD0 so screen-referenced rays self-intersect cleanly):
    // rays get LOD0 inside RX_RT_LOD_NEAR and coarsen past it. Path tracing
    // keeps LOD0 everywhere. RX_RT_LOD_NEAR <= 0 disables coarsening.
    const bool rt_lod_on = !path_trace && RtLodNear > 0.0f;
    const f32 lod_near = RtLodNear;
    rt_cull_.Configure(rt_cull_on, RtCullStart, RtCullAngle);
    rt_cull_.BeginFrame(view.camera.eye);
    const Vec3 eye = view.camera.eye;
    // Model-space bounding sphere of a mesh as a Vec3 center.
    auto mesh_center = [](const gpu::GpuMesh &m) {
      return Vec3{m.bounds_center[0], m.bounds_center[1], m.bounds_center[2]};
    };
    // Distance from the camera to an instance's world-space bounding centre.
    auto center_distance = [&](const gpu::GpuMesh &m, const Mat4 &t) {
      const Vec3 c = mesh_center(m);
      const f32 *mm = t.m;
      const f32 wx = mm[0] * c.x + mm[4] * c.y + mm[8] * c.z + mm[12];
      const f32 wy = mm[1] * c.x + mm[5] * c.y + mm[9] * c.z + mm[13];
      const f32 wz = mm[2] * c.x + mm[6] * c.y + mm[10] * c.z + mm[14];
      const f32 dx = wx - eye.x, dy = wy - eye.y, dz = wz - eye.z;
      return ::sqrtf(dx * dx + dy * dy + dz * dz);
    };
    // Resolves the RT LOD for one instance: LOD0 inside the near radius (raster
    // and rays agree there, avoiding the self-intersection disparity the AC
    // Shadows team calls out), a coarser LOD past it. Fills custom_index/lod
    // for the TLAS instance, lazily building the LOD BLAS + record on first
    // need and falling back to LOD0 when the mesh has no usable geometry at
    // that LOD.
    auto select_rt = [&](u64 key, gpu::GpuMesh &m, const Mat4 &t, u32 &out_lod,
                         u32 &out_index) {
      out_lod = 0;
      out_index = m.bindless_index;
      if (!rt_lod_on || m.lods.empty())
        return;
      const f32 dist = center_distance(m, t);
      if (dist <= lod_near)
        return;
      const u32 lod = SelectLod(m, dist);
      if (lod == 0)
        return;
      const u32 idx = EnsureLodRtGeometry(key, m, lod);
      if (idx == BindlessRegistry::kInvalidIndex)
        return; // keep LOD0
      out_lod = lod;
      out_index = idx;
    };

    // Compute-skin the requested actors into their own vertex buffers and get
    // their BLASes reserved, before anything below asks for their custom index.
    // Allocation happens here, on the build thread; only the dispatches and the
    // build/refit are recorded (see the skin_deform pass below).
    const u32 skinned_actors =
        skin_requests.empty()
            ? 0u
            : skinned_rt_.Prepare(*device_, *bindless_, *material_system_,
                                  *raytracing_, meshes_, skin_requests);

    base::Vector<RayTracingContext::Instance> instances;
    instances.reserve(view.draws.size() + instances_.instance_count());
    for (const DrawItem &item : view.draws) {
      // A posed actor is instanced against its own refit BLAS below; its mesh's
      // bind-pose structure must not enter the tlas as well, or every ray sees
      // a T-posed twin standing where she is.
      if (item.rt_skin != 0 && skinned_rt_.active(item.rt_skin))
        continue;
      gpu::GpuMesh *mesh = meshes_.find(item.mesh);
      // no_rt grass-like fill stays out of the realtime tlas; when the path
      // tracer is active it joins with a path-trace-only instance mask, so
      // realtime rays (shadows/RTAO/reflections/fog/water) skip it either way:
      // they trace with RX_RAY_MASK_REALTIME.
      if (!mesh || mesh->all_blend || (mesh->no_rt && !path_trace))
        continue;
      // Per-draw instances are frustum-culled and few, so test solid angle
      // inline. no_rt fill is path-trace-only (never culled: culling is off
      // under the path tracer).
      if (rt_cull_on && !mesh->no_rt &&
          !rt_cull_.DrawVisible(item.transform, mesh_center(*mesh),
                                mesh->bounds_radius))
        continue;
      u8 mask = mesh->no_rt
                    ? static_cast<u8>(kRayMaskPathTrace)
                    : static_cast<u8>(kRayMaskRealtime | kRayMaskPathTrace);
      u32 lod = 0, index = mesh->bindless_index;
      if (!mesh->no_rt)
        select_rt(item.mesh, *mesh, item.transform, lod, index);
      instances.push_back({.mesh_key = item.mesh,
                           .custom_index = index,
                           .mask = mask,
                           .lod = lod,
                           .transform = item.transform,
                           .previous_transform = item.prev_transform,
                           .previous_mesh = index});
      // Vegetation stand-in: a second instance on the opaque-approximation
      // BLAS, masked kRayMaskApprox so only the realtime diffuse/AO/shadow rays
      // hit it. Only at LOD0: distant LODs are built force-opaque and need no
      // stand-in.
      if (mesh->rt_approx && lod == 0) {
        instances.push_back({.mesh_key = item.mesh,
                             .custom_index = mesh->rt_approx_bindless,
                             .mask = static_cast<u8>(kRayMaskApprox),
                             .approx = true,
                             .transform = item.transform});
      }
    }
    // Posed skinned actors: one instance each, on the actor's own structure and
    // its own bindless record, so hit shaders read ITS deformed vertices.
    // Never LOD'd or approximated (skinned meshes carry neither) and never
    // solid-angle culled: the handful of actors a game asks for are the ones it
    // cares about seeing in a reflection.
    for (const DrawItem &item : view.draws) {
      if (item.rt_skin == 0)
        continue;
      const u32 index = skinned_rt_.custom_index(item.rt_skin);
      if (index == SkinnedRayTracing::kInvalidIndex)
        continue;
      instances.push_back(
          {.mesh_key = skinned_rt_.blas_key(item.rt_skin),
           .custom_index = index,
           .mask = static_cast<u8>(kRayMaskRealtime | kRayMaskPathTrace),
           .skinned = true,
           .transform = item.transform,
           .previous_transform = item.prev_transform,
           .previous_mesh = skinned_rt_.previous_custom_index(item.rt_skin)});
    }
    const base::Vector<InstanceStore::Group> &groups = instances_.groups();
    for (u32 gi = 0; gi < groups.size(); ++gi) {
      const InstanceStore::Group &group = groups[gi];
      if (!group.alive)
        continue;
      gpu::GpuMesh *mesh = meshes_.find(group.mesh);
      if (!mesh || mesh->all_blend || (mesh->no_rt && !path_trace))
        continue;
      const u8 mask =
          mesh->no_rt ? static_cast<u8>(kRayMaskPathTrace)
                      : static_cast<u8>(kRayMaskRealtime | kRayMaskPathTrace);
      // Time-sliced solid-angle sweep for the (potentially thousands of) static
      // instances in this group; the persistent bitmask drives inclusion every
      // frame while only a slice is re-tested. no_rt fill is path-trace-only,
      // so culling never applies to it (path tracing disables culling).
      const bool group_cull = rt_cull_on && !mesh->no_rt;
      const base::Vector<u8> *visible =
          group_cull ? &rt_cull_.UpdateGroup(
                           gi, group.generation, group.revision,
                           base::Span(group.transforms.data(), group.transforms.size()),
                           mesh_center(*mesh), mesh->bounds_radius)
                     : nullptr;
      for (u32 ii = 0; ii < group.transforms.size(); ++ii) {
        if (visible && !(*visible)[ii])
          continue;
        const Mat4 &transform = group.transforms[ii];
        u32 lod = 0, index = mesh->bindless_index;
        if (!mesh->no_rt)
          select_rt(group.mesh, *mesh, transform, lod, index);
        instances.push_back({.mesh_key = group.mesh,
                             .custom_index = index,
                             .mask = mask,
                             .lod = lod,
                             .transform = transform,
                             .previous_transform = transform,
                             .previous_mesh = index});
        if (mesh->rt_approx && lod == 0) {
          instances.push_back({.mesh_key = group.mesh,
                               .custom_index = mesh->rt_approx_bindless,
                               .mask = static_cast<u8>(kRayMaskApprox),
                               .approx = true,
                               .transform = transform});
        }
      }
    }
    if (path_trace) {
      path_scene_moved = pt_scene_history_.Update(
          instances, base::Span(view.bone_matrices.data(), view.bone_matrices.size()));
    } else {
      pt_scene_history_ = {};
    }
    // Skinning + BLAS refits, on the graphics timeline, recorded ahead of the
    // TLAS build that reads the structures they write.
    //
    // Placement is load-bearing, not incidental. The render graph forks the
    // async queue at the FIRST async-flagged pass and the fork semaphore orders
    // everything recorded before it, so this pass must precede that fork or the
    // async TLAS build can read a half-written structure. tlas_build below is
    // the earliest pass in the whole graph that can be flagged async (the only
    // others are ddgi, rcgi and light_grid, all added after it), and this is
    // added immediately before it. Anything that adds an async pass ahead of
    // this point has to move this with it.
    if (skinned_actors > 0) {
      graph_.AddPass(
          "skin_deform", [](RenderGraph::PassBuilder &) {},
          [this, palette = frame.bone_palette](PassContext &ctx) {
            skinned_rt_.Record(*ctx.cmd, *raytracing_, palette);
          });
      static bool logged_async_skin = false;
      if (async_tlas && !logged_async_skin) {
        logged_async_skin = true;
        RX_INFO("skinned-rt: {} actor(s) with the async tlas build active; their "
                "ray-traced pose is one frame behind the raster one",
                skinned_actors);
      }
    }
    // Grow the TLAS now, on the build thread, so the record-time BuildTlas
    // never stalls the device or frees buffers mid command list (which races
    // the frame ring and corrupts the image). Spikes here when two worlds
    // stream in.
    if (raytracing_->ReserveTlas(tlas_build_slot,
                                 static_cast<u32>(instances.size()))) {
      graph_.AddPass(
          "tlas_build",
          [async_tlas](RenderGraph::PassBuilder &b) {
            if (async_tlas)
              b.Async(); // build next frame's slot on the compute queue
          },
          [this, tlas_build_slot, frame_index = frame_index_,
           instances = base::move(instances)](PassContext &ctx) {
            raytracing_->BuildTlas(*ctx.cmd, tlas_build_slot, frame_index,
                                   instances);
          });
    } else if (path_trace) {
      path_scene_moved = true;
      pt_scene_history_ = {};
    }
  }

  // DDGI right after the TLAS (its only same-frame dependency): flagged async
  // it forks onto the compute queue here and overlaps everything up to the
  // join before its first consumer (the reflection trace / scene pass).
  bool ddgi_async = ddgi_active && !path_trace && settings_.async_compute &&
                    device_->caps().async_compute;
  if (ddgi_active && !path_trace) {
    ddgi_->AddToGraph(graph_, *raytracing_, tlas_slot, view.camera.eye,
                      applied_sun_direction_, applied_sun_intensity_,
                      applied_sun_color_, frame_index_, ddgi_async);
  }

  // SDF clipmap composition (RX_SDF): min-blend the frame's instance SDFs into
  // the camera-following clipmap. This is the software-trace world side and is
  // independent of ray tracing (the whole point). One clip recomposited per
  // frame plus any clip that snapped this frame. Composed BEFORE the RCGI world
  // pass so the software probe trace reads this frame's clipmap (there is no
  // tlas_build to anchor after in software mode). Both use the kGeneral manual-
  // barrier discipline, so submission order is execution order on the queue.
  if (sdf_clipmap_ && sdf_available_) {
    base::Vector<SdfClipmap::Instance> sdf_instances;
    sdf_instances.reserve(view.draws.size() + instances_.instance_count());
    for (const DrawItem &item : view.draws) {
      if (sdf_scene_->Find(item.mesh))
        sdf_instances.push_back({item.mesh, item.transform});
    }
    for (const InstanceStore::Group &group : instances_.groups()) {
      if (!group.alive || !sdf_scene_->Find(group.mesh))
        continue;
      for (const Mat4 &transform : group.transforms) {
        sdf_instances.push_back({.mesh_key = group.mesh,
                                 .transform = transform,
                                 .bounded_quality = true});
      }
    }
    sdf_clipmap_->AddComposeToGraph(graph_, *sdf_scene_,
                                    base::move(sdf_instances), view.camera.eye,
                                    frame_index_);
  }

  // RCGI world side: cascaded light grid + spatial-hash radiance cache +
  // irradiance cascades. On RT hardware it can fork onto the async compute
  // queue and overlap up to the JoinAsync before its first consumer (the M2
  // gather). In software mode it traces the SDF clipmap (no tlas) on the main
  // timeline.
  bool rcgi_async = rcgi_world && !rcgi_software && settings_.async_compute &&
                    device_->caps().async_compute;
  if (rcgi_world) {
    light_grid_.AddToGraph(graph_, frame.lights, light_count, view.camera.eye,
                           frame_index_, rcgi_async);
    rcgi_->SetInteriorVolumes(interior_volumes_,
                              frame_index_); // game interior bounds
    rcgi_->set_gather_scale(
        static_cast<u32>(RcgiGatherScaleOpt));   // item 23: half/quarter res
    rcgi_->set_denoise_mask(RcgiDenoiseMaskOpt); // item 22: cross-class mask
    RcgiSystem::FrameConfig rcgi_cfg;
    rcgi_cfg.authored_interior = settings_.interior;
    rcgi_cfg.interior = settings_.interior && RcgiInteriorOpt;
    rcgi_cfg.interior_ambient = settings_.interior_ambient;
    rcgi_cfg.relocate = RcgiRelocateOpt;
    rcgi_cfg.classify =
        RcgiInteriorOpt; // volume classification shares the interior gate
    rcgi_cfg.probe_ao = RcgiProbeAoOpt;
    // Interior cells author their own directional (XCLL/LGTM) and suppress the
    // sky sun; feed RCGI the same authored interior sun the raster path
    // selected (above) instead of applied_sun_*, or an outdoor sun leaks bounce
    // onto an interior with zero directional intensity.
    Vec3 rcgi_sun_dir = settings_.interior
                            ? Normalize(settings_.interior_directional_dir)
                            : applied_sun_direction_;
    f32 rcgi_sun_int = settings_.interior
                           ? settings_.interior_directional_intensity
                           : applied_sun_intensity_;
    Vec3 rcgi_sun_col = settings_.interior
                            ? settings_.interior_directional_color
                            : applied_sun_color_;
    rcgi_->AddToGraph(graph_, raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), tlas_slot, light_grid_,
                      frame.lights, view.camera.eye, rcgi_sun_dir, rcgi_sun_int,
                      rcgi_sun_col, frame_index_, rcgi_cfg, rcgi_async,
                      rcgi_software ? sdf_clipmap_.Get_UseOnlyIfYouKnowWhatYouareDoing() : nullptr);
  }

  // The path tracer takes over the whole frame: it writes scene_color directly
  // and skips the entire raster path (g-buffer, gi, transparency, aa).
  ResourceHandle lit = scene_color;
  if (path_trace) {
    PathTracer::Frame pt;
    pt.inv_view_proj = globals.inv_view_proj;
    pt.view_proj = view_proj;
    pt.prev_view_proj =
        globals.prev_view_proj; // last frame's, set before the overwrite below
    pt.camera_pos = view.camera.eye;
    pt.sun_direction = settings_.sun_direction;
    pt.sun_intensity = settings_.sun_intensity;
    pt.sun_color = settings_.sun_color;
    pt.sun_radius = settings_.sun_angular_radius;
    pt.frame_index = frame_index_;
    bool moved =
        base::MemCompare(&view_proj, &pt_prev_view_proj_, sizeof(Mat4)) != 0;
    bool lit_changed =
        settings_.sun_intensity != pt_prev_sun_intensity_ ||
        settings_.sun_angular_radius != pt_prev_sun_radius_ ||
        base::MemCompare(&settings_.sun_direction, &pt_prev_sun_direction_, sizeof(Vec3)) != 0 ||
        base::MemCompare(&settings_.sun_color, &pt_prev_sun_color_, sizeof(Vec3)) != 0;
    bool scene_changed = scene_revision_ != pt_prev_scene_revision_;
    bool denoised_path = false;

    // Gameplay reconstruction renderer: own 1-spp gbuffer + temporal
    // accumulation
    // + a-trous denoise + composite. Separate from the brute-force reference
    // and the NRD path. Reference always wins (screenshots); else recon if
    // selected.
    bool recon_path =
        settings_.path_trace_recon && !settings_.path_trace_reference &&
        recon_path_tracer_.available();
    if (recon_path) {
      // Lazily allocate the recon history targets on first use: the mode is off
      // by default and the buffers are large, so they are not created up front.
      recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
      bool rr_active = false;
#if defined(RX_HAS_DLSS)
      // Ray reconstruction replaces the SVGF chain when its snippet loads;
      // lazy-init mirrors the recon targets above.
      if (settings_.path_trace_rr && !rr_init_attempted_) {
        rr_init_attempted_ = true;
        if (!rr_.Initialize(*device_, {render_width_, render_height_})) {
          RX_INFO("dlss-rr unavailable, recon uses the in-tree svgf denoiser");
        }
      }
      rr_active = settings_.path_trace_rr && rr_.available();
#endif
      ReconPathTracer::Frame rf;
      rf.inv_view_proj = globals.inv_view_proj;
      rf.view_proj = view_proj;
      rf.prev_view_proj = globals.prev_view_proj;
      rf.camera_pos = view.camera.eye;
      rf.sun_direction = settings_.sun_direction;
      rf.sun_intensity = settings_.sun_intensity;
      rf.sun_color = settings_.sun_color;
      rf.sun_radius = settings_.sun_angular_radius;
      rf.pixel_spread = 2.0f * ::tanf(view.camera.fov_y * 0.5f) /
                        static_cast<f32>(render_height_);
      rf.spp = settings_.path_trace_spp;
      rf.frame_index = frame_index_;
      if (rr_active) JitterSequence::Sample(frame_index_, 32, &rf.jitter[0], &rf.jitter[1]);
      // Reset on first frame, on (re)activation, AND when switching into recon
      // from another path-trace mode (its ping-pong history was never written
      // by the reference/NRD paths). Never on the day/night drift.
      rf.reset =
          first_frame || !pt_was_active_ || pt_prev_mode_ != 2 || scene_changed;
      rf.current_weight_min = settings_.path_trace_recon_weight;
      rf.max_history = settings_.path_trace_accum;
      rf.atrous_passes = settings_.path_trace_recon_atrous;
      rf.debug_mode = settings_.path_trace_recon_debug;
      // Modes 8/9 visualize the restir reservoir (M / W): the spatial pass
      // substitutes the heatmap, the composite renders it as raw lighting.
      rf.restir = settings_.path_trace_restir || rf.debug_mode >= 8;
      rf.restir_di = settings_.path_trace_restir_di;
      rf.reset_reservoirs = path_scene_moved;
      rf.lights = frame.lights;
      rf.light_count = light_count;
      rf.fog = settings_.fog;
      rf.fog_density = settings_.fog_density;
      rf.fog_height_falloff = settings_.fog_height_falloff;
      rf.fog_base_height = settings_.fog_base_height;
      rf.fog_anisotropy = settings_.fog_anisotropy;
#if defined(RX_HAS_DLSS)
      if (rr_active) {
        ReconPathTracer::ExternalInputs ext;
        recon_path_tracer_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(),
            environment_->sky_view(), environment_->sampler(), scene_color, rf,
            &ext);
        RrDenoiser::Frame rrf;
        rrf.world_to_view = view_mat;
        rrf.view_to_clip = proj;
        rrf.frame_delta_ms = view.frame_delta_seconds * 1000.0f;
        rrf.reset = rf.reset;
        rrf.frame_index = frame_index_;
        rrf.jitter[0] = rf.jitter[0];
        rrf.jitter[1] = rf.jitter[1];
        rr_.AddToGraph(graph_,
                       {ext.color, ext.depth, ext.motion, ext.normals_rough,
                        ext.diffuse_albedo, ext.specular_albedo, ext.specular_hit_distance},
                       scene_color, rrf);
      } else
#endif
      {
        (void)rr_active;
        recon_path_tracer_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(),
            environment_->sky_view(), environment_->sampler(), scene_color, rf);
      }
    }
#if defined(RX_HAS_NRD)
    if (!recon_path && nrd_.available() && !settings_.path_trace_reference) {
      // Playable: spp lighting samples, then NRD's REBLUR_DIFFUSE reprojects
      // history across camera motion (no full reset), so the view stays clean
      // while moving. More spp = lower input variance = less shimmer.
      denoised_path = true;
      pt.spp = settings_.path_trace_spp;
      // Ray-cone spread for texture lod: vertical fov radians per pixel.
      pt.pixel_spread = 2.0f * ::tanf(view.camera.fov_y * 0.5f) /
                        static_cast<f32>(render_height_);
      PathTracer::GbufferTargets t;
      auto guide = [&](const char *name, gpu::Format format) {
        return graph_.CreateTexture({.name = name,
                                     .format = format,
                                     .width = render_width_,
                                     .height = render_height_});
      };
      t.radiance_hitdist =
          guide("pt_radiance", NrdDenoiser::kDiffuseRadianceFormat);
      t.normal_roughness =
          guide("pt_normal_roughness", NrdDenoiser::kNormalRoughnessFormat);
      t.viewz = guide("pt_viewz", NrdDenoiser::kViewZFormat);
      t.motion = guide("pt_motion", kMotionFormat);
      t.albedo = guide("pt_albedo", kSceneColorFormat);
      t.background = guide("pt_background", kSceneColorFormat);
      path_tracer_.AddGbufferPass(graph_, *raytracing_, tlas_slot,
                                  bindless_->set(), environment_->sky_view(),
                                  environment_->sampler(), t, pt);

      NrdDenoiser::FrameSettings fs;
      fs.view_to_clip = proj;
      fs.view_to_clip_prev = prev_proj_;
      fs.world_to_view = view_mat;
      fs.world_to_view_prev = prev_view_;
      fs.jitter[0] = fs.jitter[1] =
          0.0f; // the path tracer shoots un-jittered rays
      fs.jitter_prev[0] = fs.jitter_prev[1] = 0.0f;
      fs.sun_direction = sun;
      fs.frame_index = frame_index_;
      fs.diffuse_accumulated_frames = settings_.path_trace_accum;
      // Restart ONLY on activation / first frame. NOT on lighting change: the
      // day/night cycle nudges the sun every frame, so resetting on that would
      // restart accumulation every frame and the image would never denoise (it
      // would stay 1-spp grainy forever). NRD tracks gradual lighting changes
      // through its own temporal accumulation + antilag instead. Also reset
      // when switching into the NRD path from another mode (stale reprojection
      // history).
      fs.reset =
          first_frame || !pt_was_active_ || pt_prev_mode_ != 1 || scene_changed;
      nrd_.SetFrame(fs);
      ResourceHandle denoised = nrd_.DenoiseDiffuse(
          graph_, t.normal_roughness, t.viewz, t.motion, t.radiance_hitdist);
      path_tracer_.AddCompositePass(graph_, denoised, t.albedo, t.background,
                                    scene_color);

      // The raster path stores these for NRD only when it runs; keep them
      // current for the next path-traced frame's motion vectors and
      // reprojection.
      prev_proj_ = proj;
      prev_view_ = view_mat;
      prev_jitter_[0] = prev_jitter_[1] = 0.0f;
    }
#endif
    if (!denoised_path && !recon_path) {
      // Reference: brute-force accumulation, hard reset on any motion = ground
      // truth. Also reset when switching into reference from another mode.
      pt.reset = !pt_was_active_ || moved || lit_changed || scene_changed || path_scene_moved ||
                 pt_prev_mode_ != 0;
      path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                              environment_->sky_view(), environment_->sampler(),
                              scene_color, pt);
    }
    pt_prev_view_proj_ = view_proj;
    pt_prev_sun_intensity_ = settings_.sun_intensity;
    pt_prev_sun_radius_ = settings_.sun_angular_radius;
    pt_prev_sun_direction_ = settings_.sun_direction;
    pt_prev_sun_color_ = settings_.sun_color;
    pt_prev_scene_revision_ = scene_revision_;
    pt_was_active_ = true;
    pt_prev_mode_ = recon_path ? 2 : (denoised_path ? 1 : 0);
  } else {
    pt_was_active_ = false;
    pt_prev_mode_ = -1;

    // Precipitation sky occlusion: a top-down "what can see the sky" depth map
    // gating the volumetric rain/snow, its splashes and the surface wetness /
    // snow accumulation (dry under bridges, splashes on roofs). Rendered early
    // so every consumer this frame samples fresh cover; re-rendered only when
    // the camera crosses an anchor cell or on a slow cadence.
    {
      const WeatherSettings &weather = settings_.weather;
      const bool weather_marks = weather.precipitation > 0.0f ||
                                 weather.wetness > 0.0f ||
                                 weather.snow_cover > 0.0f;
      // Not gated on `volumetric`: that flag only picks 3D versus screen-space
      // falling precipitation, while the surface wetness / snow passes need the
      // sky-visibility map either way (dry strips under bridges).
      precip_occlusion_active_ = weather_marks &&
                                 precip_occlusion_.available() &&
                                 !settings_.interior && !path_trace;
      if (precip_occlusion_active_) {
        precip_occlusion_.BeginFrame(view.camera.eye, frame_index_);
        precip_occlusion_.AddToGraph(
            graph_, [this, &frame, &view](gpu::CommandList &cmd,
                                          const Mat4 &occl_view_proj) {
              RecordDepthOnlyScene(cmd, occl_view_proj, frame, view);
            });
      }
    }

    u32 shadow_slot = frame_index_ % 2;
    if (csm_active) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      Vec3 up = Cross(right, fwd);
      f32 shadow_aspect =
          static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
      shadow_.Update(view.camera.eye, fwd, right, up, view.camera.fov_y,
                     shadow_aspect, settings_.sun_direction, shadow_slot);
      graph_.AddPass(
          "shadow_cascades",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Write(shadow_atlas, ResourceUsage::kDepthAttachment);
          },
          [this, shadow_atlas, &frame, &view](PassContext &ctx) {
            gpu::TextureView atlas = ctx.graph->image(shadow_atlas).view;
            shadow_.Render(*ctx.cmd, atlas,
                           [this, &frame, &view](gpu::CommandList &cmd,
                                                 const Mat4 &light_view_proj) {
                             RecordDepthOnlyScene(cmd, light_view_proj, frame,
                                                  view);
                           });
          });
    }

    // Local light shadow faces: depth-only renders with a light-radius culled
    // draw list, into the persistent atlas the cluster loop samples.
    if (local_shadows_active_) {
      graph_.AddPass(
          "local_shadows", [](RenderGraph::PassBuilder &) {},
          [this, &frame, &view](PassContext &ctx) {
            local_shadows_.Render(
                *ctx.cmd, shadow_.local_pipeline(),
                [this, &frame, &view](gpu::CommandList &cmd,
                                      const LocalShadows::Face &face) {
                  gpu::BindingSetHandle bound_material{};
                  // LocalShadows::Render bound the passed masked static
                  // pipeline.
                  gpu::PipelineHandle bound_pipeline = shadow_.local_pipeline();
                  cmd.BindTransient(
                      ShadowPass::kDrawRecordSet,
                      {gpu::Bind::StorageBuffer(0, frame.draw_records)});
                  for (const DrawItem &item : view.draws) {
                    const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
                    if (!mesh || mesh->all_blend ||
                        (mesh->no_rt && !mesh->skinned &&
                         !mesh->dynamic_vertices))
                      continue;
                    // Sphere cull against the light's influence: local shadows
                    // only ever see casters inside the radius.
                    Vec3 wc = TransformPoint(item.transform,
                                             {mesh->bounds_center[0],
                                              mesh->bounds_center[1],
                                              mesh->bounds_center[2]});
                    const f32 *m = item.transform.m;
                    f32 sx = ::sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                    f32 sy = ::sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                    f32 sz =
                        ::sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                    f32 wr =
                        mesh->bounds_radius * rx::Max(sx, rx::Max(sy, sz));
                    Vec3 d{wc.x - face.light_pos.x, wc.y - face.light_pos.y,
                           wc.z - face.light_pos.z};
                    f32 reach = face.light_radius + wr;
                    if (wr > 0.0f &&
                        d.x * d.x + d.y * d.y + d.z * d.z > reach * reach)
                      continue;

                    bool draw_skinned =
                        mesh->skinned && item.skin_offset >= 0 &&
                        static_cast<bool>(shadow_.local_skinned_pipeline());
                    ShadowPass::DrawPush draw_push{};
                    draw_push.draw_index =
                        static_cast<u32>(&item - view.draws.data()) + 1u;
                    if (draw_skinned) {
                      draw_push.skin_offset =
                          static_cast<u32>(item.skin_offset);
                      draw_push.bone_address = frame.bone_palette.address;
                    }
                    cmd.PushConstants(&draw_push, sizeof(draw_push));
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    if (draw_skinned) {
                      cmd.BindVertexBuffer(1, mesh->skinning);
                    }
                    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
                    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
                      if (submesh.blend)
                        continue;
                      // Depth-only for opaque casters, alpha-test only for
                      // masked.
                      gpu::PipelineHandle pipeline =
                          draw_skinned
                              ? shadow_.local_skinned_pipeline(
                                    submesh.alpha_mask)
                              : shadow_.local_pipeline(submesh.alpha_mask);
                      if (!(pipeline == bound_pipeline)) {
                        cmd.BindPipeline(pipeline);
                        bound_pipeline = pipeline;
                      }
                      if (submesh.alpha_mask) {
                        gpu::BindingSetHandle material =
                            material_system_->set(submesh.material);
                        if (!(material == bound_material)) {
                          cmd.BindSet(0, material);
                          bound_material = material;
                        }
                      }
                      cmd.DrawIndexed(submesh.index_count, 1,
                                      submesh.index_offset, 0, 0);
                    }
                  }
                  for (const InstanceStore::Group &group :
                       instances_.groups()) {
                    if (!group.alive)
                      continue;
                    const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
                    if (!mesh || mesh->all_blend ||
                        (mesh->no_rt && !mesh->dynamic_vertices))
                      continue;
                    const Vec3 delta = group.bounds_center - face.light_pos;
                    const f32 reach = face.light_radius + group.bounds_radius;
                    if (group.cullable && Dot(delta, delta) > reach * reach)
                      continue;
                    f32 face_planes[5][4];
                    ExtractFrustumPlanes(face.view_proj, face_planes);
                    if (group.cullable &&
                        SphereOutsideFrustum(face_planes, group.bounds_center,
                                             group.bounds_radius))
                      continue;
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindVertexBuffer(1, group.buffer);
                    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
                    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
                      if (submesh.blend)
                        continue;
                      const gpu::PipelineHandle pipeline =
                          shadow_.local_instanced_pipeline(submesh.alpha_mask);
                      if (!(pipeline == bound_pipeline)) {
                        cmd.BindPipeline(pipeline);
                        bound_pipeline = pipeline;
                      }
                      if (submesh.alpha_mask) {
                        const gpu::BindingSetHandle material =
                            material_system_->set(submesh.material);
                        if (!(material == bound_material)) {
                          cmd.BindSet(0, material);
                          bound_material = material;
                        }
                      }
                      cmd.DrawIndexed(submesh.index_count,
                                      static_cast<u32>(group.transforms.size()),
                                      submesh.index_offset, 0, 0);
                    }
                  }
                });
          });
    }

    // GPU-driven culling: build one indirect command per opaque submesh and one
    // cull instance per opaque mesh, in the exact order the prepass/scene draw
    // loops walk view.draws, then let a compute pass zero the culled
    // instanceCounts.
    u32 cull_slot = frame_index_ % 2;
    gpu_cull_.ResizeDepth(*device_, render_width_, render_height_);
    const gpu::GpuBuffer &cull_commands = gpu_cull_.command_buffer(cull_slot);
    u32 cull_instance_count = 0;
    {
      GpuCull::Instance *insts = gpu_cull_.instances(cull_slot);
      GpuCull::Command *cmds = gpu_cull_.commands(cull_slot);
      u32 cmd_total = 0;
      for (const DrawItem &item : view.draws) {
        const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
        if (!mesh || mesh->all_blend)
          continue;
        if (cull_instance_count >= GpuCull::kMaxInstances ||
            cmd_total >= GpuCull::kMaxCommands)
          break;
        GpuCull::Instance &inst = insts[cull_instance_count];
        inst.model = item.transform;
        inst.bounds[0] = mesh->bounds_center[0];
        inst.bounds[1] = mesh->bounds_center[1];
        inst.bounds[2] = mesh->bounds_center[2];
        inst.bounds[3] = mesh->bounds_radius;
        inst.first_cmd = cmd_total;
        inst.cull_disabled = (mesh->skinned || mesh->morph_target_count > 0 ||
                              mesh->bounds_radius <= 0.0f)
                                 ? 1u
                                 : 0u;
        inst.pad = 0;

        // Default to lod 0 (finest): we stream and render the highest detail
        // the game ships. Distance-based downgrade is opt-in
        // (settings_.distance_lod). Skinned, morphed and rt-shaded meshes
        // always stay on lod 0 (their bounds deform / the morph deltas index
        // lod 0 vertices / the tlas is built from lod 0). The submesh count
        // matches lod 0 so the prepass/scene draw loops issue one indirect per
        // submesh.
        bool fixed_lod = !settings_.distance_lod || mesh->skinned ||
                         mesh->morph_target_count > 0 ||
                         (force_lod0_for_tlas && !mesh->no_rt);
        u32 lod = 0;
        if (!fixed_lod) {
          Vec3 wc = TransformPoint(item.transform, {mesh->bounds_center[0],
                                                    mesh->bounds_center[1],
                                                    mesh->bounds_center[2]});
          Vec3 d = view.camera.eye - wc;
          lod = SelectLod(*mesh, ::sqrtf(d.x * d.x + d.y * d.y + d.z * d.z));
        }
        const base::Vector<gpu::GpuSubmesh> &lod_subs =
            lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
        i32 vtx_off =
            lod == 0 ? 0 : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);

        u32 mesh_cmds = 0;
        u32 k = 0;
        for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
          if (!submesh.blend) {
            if (cmd_total >= GpuCull::kMaxCommands)
              break;
            const gpu::GpuSubmesh &s = k < lod_subs.size() ? lod_subs[k] : submesh;
            cmds[cmd_total] = {s.index_count, 1u, s.index_offset, vtx_off, 0u};
            ++cmd_total;
            ++mesh_cmds;
          }
          ++k;
        }
        inst.cmd_count = mesh_cmds;
        if (mesh_cmds > 0)
          ++cull_instance_count;
      }
      cull_total_commands_ = cmd_total;
      // This slot's previous cull finished (the frame fence was waited on), so
      // its count is valid; read it before AddToGraph resets the buffer.
      cull_visible_ =
          settings_.gpu_culling ? gpu_cull_.last_visible(cull_slot) : cmd_total;
    }
    // Occlusion needs last frame's depth snapshot, and something to test
    // against it. A frame that only draws through the scene hooks (a voxel game
    // with its own culling) has no draws, and the hi-z build alone costs
    // ~1.7 ms on a Steam Deck.
    const bool occlusion_wanted = settings_.gpu_culling && settings_.gpu_occlusion &&
                                  !view.draws.empty();
    bool cull_occlusion = occlusion_wanted && has_prev_frame_ && cull_depth_snapshot_;
    ResourceHandle cull_hiz = cull_occlusion
                                  ? gpu_cull_.BuildHiZ(graph_, cull_slot)
                                  : kInvalidResource;
    const f32 cull_proj_scale[2] = {proj.m[0], proj.m[5]};
    gpu_cull_.AddToGraph(graph_, view_proj, globals.prev_view_proj,
                         cull_proj_scale, view.camera.eye, cull_instance_count,
                         settings_.gpu_culling, cull_occlusion, cull_hiz,
                         cull_slot);

    bool grass_active = false;
    if (settings_.procedural_grass && view.grass_domain &&
        procedural_grass_.EnsureSampleCount(*device_, msaa_samples)) {
      ProceduralGrass::Frame grass_frame;
      grass_frame.view_proj = globals.view_proj;
      grass_frame.prev_view_proj = globals.prev_view_proj;
      grass_frame.camera_pos = view.camera.eye;
      grass_frame.sun_direction = sun;
      grass_frame.sun_color = {globals.sun_color[0], globals.sun_color[1],
                               globals.sun_color[2]};
      grass_frame.sun_intensity = globals.sun_direction[3];
      grass_frame.ambient = globals.sun_color[3];
      grass_frame.time = static_cast<f32>(time_seconds_);
      grass_frame.delta_time = view.frame_delta_seconds;
      grass_frame.jitter[0] = globals.jitter[0];
      grass_frame.jitter[1] = globals.jitter[1];
      grass_frame.wind_speed = settings_.weather.wind_speed;
      grass_frame.wind_yaw = settings_.weather.wind_yaw;
      grass_frame.gustiness = settings_.weather.gustiness;
      // World width of one pixel at unit view depth, for the sub-pixel blade
      // width clamp. m[5] is negative under the reversed-Z Y-flip projection.
      grass_frame.pixel_scale =
          proj.m[5] != 0.0f
              ? 2.0f / (::fabsf(proj.m[5]) * static_cast<f32>(render_height_))
              : 0.0f;
      grass_active = procedural_grass_.Prepare(
          *view.grass_domain,
          base::Span(view.grass_interactions.data(), view.grass_interactions.size()),
          grass_frame, frame_slot);
      if (grass_active) procedural_grass_.AddGeneration(graph_, frame_slot);
    }

    // Mesh-shader opaque path: drawn this frame if enabled and supported. The
    // task stage reuses the same hi-z the raster cull built (last frame's) for
    // instance occlusion; ms_occ carries the projection scale + hi-z size (z=0
    // disables it). The meshlet pipelines stay single-sampled, so the path sits
    // out kMsaa.
    const bool ms_active =
        settings_.mesh_shader_lod && mesh_pipeline_->has_mesh_shader() && !msaa;
    const bool ms_occlude = ms_active && cull_occlusion;
    f32 ms_occ[4] = {0, 0, 0, 0};
    if (ms_occlude) {
      ms_occ[0] = proj.m[0];
      ms_occ[1] = proj.m[5];
      ms_occ[2] = static_cast<f32>(gpu_cull_.hiz_width());
      ms_occ[3] = static_cast<f32>(gpu_cull_.hiz_height());
    }
    // Frustum planes for the cpu-side skip of off-screen mesh-shader draws.
    f32 ms_planes[5][4];
    ExtractFrustumPlanes(view_proj, ms_planes);

    // Draws every mesh-shader-eligible mesh; shared by the prepass and scene
    // sub-passes (material binding differs via the bind callbacks).
    auto draw_meshlet_instances = [this, &view, &ms_occ, &ms_planes,
                                   &frame](PassContext &ctx) {
      gpu::BindingSetHandle bound{};
      for (const DrawItem &item : view.draws) {
        const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
        if (!mesh || mesh->all_blend || !mesh->has_meshlets)
          continue;
        MeshShaderPush push{};
        push.draw_index = static_cast<u32>(&item - view.draws.data()) + 1u;
        push.meshlets_address = mesh->meshlets.address;
        push.meshlet_vertices_address = mesh->meshlet_vertices.address;
        push.meshlet_triangles_address = mesh->meshlet_triangles.address;
        push.vertices_address = mesh->vertices.address;
        push.bounds[0] = mesh->bounds_center[0];
        push.bounds[1] = mesh->bounds_center[1];
        push.bounds[2] = mesh->bounds_center[2];
        push.bounds[3] = mesh->bounds_radius;
        push.occlusion[0] = ms_occ[0];
        push.occlusion[1] = ms_occ[1];
        push.occlusion[2] = ms_occ[2];
        push.occlusion[3] = ms_occ[3];
        // Distance lod pick; the task stage dispatches the chosen lod range.
        Vec3 ms_wc = TransformPoint(item.transform, {mesh->bounds_center[0],
                                                     mesh->bounds_center[1],
                                                     mesh->bounds_center[2]});
        Vec3 ms_d = view.camera.eye - ms_wc;
        // Cpu frustum skip: a conservative world radius (bounds scaled by the
        // largest transform axis) lets off-screen instances cost no dispatch.
        const f32 *m = item.transform.m;
        f32 sx = ::sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        f32 sy = ::sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        f32 sz = ::sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        f32 ms_radius = mesh->bounds_radius * rx::Max(sx, rx::Max(sy, sz));
        if (ms_radius > 0.0f &&
            SphereOutsideFrustum(ms_planes, ms_wc, ms_radius))
          continue;
        u32 ms_lod =
            SelectLod(*mesh, ::sqrtf(ms_d.x * ms_d.x + ms_d.y * ms_d.y +
                                       ms_d.z * ms_d.z));
        const base::Vector<gpu::GpuSubmesh> &ms_subs =
            ms_lod == 0 ? mesh->submeshes : mesh->lods[ms_lod - 1].submeshes;
        for (const gpu::GpuSubmesh &submesh : ms_subs) {
          if (submesh.blend || submesh.meshlet_count == 0)
            continue;
          gpu::BindingSetHandle material = material_system_->set(submesh.material);
          if (!(material == bound)) {
            mesh_pipeline_->BindMeshMaterial(*ctx.cmd, material);
            bound = material;
          }
          push.meshlet_offset = submesh.meshlet_offset;
          push.meshlet_count = submesh.meshlet_count;
          mesh_pipeline_->DrawMeshlets(*ctx.cmd, push);
        }
      }
    };

    graph_.AddPass(
        "prepass",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(geom_normals, ResourceUsage::kColorAttachment);
          builder.Write(geom_motion, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth_export, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth, ResourceUsage::kDepthAttachment);
          if (ms_occlude)
            builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
        },
        [this, geom_normals, geom_motion, geom_depth_export, geom_depth,
         cull_commands, &frame, &view, ms_active, ms_occlude, cull_hiz,
         globals_set, update_globals_set, frame_slot, draw_meshlet_instances,
         view_proj, force_lod0_for_tlas, grass_active,
         msaa_samples](PassContext &ctx) {
          // First globals-set user this frame: write uniform + tlas + hi-z
          // once.
          update_globals_set(ctx, ms_occlude ? cull_hiz : kInvalidResource,
                             ms_active,
                             /*want_tlas=*/true);

          gpu::ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(geom_normals).view};
          colors[1] = {.view = ctx.graph->image(geom_motion).view};
          colors[2] = {.view = ctx.graph->image(geom_depth_export).view};
          gpu::DepthAttachment depth_attachment{
              .view = ctx.graph->image(geom_depth).view,
              .clear = 0.0f}; // reversed z clears to far = 0
          ctx.cmd->BeginRendering(
              {.extent = {render_width_, render_height_},
               .colors = base::Span(colors, 3),
               .depth = &depth_attachment,
               .shading_rate = vrs_active_ ? vrs_.rate_view() : gpu::TextureView{}});

          // Mesh-shader sub-pass: static opaque meshes, cluster-culled on the
          // gpu.
          if (ms_active) {
            mesh_pipeline_->BindMeshPrepass(*ctx.cmd, globals_set);
            draw_meshlet_instances(ctx);
          }

          // Raster sub-pass: skinned / non-meshlet meshes via gpu-culled
          // indirect draws. Meshes already drawn by the mesh shader are skipped
          // but still advance the cull index so it stays aligned with the cull
          // build order.
          environment_->WriteEnvSet(
              env_prepass_sets_[frame_slot], gpu::TextureView{}, nullptr,
              gpu::TextureView{}, gpu::GpuBuffer{}, 0, gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, 0, gpu::TextureView{}, gpu::GpuBuffer{}, gpu::GpuBuffer{},
              gpu::GpuBuffer{}, gpu::GpuBuffer{}, gpu::TextureView{}, gpu::GpuBuffer{},
              gpu::TextureView{}, gpu::TextureView{}, gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, gpu::TextureView{}, gpu::TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : gpu::TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : gpu::TextureView{});
          mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set,
                                      env_prepass_sets_[frame_slot]);
          gpu::BindingSetHandle bound_material{};
          bool skinned_bound = false;
          bool masked_bound =
              false;              // BindPrepass bound the opaque static variant
          u32 cull_cmd_index = 0; // matches the cull build order
          for (const DrawItem &item : view.draws) {
            const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            // Stay within the commands the cull build wrote; past that the
            // indirect buffer holds no valid command and reading it renders as
            // garbage.
            if (cull_cmd_index >= cull_total_commands_)
              break;
            bool ms_handled = ms_active && mesh->has_meshlets;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            if (!ms_handled) {
              MeshPushConstants push{};
              push.draw_index =
                  static_cast<u32>(&item - view.draws.data()) + 1u;
              if (mesh->terrain_lod) {
                base::MemCopy(push.detail_rect, view.detail_rect,
                            sizeof(push.detail_rect));
              }
              if (draw_skinned && item.skin_offset >= 0) {
                push.bone_address = frame.bone_palette.address;
                push.skin_offset = static_cast<u32>(item.skin_offset);
                push.prev_skin_offset = PrevSkinOffset(item);
              }
              if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                  item.morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(item.morph_offset);
                push.morph_count = item.morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            }
            for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue; // transparency owns its own depth
              if (cull_cmd_index >= cull_total_commands_)
                break; // partial-mesh boundary
              if (!ms_handled) {
                // Masked submeshes take the alpha-test variant; opaque ones
                // keep the discard-free fragment so the draw keeps early-Z.
                if (draw_skinned != skinned_bound ||
                    submesh.alpha_mask != masked_bound) {
                  mesh_pipeline_->SetPrepassVariant(*ctx.cmd, draw_skinned,
                                                    submesh.alpha_mask);
                  skinned_bound = draw_skinned;
                  masked_bound = submesh.alpha_mask;
                }
                gpu::BindingSetHandle material =
                    material_system_->set(submesh.material);
                if (!(material == bound_material)) {
                  mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                  bound_material = material;
                }
                ctx.cmd->DrawIndexedIndirect(
                    cull_commands, cull_cmd_index * GpuCull::kCommandStride, 1,
                    GpuCull::kCommandStride);
              }
              ++cull_cmd_index;
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            const u32 lod =
                !settings_.distance_lod || (force_lod0_for_tlas && !mesh->no_rt)
                    ? 0
                    : SelectLod(*mesh,
                                InstanceGroupDistance(group, view.camera.eye));
            const base::Vector<gpu::GpuSubmesh> &submeshes =
                lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
            const i32 vertex_offset =
                lod == 0 ? 0
                         : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);
            MeshPushConstants push{};
            const gpu::GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const gpu::GpuSubmesh &submesh : submeshes) {
              if (submesh.blend)
                continue;
              mesh_pipeline_->SetInstancedPrepass(*ctx.cmd, submesh.alpha_mask);
              const gpu::BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, vertex_offset, 0);
            }
          }
          if (grass_active)
            procedural_grass_.DrawPrepass(*ctx.cmd, frame_slot, msaa_samples);
          ctx.cmd->EndRendering();
        });

    // kMsaa: resolve the prepass guides to single-sampled (sample 0 - averaging
    // would invent phantom depths/normals at silhouettes) and rebuild the 1x
    // hardware depth every post-resolve raster pass tests against. Motion
    // resolves after the scene pass instead (the sky still writes it there).
    if (msaa) {
      graph_.AddPass(
          "msaa_guides",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(geom_normals, ResourceUsage::kSampledCompute);
            builder.Read(geom_depth_export, ResourceUsage::kSampledCompute);
            builder.Write(normals, ResourceUsage::kStorageWrite);
            builder.Write(depth_export, ResourceUsage::kStorageWrite);
          },
          [this, geom_normals, geom_depth_export, normals,
           depth_export](PassContext &ctx) {
            struct {
              u32 width;
              u32 height;
            } push{render_width_, render_height_};
            ctx.cmd->BindPipeline(msaa_resolve_pipeline_);
            ctx.cmd->BindTransient(
                0,
                {gpu::Bind::SampledView(0, ctx.graph->image(geom_normals).view),
                 gpu::Bind::SampledView(1, ctx.graph->image(geom_depth_export).view),
                 gpu::Bind::Storage(2, ctx.graph->image(normals)),
                 gpu::Bind::Storage(3, ctx.graph->image(depth_export))});
            ctx.cmd->Push(push);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
      graph_.AddPass(
          "msaa_depth_copy",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(depth_export, ResourceUsage::kSampledFragment);
            builder.Write(depth, ResourceUsage::kDepthAttachment);
          },
          [this, depth_export, depth](PassContext &ctx) {
            gpu::DepthAttachment depth_attachment{.view =
                                                 ctx.graph->image(depth).view,
                                             .load = gpu::LoadOp::kDontCare};
            ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                     .depth = &depth_attachment});
            ctx.cmd->BindPipeline(depth_copy_pipeline_);
            ctx.cmd->BindTransient(
                0, {gpu::Bind::SampledView(0, ctx.graph->image(depth_export).view)});
            ctx.cmd->Draw(3, 1, 0, 0);
            ctx.cmd->EndRendering();
          });
    }

    // Snapshot this frame's depth for next frame's occlusion test. Skipped with
    // the test itself; the next frame then waits for a fresh snapshot rather
    // than culling against a stale one.
    cull_depth_snapshot_ = occlusion_wanted;
    if (occlusion_wanted) gpu_cull_.CopyDepth(graph_, depth_export, cull_slot);

    // Persistent foam/ripple field: recenter+advect+decay the rings, step the
    // near-camera ripples, and inject crest foam + object wakes. Scheduled
    // here, after the prepass exports depth, so the local-interaction phase can
    // read it: it projects each ring-0 column into the frame, and where opaque
    // geometry crosses the waterline it rings the surface (no CPU disturbance
    // needed) and reflects ripples off the analytic island. Still ordered after
    // the FFT ocean (recorded earlier) so crest injection samples the fresh
    // foam map.
    if (water_field_active_) {
      WaterField::UpdateParams wf{};
      wf.camera_pos = view.camera.eye;
      wf.time = static_cast<f32>(time_seconds_);
      wf.dt = rx::Min(view.frame_delta_seconds,
                       1.0f / 30.0f); // clamp for stability
      wf.frame_slot = frame_slot;
      wf.fft_ocean = fft_ocean_active;
      wf.disturbances = view.water_disturbances.data();
      wf.disturbance_count = static_cast<u32>(view.water_disturbances.size());
      wf.interaction = settings_.water_interaction;
      // The analytic island is only a defined terrain source where the
      // shoreline wetting owns it; reflect ripples off it only then (mirrors
      // that gating).
      wf.obstacle = settings_.water_interaction && shore_wetting_active_ &&
                    WaterObstacleOpt;
      wf.view_proj = globals.view_proj;
      wf.inv_view_proj = globals.inv_view_proj;
      for (u32 i = 0; i < 4; ++i)
        wf.island[i] = settings_.shore_island[i];
      wf.water_level = 0.0f;
      wf.render_size[0] = static_cast<f32>(render_width_);
      wf.render_size[1] = static_cast<f32>(render_height_);
      water_field_.AddToGraph(
          graph_, wf,
          fft_ocean_active ? ocean_.normal_foam_view() : gpu::TextureView{},
          fft_ocean_active ? ocean_.displacement_view() : gpu::TextureView{},
          depth_export);
    }

    // Optional heightfield fluid solver (flowing water + lava). Scheduled here
    // alongside the water field; it steps its own domain textures and the
    // surface renderer samples them downstream. Sources bounded at 64.
    if (fluid_sim_active_) {
      FluidSim::UpdateParams fp{};
      fp.domain = view.fluid_domain;
      fp.dt = rx::Min(view.frame_delta_seconds, 1.0f / 30.0f);
      fp.frame_slot = frame_slot;
      fp.sources = view.fluid_sources.data();
      fp.source_count =
          rx::Min<u32>(static_cast<u32>(view.fluid_sources.size()), FluidSim::kMaxSources);
      fluid_sim_.AddToGraph(graph_, fp);
    }

    ResourceHandle ao = kInvalidResource;
    ResourceHandle sun_shadow = kInvalidResource;
    ResourceHandle spec_refl = kInvalidResource;
    ResourceHandle rcgi_irr = kInvalidResource;

    // RCGI screen side runs BEFORE the reflection trace so the reflection
    // ray-skip (item 16) can read the gather's denoised per-pixel diffuse SH.
    // M2 half-res SH final gather -> bilateral denoise -> full-res upscale +
    // temporal filter, writing the "rcgi_irradiance" transient the forward pass
    // folds in. `RX_RCGI_PROBES_ONLY=1` (and always in software mode) swaps in
    // the M1 per-pixel cascade resolve. The gather is the first consumer of the
    // (optionally async) world passes, so join the compute queue before it.
    // RCGI and DDGI are mutually exclusive, so this never races the DDGI join
    // used by the reflection/DDGI path below.
#if defined(RX_HAS_NRD)
    // The gather's denoised SH, captured for the specular ray-skip (reflections
    // are NRD-gated, so this is only consumed under RX_HAS_NRD).
    ResourceHandle refl_sh[3] = {kInvalidResource, kInvalidResource,
                                 kInvalidResource};
    gpu::Extent2D refl_sh_extent{};
#endif
    if (rcgi_world && depth_export != kInvalidResource &&
        normals != kInvalidResource) {
      if (rcgi_async) {
        graph_.AddPass(
            "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
            [](PassContext &) {});
      }
      if (rcgi_probes_only ||
          !rcgi_->EnsureScreenResources({render_width_, render_height_})) {
        rcgi_irr = rcgi_->AddResolvePass(
            graph_, depth_export, normals, {render_width_, render_height_},
            globals.inv_view_proj, view.camera.eye, settings_.rcgi_intensity,
            frame_index_);
      } else {
        rcgi_irr = rcgi_->AddGatherChain(
            graph_, *raytracing_, tlas_slot, depth_export, normals, motion,
            {render_width_, render_height_}, globals.inv_view_proj,
            globals.prev_view_proj, view.camera.eye, settings_.rcgi_intensity,
            frame_index_, first_frame);
#if defined(RX_HAS_NRD)
        ResourceHandle sh[3];
        gpu::Extent2D e{};
        if (rcgi_->denoised_sh(sh, e)) {
          refl_sh[0] = sh[0];
          refl_sh[1] = sh[1];
          refl_sh[2] = sh[2];
          refl_sh_extent = e;
        }
#endif
      }
    }
#if defined(RX_HAS_NRD)
    if (nrd_ao || nrd_shadow) {
      // Shared NRD guides (normal+roughness, viewZ) and per-frame camera state
      // for the REBLUR ao and SIGMA sun-shadow denoisers.
      NrdDenoiser::Inputs nrd_inputs =
          nrd_.PrepareInputs(graph_, depth_export, normals, 0.1f);
      NrdDenoiser::FrameSettings fs;
      fs.view_to_clip = proj;
      fs.view_to_clip_prev = prev_proj_;
      fs.world_to_view = view_mat;
      fs.world_to_view_prev = prev_view_;
      fs.jitter[0] = jitter_x;
      fs.jitter[1] = jitter_y;
      fs.jitter_prev[0] = prev_jitter_[0];
      fs.jitter_prev[1] = prev_jitter_[1];
      fs.sun_direction = sun;
      fs.frame_index = frame_index_;
      fs.reset = first_frame;
      nrd_.SetFrame(fs);
      if (nrd_ao) {
        // RTAO traces a raw hit distance; REBLUR denoises it.
        ResourceHandle hitdist =
            rtao_.AddToGraph(graph_, *raytracing_, tlas_slot, depth_export,
                             normals, globals.inv_view_proj, frame_index_, 0.1f,
                             NrdDenoiser::kHitDistParams);
        ao = nrd_.DenoiseAo(graph_, nrd_inputs.normal_roughness,
                            nrd_inputs.view_z, motion, hitdist);
      }
      if (nrd_shadow) {
        // Trace a 1-spp cone-jittered sun visibility into SIGMA's penumbra
        // input, then denoise it into a clean screen-space sun shadow the
        // lighting samples (instead of the noisier inline trace the temporal
        // pass had to integrate).
        ResourceHandle penumbra = shadow_trace_.AddToGraph(
            graph_, *raytracing_, tlas_slot, depth_export,
            globals.inv_view_proj, sun, 0.1f, settings_.sun_angular_radius,
            globals.jitter[0], globals.jitter[1]);
        sun_shadow = nrd_.DenoiseShadow(graph_, nrd_inputs.normal_roughness,
                                        nrd_inputs.view_z, motion, penumbra);
        // Contact shadows: a short screen-space march folded into the denoised
        // shadow, restoring the sub-30cm grounding the 1-spp ray blurs away.
        graph_.AddPass(
            "contact_shadow",
            [&](RenderGraph::PassBuilder &b) {
              b.Write(sun_shadow, ResourceUsage::kStorageWrite);
              b.Read(depth_export, ResourceUsage::kSampledCompute);
            },
            [this, sun_shadow, depth_export, sun, frame_slot,
             view_proj = globals.view_proj,
             inv_view_proj = globals.inv_view_proj](PassContext &ctx) {
              const internal::ContactCamera camera{view_proj, inv_view_proj};
              base::MemCopy(contact_camera_[frame_slot].mapped, &camera,
                          sizeof(camera));
              struct ContactPush {
                f32 sun_dir[3];
                f32 near_plane;
                u32 size[2];
                f32 range;
                f32 thickness;
                u32 steps;
                u32 frame_index;
                f32 pad[2];
              } p{};
              p.sun_dir[0] = sun.x;
              p.sun_dir[1] = sun.y;
              p.sun_dir[2] = sun.z;
              p.near_plane = 0.1f;
              p.size[0] = render_width_;
              p.size[1] = render_height_;
              p.range = 0.35f;
              p.thickness = 0.4f;
              p.steps = 12;
              p.frame_index = frame_index_;
              ctx.cmd->BindPipeline(contact_shadow_pipeline_);
              ctx.cmd->BindTransient(
                  0, {gpu::Bind::Storage(0, ctx.graph->image(sun_shadow)),
                      gpu::Bind::Sampled(1, ctx.graph->image(depth_export)),
                      gpu::Bind::Uniform(2, contact_camera_[frame_slot], 0,
                                    sizeof(internal::ContactCamera))});
              ctx.cmd->Push(p);
              ctx.cmd->Dispatch2D({render_width_, render_height_});
            });
        if (settings_.cloudscape && cloudscape_ready_ && !interior) {
          // Cloudscape ground shadows: the same textured density field the
          // march renders, so the shade tracks the formations that actually
          // occlude the sun (gaps stay lit, cores darken, wind advects both).
          Cloudscape::Frame sf;
          sf.inv_view_proj = globals.inv_view_proj;
          sf.frame_index = frame_index_;
          sf.jitter[0] = globals.jitter[0];
          sf.jitter[1] = globals.jitter[1];
          sf.sun_direction = settings_.sun_direction;
          sf.time = static_cast<f32>(time_seconds_);
          sf.controls = settings_.cloudscape_controls;
          sf.controls.wind_yaw = settings_.weather.wind_yaw;
          sf.controls.wind_speed = settings_.weather.wind_speed;
          cloudscape_.AddShadowToGraph(graph_, sun_shadow, depth_export,
                                       {render_width_, render_height_}, sf, 0.75f);
        } else if (settings_.clouds && !interior) {
          // Cloud shadows: the layer's optical depth along the sun ray darkens
          // the same denoised shadow the shading samples.
          graph_.AddPass(
              "cloud_shadow",
              [&](RenderGraph::PassBuilder &b) {
                b.Write(sun_shadow, ResourceUsage::kStorageWrite);
                b.Read(depth_export, ResourceUsage::kSampledCompute);
              },
              [this, sun_shadow, depth_export, sun,
               inv_view_proj = globals.inv_view_proj](PassContext &ctx) {
                struct CloudShadowPush {
                  Mat4 inv_view_proj;
                  f32 sun_dir[3];
                  f32 near_plane;
                  u32 size[2];
                  f32 time;
                  f32 coverage;
                  f32 bottom;
                  f32 top;
                  f32 wind;
                  f32 strength;
                  f32 wind_z; // z drift velocity, matches cloud_shadow.cs
                  f32 pad[3];
                } p{};
                p.inv_view_proj = inv_view_proj;
                p.sun_dir[0] = sun.x;
                p.sun_dir[1] = sun.y;
                p.sun_dir[2] = sun.z;
                p.near_plane = 0.1f;
                p.size[0] = render_width_;
                p.size[1] = render_height_;
                p.time = static_cast<f32>(time_seconds_);
                p.coverage = settings_.cloud_coverage;
                p.bottom = 1500.0f;
                p.top = 4200.0f;
                // Same drift velocity as clouds.cs, so the shadows track the
                // deck.
                p.wind = ::cosf(settings_.weather.wind_yaw) *
                         settings_.weather.wind_speed;
                p.wind_z = ::sinf(settings_.weather.wind_yaw) *
                           settings_.weather.wind_speed;
                p.strength = 0.75f;
                ctx.cmd->BindPipeline(cloud_shadow_pipeline_);
                ctx.cmd->BindTransient(
                    0, {gpu::Bind::Storage(0, ctx.graph->image(sun_shadow)),
                        gpu::Bind::Sampled(1, ctx.graph->image(depth_export))});
                ctx.cmd->Push(p);
                ctx.cmd->Dispatch2D({render_width_, render_height_});
              });
        }
      }
      if (ddgi_async && spec_refl_active) {
        // First DDGI consumer: the reflection trace samples the probe atlases.
        graph_.AddPass(
            "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
            [](PassContext &) {});
      }
      if (spec_refl_active) {
        // 1-spp VNDF reflection radiance -> REBLUR_SPECULAR; the scene pass
        // samples the result instead of tracing an inline mirror ray.
        EnvironmentSystem::DdgiBinding refl_ddgi;
        if (ddgi_active)
          refl_ddgi = ddgi_->binding(frame_index_);
        ReflectionTrace::Frame rfl;
        rfl.inv_view_proj = globals.inv_view_proj;
        rfl.camera_pos = view.camera.eye;
        rfl.sun_direction = settings_.sun_direction;
        rfl.sun_intensity =
            settings_.sun_intensity + settings_.weather.lightning * 9.0f;
        rfl.sun_color = settings_.sun_color;
        rfl.roughness_cutoff = settings_.reflection_roughness_cutoff;
        rfl.frame_index = frame_index_;
        rfl.near_plane = 0.1f;
        rfl.hit_dist_params = NrdDenoiser::kHitDistParams;
        rfl.ddgi = ddgi_active;
        // Real-alpha any-hit only when the vegetation feature is on overall.
        rfl.veg_anyhit = internal::RtVegOpt && RtVegAnyHitOpt;
        // Phase 4: half-res trace + upscale, roughness-scaled reach, one-step
        // fog, and diffuse-SH ray-skip (only when the RCGI gather SH is
        // available).
        rfl.half_res = ReflHalfOpt;
        rfl.fog = ReflFogOpt && fog_active;
        rfl.fog_density = settings_.fog_density;
        rfl.fog_height_falloff = settings_.fog_height_falloff;
        rfl.fog_base_height = settings_.fog_base_height;
        rfl.sh_skip = ReflShSkipOpt && refl_sh[0] != kInvalidResource;
        rfl.sh_skip_roughness = ReflShSkipRough;
        // Spec-bounce indirect diffuse: under RCGI the DDGI atlas is empty, so
        // the reflection hit reads the RCGI irradiance cascades (item 20b).
        // Bind the real cascades when RCGI is active, else environment
        // placeholders so the descriptor set is complete (kFlagRcgi stays clear
        // -> never sampled).
        ReflectionTrace::RcgiBinding refl_rcgi;
        RcgiSystem::IrradianceBinding rcgi_irr_bind;
        if (rcgi_active)
          rcgi_irr_bind = rcgi_->irradiance_binding(frame_index_);
        if (rcgi_irr_bind.valid) {
          refl_rcgi.irradiance = rcgi_irr_bind.irradiance;
          refl_rcgi.visibility = rcgi_irr_bind.visibility;
          refl_rcgi.globals = rcgi_irr_bind.globals;
          refl_rcgi.probe_meta = rcgi_irr_bind.probe_meta;
          refl_rcgi.interior_vols = rcgi_irr_bind.interior_vols;
          refl_rcgi.sampler = rcgi_irr_bind.sampler;
          refl_rcgi.in_general = true;
          refl_rcgi.active = true;
        } else {
          refl_rcgi.irradiance = environment_->black_view();
          refl_rcgi.visibility = environment_->black_view();
          refl_rcgi.globals = &environment_->dummy_volume();
          refl_rcgi.probe_meta = &environment_->dummy_storage();
          refl_rcgi.interior_vols = &environment_->dummy_storage();
          refl_rcgi.sampler = environment_->sampler();
        }
        ResourceHandle raw = reflection_trace_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(), depth_export,
            normals, environment_->prefiltered_view(),
            ddgi_active ? refl_ddgi.irradiance
                        : environment_->black_array_view(),
            ddgi_active,
            ddgi_active ? refl_ddgi.volume : environment_->dummy_volume(),
            ddgi_active ? refl_ddgi.volume_size : 256, environment_->sampler(),
            {render_width_, render_height_}, refl_sh[0], refl_sh[1], refl_sh[2],
            refl_sh_extent, refl_rcgi, rfl);
        spec_refl = nrd_.DenoiseSpecular(graph_, nrd_inputs.normal_roughness,
                                         nrd_inputs.view_z, motion, raw);
      }
      prev_proj_ = proj;
      prev_view_ = view_mat;
      prev_jitter_[0] = jitter_x;
      prev_jitter_[1] = jitter_y;
    }
#endif
    if (ss_ao) {
      const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
      ao =
          ssao_.AddToGraph(graph_, depth_export, normals, globals.inv_view_proj,
                           proj_scale, 0.1f, frame_index_);
    }

    // Hair transmittance volume. It has to be built BEFORE anything shades
    // against it: the scene pass and the transparent pass both sample it for
    // the shadow a groom casts on the scalp and shoulders, and the graph runs
    // passes in declaration order. Built late it would hand them the previous
    // frame's texels indexed by this frame's light matrix, which slides the
    // hair shadow off the head the moment either the sun or the groom moves.
    if (hair_.active()) {
      hair_frame.view_proj = view_proj;
      hair_frame.camera_pos = view.camera.eye;
      hair_frame.sun_direction = applied_sun_direction_;
      hair_frame.sun_intensity = applied_sun_intensity_;
      hair_frame.sun_color = applied_sun_color_;
      // Hair with no ambient is a black silhouette the moment it leaves the
      // sun. The flat ambient is the sky term the forward pass uses for
      // everything else when IBL is off; with IBL on the sky already carries
      // it, so the groom takes a matching share.
      const f32 ambient_level =
          settings_.ibl ? settings_.ibl_intensity * 0.12f : settings_.ambient;
      hair_frame.ambient = {applied_sun_color_.x * ambient_level,
                            applied_sun_color_.y * ambient_level,
                            applied_sun_color_.z * ambient_level};
      hair_frame.transmittance = settings_.hair_transmittance;
      hair_frame.transmittance_depth = settings_.hair_transmittance_depth;
      hair_frame.fibre_scale = settings_.hair_fibre_scale;
      hair_frame.shadow_density = settings_.hair_shadow_density;
      hair_frame.debug_view = static_cast<u32>(settings_.debug_view);
      hair_.AddTransmittanceToGraph(graph_, hair_frame, frame_slot);
    }
    hair_volume_on = hair_.volume_handles(&hair_front, &hair_layers);

    // Hybrid ReSTIR DI: reservoir-resampled point/spot lights with one shadow
    // ray per pixel, shaded off the prepass G-buffer into screen-space targets
    // the forward pass folds back in (env slots 23/24, kFrameFlagRestirDi).
    RestirDi::Outputs restir_out;
    if (restir_active) {
      RestirDi::Frame rf;
      rf.inv_view_proj = globals.inv_view_proj;
      rf.camera_pos = view.camera.eye;
      rf.frame_index = frame_index_;
      rf.light_count = globals.light_count;
      rf.lights = frame.lights;
      rf.tlas_slot = tlas_slot;
      restir_out = restir_di_.AddToGraph(graph_, depth_export, normals, motion,
                                         *raytracing_,
                                         {render_width_, render_height_}, rf);
    }

    // Without the reflection trace the scene pass is DDGI's first consumer;
    // join the async queue before the lighting work leading into it.
    if (ddgi_async && spec_refl == kInvalidResource) {
      graph_.AddPass(
          "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
          [](PassContext &) {});
    }

    // Froxel light culling: fixed slots per cluster, run every frame (a zero
    // light count still zeroes the counts the forward loop reads).
    graph_.AddPass(
        "light_cluster", [&](RenderGraph::PassBuilder &) {},
        [this, &frame, &view, light_count, decal_count,
         proj](PassContext &ctx) {
          struct ClusterPush {
            Mat4 view_mat;
            f32 screen[2];
            f32 near_plane;
            f32 slice_scale;
            f32 slice_bias;
            u32 light_count;
            f32 tan_half_fov_y;
            f32 aspect;
            u32 decal_count;
            f32 pad[3];
          } p{};
          p.view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
          p.screen[0] = static_cast<f32>(render_width_);
          p.screen[1] = static_cast<f32>(render_height_);
          p.near_plane = 0.1f;
          constexpr f32 kNear = 0.1f, kFar = 500.0f;
          p.slice_scale =
              static_cast<f32>(kClusterSlices) / ::log2f(kFar / kNear);
          p.slice_bias = -::log2f(kNear) * p.slice_scale;
          p.light_count = light_count;
          p.tan_half_fov_y = ::tanf(view.camera.fov_y * 0.5f);
          p.aspect = static_cast<f32>(render_width_) /
                     static_cast<f32>(render_height_);
          p.decal_count = decal_count;
          ctx.cmd->BindPipeline(light_cluster_pipeline_);
          ctx.cmd->BindTransient(
              0,
              {gpu::Bind::StorageBuffer(0, frame.lights, 0, frame.lights.size),
               gpu::Bind::StorageBuffer(1, cluster_counts_, 0, cluster_counts_.size),
               gpu::Bind::StorageBuffer(2, cluster_indices_, 0,
                                   cluster_indices_.size),
               gpu::Bind::StorageBuffer(3, frame.decals, 0, frame.decals.size),
               gpu::Bind::StorageBuffer(4, decal_cluster_indices_, 0,
                                   decal_cluster_indices_.size)});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch((kClusterCount + 63) / 64, 1, 1);
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite,
                                 gpu::BarrierScope::kGraphicsRead);
        });

    graph_.AddPass(
        "scene",
        [&](RenderGraph::PassBuilder &builder) {
          // The hair volume is sampled for the groom's shadow on skin; the
          // declaration is what orders it against the pass that wrote it.
          if (hair_volume_on) {
            builder.Read(hair_front, ResourceUsage::kSampledFragment);
            builder.Read(hair_layers, ResourceUsage::kSampledFragment);
          }
          builder.Write(geom_scene, ResourceUsage::kColorAttachment);
          builder.Write(geom_motion, ResourceUsage::kColorAttachment);
          builder.Write(geom_skin, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth, ResourceUsage::kDepthAttachment);
          if (ao != kInvalidResource)
            builder.Read(ao, ResourceUsage::kSampledFragment);
          if (sun_shadow != kInvalidResource)
            builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
          if (spec_refl != kInvalidResource)
            builder.Read(spec_refl, ResourceUsage::kSampledFragment);
          if (csm_active)
            builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
          if (restir_active) {
            builder.Read(restir_out.diffuse, ResourceUsage::kSampledFragment);
            builder.Read(restir_out.spec, ResourceUsage::kSampledFragment);
          }
          if (rcgi_irr != kInvalidResource)
            builder.Read(rcgi_irr, ResourceUsage::kSampledFragment);
          if (ms_occlude)
            builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
        },
        [this, geom_scene, geom_motion, geom_skin, geom_depth, msaa, ao,
         sun_shadow, spec_refl, rcgi_irr, rcgi_world, use_rt_frag, ddgi_active,
         csm_active, shadow_slot, shadow_atlas, cull_commands, ms_active,
         globals_set, frame_slot, restir_active, restir_out, &frame, &view,
          draw_meshlet_instances, view_proj, force_lod0_for_tlas,
          grass_active, msaa_samples](PassContext &ctx) {
          gpu::BindingSetHandle env_set = env_scene_sets_[frame_slot];
          gpu::TextureView ao_view = ao != kInvalidResource
                                    ? ctx.graph->image(ao).view
                                    : gpu::TextureView{};
          gpu::TextureView rcgi_irr_view = rcgi_irr != kInvalidResource
                                          ? ctx.graph->image(rcgi_irr).view
                                          : gpu::TextureView{};
          gpu::TextureView sun_shadow_view = sun_shadow != kInvalidResource
                                            ? ctx.graph->image(sun_shadow).view
                                            : gpu::TextureView{};
          gpu::TextureView spec_refl_view = spec_refl != kInvalidResource
                                           ? ctx.graph->image(spec_refl).view
                                           : gpu::TextureView{};
          // The hair transmittance volume, so skin under a groom is shadowed
          // by the fibres over it (see HairStrands::AddTransmittanceToGraph).
          // Null params = no hair this frame, which the forward pass reads as
          // "nothing overhead" rather than as a black shadow map.
          EnvironmentSystem::HairVolumeBinding hair_env_binding;
          {
            const HairStrands::TransmittanceBinding hb = hair_.transmittance();
            hair_env_binding.front_depth = hb.front_depth;
            hair_env_binding.layers = hb.layers;
            hair_env_binding.params = hb.params;
          }
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active)
            ddgi_binding = ddgi_->binding(frame_index_);
          // Inline reflection bounce reads the RCGI world cascades when RCGI is
          // active (matches the NRD reflection path and the kFrameFlagRcgi
          // gate); null -> placeholders. The atlases stay in GENERAL between
          // frames.
          EnvironmentSystem::RcgiWorldBinding rcgi_world_binding;
          RcgiSystem::IrradianceBinding rcgi_world_src;
          if (rcgi_world)
            rcgi_world_src = rcgi_->irradiance_binding(frame_index_);
          if (rcgi_world_src.valid) {
            rcgi_world_binding.irradiance = rcgi_world_src.irradiance;
            rcgi_world_binding.visibility = rcgi_world_src.visibility;
            rcgi_world_binding.globals = rcgi_world_src.globals;
            rcgi_world_binding.probe_meta = rcgi_world_src.probe_meta;
            rcgi_world_binding.interior_vols = rcgi_world_src.interior_vols;
          }
          environment_->WriteEnvSet(
              env_set, ao_view, ddgi_active ? &ddgi_binding : nullptr,
              csm_active ? ctx.graph->image(shadow_atlas).view : gpu::TextureView{},
              csm_active ? shadow_.cascade_buffer(shadow_slot) : gpu::GpuBuffer{},
              shadow_.cascade_buffer_size(), gpu::TextureView{}, sun_shadow_view,
              frame.lights, frame.lights.size, spec_refl_view, cluster_counts_,
              cluster_indices_, frame.decals, decal_cluster_indices_,
              decal_atlas_view_,
              local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                    : gpu::GpuBuffer{},
              local_shadows_active_ ? local_shadows_.atlas().view
                                    : gpu::TextureView{},
              decal_normal_atlas_view_,
              restir_active ? ctx.graph->image(restir_out.diffuse).view
                            : gpu::TextureView{},
              restir_active ? ctx.graph->image(restir_out.spec).view
                            : gpu::TextureView{},
              virtual_texture_.available() ? virtual_texture_.feedback_buffer()
                                           : gpu::GpuBuffer{},
              virtual_texture_.available() ? virtual_texture_.indirection_view()
                                           : gpu::TextureView{},
              virtual_texture_.available() ? virtual_texture_.atlas_view()
                                           : gpu::TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : gpu::TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : gpu::TextureView{},
              gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, // water field rings: transparent pass only
              shore_wetting_active_ ? shore_wetting_.current_view()
                                    : gpu::TextureView{},
              water_caustics_active_ ? water_caustics_.current_view()
                                     : gpu::TextureView{},
              rcgi_irr_view,
              rcgi_world_src.valid ? &rcgi_world_binding : nullptr,
              decal_baker_.available() ? decal_baker_.albedo_view()
                                       : gpu::TextureView{},
              decal_baker_.available() ? decal_baker_.fx_view()
                                       : gpu::TextureView{},
              decal_baker_.available() ? decal_baker_.tile_uv_buffer(frame_slot)
                                       : gpu::GpuBuffer{},
              hair_env_binding.params ? &hair_env_binding : nullptr);

          gpu::ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(geom_scene).view,
                       .load = gpu::LoadOp::kClear,
                       .clear = {0.02f, 0.02f, 0.05f, 1.0f}};
          colors[1] = {.view = ctx.graph->image(geom_motion).view,
                       .load = gpu::LoadOp::kLoad}; // the prepass wrote motion
          colors[2] = {.view = ctx.graph->image(geom_skin).view,
                       .load = gpu::LoadOp::kClear,
                       .clear = {0.0f, 0.0f, 0.0f, 0.0f}};
          gpu::DepthAttachment depth_attachment{
              .view = ctx.graph->image(geom_depth).view,
              .load = gpu::LoadOp::kLoad}; // prepass depth, tested EQUAL
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = base::Span(colors, 3),
                                   .depth = &depth_attachment});

          gpu::BindingSetHandle bindless_set =
              bindless_ ? bindless_->set() : gpu::BindingSetHandle{};

          // Mesh-shader sub-pass: static opaque meshes, finest lod,
          // cluster-culled.
          if (ms_active) {
            mesh_pipeline_->BindMeshScene(*ctx.cmd, globals_set, env_set,
                                          bindless_set, use_rt_frag);
            draw_meshlet_instances(ctx);
          }

          mesh_pipeline_->Bind(*ctx.cmd, globals_set, env_set, bindless_set,
                               use_rt_frag, settings_.wireframe);
          gpu::BindingSetHandle bound_material{};
          bool skinned_bound = false;
          u32 cull_cmd_index = 0; // matches the cull build + prepass order
          for (const DrawItem &item : view.draws) {
            const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            if (cull_cmd_index >= cull_total_commands_)
              break; // clamp to the built commands
            bool ms_handled = ms_active && mesh->has_meshlets;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            if (!ms_handled) {
              if (draw_skinned != skinned_bound) {
                mesh_pipeline_->SetSkinned(*ctx.cmd, draw_skinned, use_rt_frag,
                                           settings_.wireframe);
                skinned_bound = draw_skinned;
              }
              MeshPushConstants push{};
              push.draw_index =
                  static_cast<u32>(&item - view.draws.data()) + 1u;
              if (mesh->terrain_lod) {
                base::MemCopy(push.detail_rect, view.detail_rect,
                            sizeof(push.detail_rect));
              }
              if (draw_skinned && item.skin_offset >= 0) {
                push.bone_address = frame.bone_palette.address;
                push.skin_offset = static_cast<u32>(item.skin_offset);
                push.prev_skin_offset = PrevSkinOffset(item);
              }
              if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                  item.morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(item.morph_offset);
                push.morph_count = item.morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              // Low 24 bits the faction/team colour, top byte the draw's
              // baked decal-layer tile (0 = none). See MeshPushConstants.
              push.tint_packed =
                  (item.tint & 0xffffffu) |
                  (decal_baker_.tile_slot(item.decal_receiver) << 24);
              mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            }
            for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue;
              if (cull_cmd_index >= cull_total_commands_)
                break; // partial-mesh boundary
              if (!ms_handled) {
                gpu::BindingSetHandle material =
                    material_system_->set(submesh.material);
                if (!(material == bound_material)) {
                  mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                  bound_material = material;
                }
                ctx.cmd->DrawIndexedIndirect(
                    cull_commands, cull_cmd_index * GpuCull::kCommandStride, 1,
                    GpuCull::kCommandStride);
              }
              ++cull_cmd_index;
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            const u32 lod =
                !settings_.distance_lod || (force_lod0_for_tlas && !mesh->no_rt)
                    ? 0
                    : SelectLod(*mesh,
                                InstanceGroupDistance(group, view.camera.eye));
            const base::Vector<gpu::GpuSubmesh> &submeshes =
                lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
            const i32 vertex_offset =
                lod == 0 ? 0
                         : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);
            mesh_pipeline_->SetInstanced(*ctx.cmd, use_rt_frag,
                                         settings_.wireframe);
            MeshPushConstants push{};
            const gpu::GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const gpu::GpuSubmesh &submesh : submeshes) {
              if (submesh.blend)
                continue;
              const gpu::BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, vertex_offset, 0);
            }
          }
          if (grass_active)
            procedural_grass_.DrawScene(*ctx.cmd, frame_slot, msaa_samples);
          // The sky pipeline is single-sampled; under kMsaa it draws in its own
          // pass right after the resolve instead (same attachments at 1x).
          if (settings_.sky && !settings_.interior && !msaa) {
            environment_->DrawSky(*ctx.cmd, globals_set);
          }
          ctx.cmd->EndRendering();
        });

    // kMsaa: average-resolve the scene color (this is the actual antialiasing),
    // motion (the geometry part; the sky adds its motion at 1x below) and the
    // skin-diffuse export, then draw the sky into the resolved targets with the
    // rebuilt 1x depth. Everything after this point runs exactly as in kNone.
    if (msaa) {
      graph_.AddPass(
          "msaa_resolve",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(geom_scene, ResourceUsage::kResolveSrc);
            builder.Read(geom_motion, ResourceUsage::kResolveSrc);
            builder.Read(geom_skin, ResourceUsage::kResolveSrc);
            builder.Write(scene_color, ResourceUsage::kResolveDst);
            builder.Write(motion, ResourceUsage::kResolveDst);
            builder.Write(skin_diffuse, ResourceUsage::kResolveDst);
          },
          [this, geom_scene, geom_motion, geom_skin, scene_color, motion,
           skin_diffuse](PassContext &ctx) {
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_scene),
                                    ctx.graph->image(scene_color));
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_motion),
                                    ctx.graph->image(motion));
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_skin),
                                    ctx.graph->image(skin_diffuse));
          });
      if (settings_.sky && !settings_.interior) {
        graph_.AddPass(
            "msaa_sky",
            [&](RenderGraph::PassBuilder &builder) {
              builder.Write(scene_color, ResourceUsage::kColorAttachment);
              builder.Write(motion, ResourceUsage::kColorAttachment);
              builder.Write(skin_diffuse, ResourceUsage::kColorAttachment);
              builder.Write(depth, ResourceUsage::kDepthAttachment);
            },
            [this, scene_color, motion, skin_diffuse, depth,
             globals_set](PassContext &ctx) {
              gpu::ColorAttachment colors[3];
              colors[0] = {.view = ctx.graph->image(scene_color).view,
                           .load = gpu::LoadOp::kLoad};
              colors[1] = {.view = ctx.graph->image(motion).view,
                           .load = gpu::LoadOp::kLoad};
              colors[2] = {.view = ctx.graph->image(skin_diffuse).view,
                           .load = gpu::LoadOp::kLoad};
              gpu::DepthAttachment depth_attachment{
                  .view = ctx.graph->image(depth).view, .load = gpu::LoadOp::kLoad};
              ctx.cmd->BeginRendering(
                  {.extent = {render_width_, render_height_},
                   .colors = base::Span(colors, 3),
                   .depth = &depth_attachment});
              environment_->DrawSky(*ctx.cmd, globals_set);
              ctx.cmd->EndRendering();
            });
      }
    }

    // App scene passes (render/rhi hooks): record raw app GPU work into rx's
    // frame, depth-interleaved with rx geometry. Nothing added when unset.
    // `add_scene_hook` runs the callback inside a first-class graph pass with
    // the color/depth (and, for the opaque phase, depth-export) writes declared
    // so the graph barriers them; the callback opens its own dynamic-rendering
    // section.
    auto add_scene_hook =
        [&](const base::Function<void(const SceneHookContext &)> &hook,
            ScenePhase phase, ResourceHandle color_h, ResourceHandle depth_h,
            ResourceHandle export_h, ResourceHandle motion_h) {
          const f32 jx = globals.jitter[0], jy = globals.jitter[1];
          const Mat4 prev_vp = globals.prev_view_proj;
          graph_.AddPass(
              phase == ScenePhase::kOpaque ? "app_scene_opaque"
                                           : "app_scene_transparent",
              [color_h, depth_h, export_h, motion_h](RenderGraph::PassBuilder &b) {
                b.Write(color_h, ResourceUsage::kColorAttachment);
                b.Write(depth_h, ResourceUsage::kDepthAttachment);
                if (export_h != kInvalidResource)
                  b.Write(export_h, ResourceUsage::kColorAttachment);
                if (motion_h != kInvalidResource)
                  b.Write(motion_h, ResourceUsage::kColorAttachment);
              },
              // `hook` is copied into the closure (the graph executes this
              // after add_scene_hook returns, so a reference to the parameter
              // would dangle).
              [this, hook, phase, color_h, depth_h, export_h, motion_h, proj,
               view_mat, view_proj, prev_vp, jx, jy, &view](PassContext &ctx) {
                SceneHookContext hc;
                hc.phase = phase;
                hc.cmd = ctx.cmd;
                hc.device = ctx.device;
                const gpu::GpuImage &color_img = ctx.graph->image(color_h);
                hc.color = &color_img;
                hc.color_view = color_img.view;
                hc.color_format = color_img.format;
                const gpu::GpuImage &depth_img = ctx.graph->image(depth_h);
                hc.depth = &depth_img;
                hc.depth_view = depth_img.view;
                hc.depth_format = depth_img.format;
                if (export_h != kInvalidResource) {
                  const gpu::GpuImage &export_img = ctx.graph->image(export_h);
                  hc.depth_export = &export_img;
                  hc.depth_export_view = export_img.view;
                  hc.depth_export_format = export_img.format;
                }
                if (motion_h != kInvalidResource) {
                  const gpu::GpuImage &motion_img = ctx.graph->image(motion_h);
                  hc.motion = &motion_img;
                  hc.motion_view = motion_img.view;
                  hc.motion_format = motion_img.format;
                }
                hc.extent = {render_width_, render_height_};
                hc.frame_slot = frame_index_ % kFramesInFlight;
                hc.frames_in_flight = kFramesInFlight;
                hc.view = view_mat;
                hc.proj = proj;
                hc.view_proj = view_proj;
                hc.prev_view_proj = prev_vp;
                hc.jitter[0] = jx;
                hc.jitter[1] = jy;
                hc.near_plane = 0.1f;
                hc.camera_pos = view.camera.eye;
                hook(hc);
              });
        };
    if (view.scene_opaque && !path_trace && depth != kInvalidResource) {
      add_scene_hook(view.scene_opaque, ScenePhase::kOpaque, scene_color, depth,
                     depth_export, motion);
    }

    // Screen-space subsurface scattering: separable per-channel diffusion of
    // the skin buffer the scene pass exported, folded back into the opaque
    // color before anything composites on top.
    if (settings_.sss) {
      struct SssPush {
        u32 size[2];
        f32 inv_size[2];
        f32 dir[2];
        f32 near_plane;
        f32 width;
        f32 proj_scale;
        f32 max_radius;
        u32 composite;
        f32 strength;
      };
      auto fill_sss = [this, proj](SssPush &p) {
        p.size[0] = render_width_;
        p.size[1] = render_height_;
        p.inv_size[0] = 1.0f / static_cast<f32>(render_width_);
        p.inv_size[1] = 1.0f / static_cast<f32>(render_height_);
        p.near_plane = 0.1f;
        // The per-pixel scatter radius now rides the skin-diffuse alpha (derived
        // from each material's mean free path); width is a global artist
        // multiplier. 0.012 m is the historical reference radius = 1x.
        p.width = settings_.sss_width / 0.012f;
        // Pixels per meter at view depth 1. The projection bakes the vulkan
        // y-flip into m[5], so it is negative - take the magnitude.
        p.proj_scale =
            ::fabsf(proj.m[5]) * 0.5f * static_cast<f32>(render_height_);
        p.max_radius = 24.0f;
        p.strength =
            ::getenv("RX_SSS_DEBUG") ? -1.0f : 1.0f; // <0 = mask debug view
      };
      ResourceHandle sss_tmp =
          graph_.CreateTexture({.name = "sss_tmp",
                                .format = MeshPipeline::kSkinDiffuseFormat,
                                .width = render_width_,
                                .height = render_height_});
      graph_.AddPass(
          "sss_blur_h",
          [&](RenderGraph::PassBuilder &b) {
            b.Write(sss_tmp, ResourceUsage::kStorageWrite);
            b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
            b.Read(depth_export, ResourceUsage::kSampledCompute);
          },
          [this, sss_tmp, skin_diffuse, depth_export,
           fill_sss](PassContext &ctx) {
            SssPush p{};
            fill_sss(p);
            p.dir[0] = 1.0f;
            p.composite = 0;
            ctx.cmd->BindPipeline(sss_pipeline_);
            ctx.cmd->BindTransient(
                0, {gpu::Bind::Storage(0, ctx.graph->image(sss_tmp)),
                    gpu::Bind::Combined(1, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_),
                    gpu::Bind::Combined(2, ctx.graph->image(depth_export).view,
                                   sss_sampler_),
                    gpu::Bind::Combined(3, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_)});
            ctx.cmd->Push(p);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
      graph_.AddPass(
          "sss_apply",
          [&](RenderGraph::PassBuilder &b) {
            b.Write(scene_color, ResourceUsage::kStorageWrite);
            b.Read(sss_tmp, ResourceUsage::kSampledCompute);
            b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
            b.Read(depth_export, ResourceUsage::kSampledCompute);
          },
          [this, scene_color, sss_tmp, skin_diffuse, depth_export,
           fill_sss](PassContext &ctx) {
            SssPush p{};
            fill_sss(p);
            p.dir[1] = 1.0f;
            p.composite = 1;
            ctx.cmd->BindPipeline(sss_pipeline_);
            ctx.cmd->BindTransient(
                0, {gpu::Bind::Storage(0, ctx.graph->image(scene_color)),
                    gpu::Bind::Combined(1, ctx.graph->image(sss_tmp).view,
                                   sss_sampler_),
                    gpu::Bind::Combined(2, ctx.graph->image(depth_export).view,
                                   sss_sampler_),
                    gpu::Bind::Combined(3, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_)});
            ctx.cmd->Push(p);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
    }

    // Screen-space gi: add a diffuse bounce over the opaque result before
    // reflections (so reflections pick up the gi-lit color too). Raster tiers
    // only.
    if (ssgi_active && normals != kInvalidResource) {
      const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
      ResourceHandle bounced = ssgi_.AddToGraph(
          graph_, scene_color, depth_export, normals, globals.inv_view_proj,
          proj_scale, 0.1f, frame_index_);
      scene_color = bounced;
      lit = bounced;
    }

    // Screen-space reflections over the opaque result (before transparency,
    // which does not reflect). Replaces scene_color downstream so everything
    // composites onto the reflected image. Only on raster tiers; rt tiers
    // reflect via the tlas.
    if (ssr_active && normals != kInvalidResource) {
      ResourceHandle reflected =
          ssr_.AddToGraph(graph_, scene_color, depth_export, normals, view_proj,
                          globals.inv_view_proj, view.camera.eye, frame_index_);
      scene_color = reflected;
      lit = reflected;
    }

    // The fluid surface draws inside the transparent pass, so a live fluid
    // domain must open it even when the scene submits no transparent meshes
    // (a lava-only or flood scene has nothing else to draw there).
    const bool fluid_draw =
        fluid_sim_active_ && fluid_sim_.active() && fluid_surface_ != nullptr;
    if ((!transparent.empty() || fluid_draw) && water_) {
      lit = add_water(scene_color, depth, depth_export, motion, sun_shadow,
                      shadow_atlas, csm_active, shadow_slot, tlas_slot,
                      /*globals_written=*/true, rcgi_irr);
    }

    // Surface weather: wet/darken (rain) and whiten (snow) the lit surfaces
    // before the atmosphere/clouds/rain layer over them, gated by the sky
    // occlusion map so cover stays dry. Wetness and snow cover are independent
    // channels (melting snow over wet ground coexists); the game integrates
    // them over time, and live precipitation acts as a floor so setting only
    // `precipitation` still wets (or whitens) the ground.
    const f32 live_rain =
        settings_.weather.snow ? 0.0f : settings_.weather.precipitation;
    const f32 live_snow =
        settings_.weather.snow ? settings_.weather.precipitation : 0.0f;
    const f32 surface_wetness = rx::Max(settings_.weather.wetness, live_rain);
    const f32 surface_snow = rx::Max(settings_.weather.snow_cover, live_snow);
    if ((surface_wetness > 0.0f || surface_snow > 0.0f) && !path_trace &&
        normals != kInvalidResource) {
      SurfaceWeather::Frame sf;
      sf.inv_view_proj = globals.inv_view_proj;
      sf.camera_pos = view.camera.eye;
      sf.wetness = surface_wetness;
      sf.snow_cover = surface_snow;
      sf.rain = live_rain;
      sf.time = static_cast<f32>(time_seconds_);
      if (precip_occlusion_active_) {
        precip_occlusion_.Params(sf.occl);
        sf.occl_range = PrecipOcclusion::y_range();
        sf.occlusion = precip_occlusion_.view();
        sf.occlusion_sampler = precip_occlusion_.sampler();
      }
      lit = surface_weather_.AddToGraph(
          graph_, lit, normals, depth_export, environment_->sky_view(),
          environment_->sampler(), {render_width_, render_height_}, sf);
    }

    // Aerial perspective: composite the atmosphere between the camera and each
    // surface so distant geometry hazes/blue-shifts like the sky. Cheap;
    // skipped when path tracing (the path tracer scatters its own sky).
    if (settings_.aerial_perspective > 0.0f && !path_trace && !interior) {
      AerialPerspective::Frame af;
      af.inv_view_proj = globals.inv_view_proj;
      af.camera_pos = view.camera.eye;
      af.sun_direction = settings_.sun_direction;
      af.sun_intensity = settings_.sun_intensity;
      af.sun_color = settings_.sun_color;
      af.strength = settings_.aerial_perspective;
      lit = aerial_perspective_.AddToGraph(graph_, lit, depth_export,
                                           environment_->transmittance_view(),
                                           environment_->multiscatter_view(),
                                           {render_width_, render_height_}, af);
    }

    // Volumetric clouds raymarched over the sky, composited against depth so
    // terrain occludes them. Skipped when path tracing. The opt-in cloudscape
    // model replaces the procedural pass outright; if its lazy init fails
    // (e.g. no 3D-image support) the legacy pass keeps the sky.
    bool cloudscape_on = settings_.cloudscape && cloudscape_ready_ && !path_trace && !interior;
    Cloudscape::Frame cloudscape_frame;
    bool cloudscape_haze_pending = false;
    if (cloudscape_on) {
      Cloudscape::Frame &cf = cloudscape_frame;
      cf.inv_view_proj = globals.inv_view_proj;
      cf.prev_view_proj = globals.prev_view_proj;
      cf.camera_pos = view.camera.eye;
      cf.jitter[0] = globals.jitter[0];
      cf.jitter[1] = globals.jitter[1];
      cf.reset_history = first_frame;
      cf.time = static_cast<f32>(time_seconds_);
      cf.frame_index = frame_index_;
      cf.sun_direction = settings_.sun_direction;
      // Lightning floods the deck from within via the full-res composite
      // (Frame::flash); the amortized march itself stays flash-free so the
      // temporal history never bakes a bright frame in. The damped global
      // level comes from the weather layer; the raw envelope drives the
      // directional glow a distant strike throws on its own horizon.
      cf.sun_intensity = settings_.sun_intensity;
      cf.sun_color = settings_.sun_color;
      cf.ambient = settings_.ambient;
      cf.flash = settings_.weather.lightning;
      if (settings_.weather.strike_age >= 0.0f) {
        cf.strike_active = true;
        cf.strike_pos = settings_.weather.strike_pos;
        cf.flash_raw = LightningSystem::Envelope(settings_.weather.strike_age,
                                                settings_.weather.strike_seed) *
                       settings_.weather.strike_energy;
      }
      cf.steps = settings_.cloudscape_steps;
      cf.transmittance_lut = environment_->transmittance_view();
      cf.lut_sampler = environment_->sampler();
      cf.controls = settings_.cloudscape_controls;
      // The weather struct owns the live wind; keep the deck advecting with it
      // even when the app writes controls without touching the wind fields.
      cf.controls.wind_yaw = settings_.weather.wind_yaw;
      cf.controls.wind_speed = settings_.weather.wind_speed;
      lit = cloudscape_.AddToGraph(graph_, lit, depth_export,
                                   {render_width_, render_height_}, cf);
      // Funnel before haze: the tornado hangs from the deck, then the ground
      // haze veils both.
      lit = cloudscape_.AddFunnelToGraph(graph_, lit, depth_export,
                                         {render_width_, render_height_}, cf);
      cloudscape_haze_pending = true;
    }
    if (!cloudscape_on && settings_.clouds && !path_trace && !interior) {
      Clouds::Frame cf;
      cf.inv_view_proj = globals.inv_view_proj;
      cf.camera_pos = view.camera.eye;
      cf.time = static_cast<f32>(time_seconds_);
      cf.sun_direction = settings_.sun_direction;
      // Lightning brightens the cloud (its ambient + sun terms scale with
      // intensity), so the storm clouds flash from within.
      cf.sun_intensity =
          settings_.sun_intensity + settings_.weather.lightning * 7.0f;
      cf.sun_color = settings_.sun_color;
      cf.coverage = settings_.cloud_coverage;
      cf.wind_x =
          ::cosf(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      cf.wind_z =
          ::sinf(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      lit = clouds_.AddToGraph(graph_, lit, depth_export,
                               {render_width_, render_height_}, cf);
    }

    // Lightning bolt: the procedural branched channel, drawn after the clouds
    // (the bolt overlays the deck) and before the froxel composite + precip
    // volume (fog scatters over it, rain streaks cross in front of it). Drawn
    // pre-resolve with a zero-motion core so TAA treats the flash as static.
    if (settings_.weather.strike_age >= 0.0f && lightning_.available() &&
        !path_trace && !interior && depth_export != kInvalidResource &&
        motion != kInvalidResource) {
      LightningSystem::Frame lf;
      lf.view_proj = view_proj;
      lf.cam_pos = view.camera.eye;
      lf.time = static_cast<f32>(time_seconds_);
      lf.strike_pos = settings_.weather.strike_pos;
      lf.strike_age = settings_.weather.strike_age;
      lf.strike_seed = settings_.weather.strike_seed;
      lf.strike_energy = settings_.weather.strike_energy;
      lf.jitter[0] = globals.jitter[0];
      lf.jitter[1] = globals.jitter[1];
      lightning_.AddToGraph(graph_, lit, depth_export, motion, lf);
    }

    if (cloudscape_haze_pending) {
      // Ground haze is the nearest medium, so it also veils the lightning
      // channel instead of letting the bolt draw crisply over thick murk.
      lit = cloudscape_.AddHazeToGraph(graph_, lit, depth_export,
                                       {render_width_, render_height_}, cloudscape_frame);
    }

    // Note: screen-space precipitation (rain/snow streaks) is composited after
    // the temporal/upscale resolve below, so TAA's history accumulation does
    // not smear the high-frequency streaks. Surface wetness (above) stays
    // pre-resolve since it shades real surfaces that should be anti-aliased.

    // Unified froxel volumetrics: near-field scattering from the sun and the
    // shadowed clustered lights, composited before the temporal pass so the
    // jittered volume resolves clean.
    bool froxel_on = settings_.froxel_fog && froxel_fog_.available() &&
                     !path_trace && depth_export != kInvalidResource;
    if (froxel_on) {
      FroxelFog::Frame ff;
      ff.inv_view_proj = globals.inv_view_proj;
      ff.prev_view_proj = globals.prev_view_proj;
      ff.camera_pos = view.camera.eye;
      ff.frame_index = frame_index_;
      ff.sun_direction = applied_sun_direction_;
      ff.anisotropy = settings_.fog_anisotropy;
      ff.sun_color = applied_sun_color_ * applied_sun_intensity_;
      ff.ambient = settings_.ambient * 0.05f;
      ff.density = settings_.froxel_density;
      ff.height_falloff = settings_.fog_height_falloff;
      ff.base_height = settings_.fog_base_height;
      ff.start_distance = settings_.froxel_start_distance;
      base::MemCopy(ff.cluster_params, globals.cluster_params,
                  sizeof(ff.cluster_params));
      ff.screen_size[0] = static_cast<f32>(render_width_);
      ff.screen_size[1] = static_cast<f32>(render_height_);
      ff.csm_active = csm_active;
      // The rt sun-shadow tier switches the cascades off, which leaves the fog
      // lighting every froxel with an unoccluded sun: interiors fill with a
      // flat glow and window shafts never form. Trace the sun instead - keyed
      // on the selected shadow mode, not merely on the hardware being capable,
      // so turning sun shadows off does not silently buy 1M rays a frame.
      ff.ray_query_sun = rt_shadows && !csm_active;
      // Neither source available while rt shadows are the selected mode means
      // the tlas is still building or failed. An unoccluded sun would light the
      // whole volume for those frames, which reads as a flash of flat glow; the
      // fog keeps its local lights and drops the sun until the rays are there.
      const bool ray_query_fog_ready =
          froxel_fog_.ray_query_available() && raytracing_ &&
          raytracing_->TlasValid(tlas_slot);
      if (ff.ray_query_sun && !ray_query_fog_ready)
        ff.sun_color = Vec3{0.0f, 0.0f, 0.0f};
      ff.lights = frame.lights;
      ff.cluster_counts = cluster_counts_;
      ff.cluster_indices = cluster_indices_;
      ff.local_shadow_faces = local_shadows_active_
                                  ? local_shadows_.face_buffer(frame_slot)
                                  : environment_->dummy_storage();
      ff.local_shadow_atlas = local_shadows_active_
                                  ? local_shadows_.atlas().view
                                  : environment_->shadow_dummy_view();
      ff.cascade_buffer =
          csm_active ? shadow_.cascade_buffer(shadow_slot) : gpu::GpuBuffer{};
      ff.cascade_size = shadow_.cascade_buffer_size();
      ff.comparison_sampler = environment_->comparison_sampler();
      froxel_fog_.AddToGraph(graph_, lit, depth_export,
                             csm_active ? shadow_atlas : kInvalidResource,
                             raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), tlas_slot,
                             {render_width_, render_height_}, ff);
    }

    // Volumetric fog marches the lit scene against depth before the temporal
    // pass, so the marched noise resolves into stable shafts.
    if (fog_active) {
      VolumetricFog::Frame ff;
      ff.inv_view_proj = globals.inv_view_proj;
      ff.camera_pos = view.camera.eye;
      ff.sun_direction = settings_.sun_direction;
      ff.sun_intensity = settings_.sun_intensity;
      ff.sun_color = settings_.sun_color;
      ff.density = settings_.fog_density;
      ff.height_falloff = settings_.fog_height_falloff;
      ff.base_height = settings_.fog_base_height;
      ff.anisotropy = settings_.fog_anisotropy;
      ff.frame_index = frame_index_;
      lit = volumetric_fog_.AddToGraph(graph_, *raytracing_, tlas_slot, lit,
                                       depth_export,
                                       {render_width_, render_height_}, ff);
    }

    // Shell fur over the lit scene, depth-tested against the scene depth so the
    // core sphere occludes the far-side shells.
    if (view.fur_ball && !path_trace) {
      Mat4 model{};
      model.m[0] = model.m[5] = model.m[10] = model.m[15] = 1.0f;
      model.m[12] = view.fur_position.x;
      model.m[13] = view.fur_position.y;
      model.m[14] = view.fur_position.z;
      Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
      fur_.AddToGraph(graph_, lit, depth, model, view_proj,
                      applied_sun_direction_, sun_col,
                      rx::Max(settings_.ambient, 0.12f), FurPass::Params{});
    }

    // Order-independent transparency (weighted blended) over the lit scene.
    if (!view.oit.empty() && !path_trace) {
      Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
      WboitPass::LightingContext oit_light;
      oit_light.lights = frame.lights;
      oit_light.cluster_counts = cluster_counts_;
      oit_light.cluster_indices = cluster_indices_;
      base::MemCopy(oit_light.cluster_params, globals.cluster_params,
                  sizeof(oit_light.cluster_params));
      oit_light.froxel_volume = froxel_fog_.integrated().view;
      oit_light.froxel_sampler = froxel_fog_.volume_sampler();
      oit_light.froxel_near = FroxelFog::kNear;
      oit_light.froxel_far = FroxelFog::kFar;
      oit_light.froxel_enabled = froxel_on;
      lit = wboit_.AddToGraph(graph_, lit, depth, view.oit, view_proj,
                              applied_sun_direction_, sun_col,
                              rx::Max(settings_.ambient, 0.12f), render_width_,
                              render_height_, oit_light);
    }

    // True volumetric precipitation: stateless world-anchored rain streaks /
    // snow flakes with impact splashes, drawn pre-resolve with motion vectors
    // so TAA treats them like geometry. Gated by the sky-occlusion map (nothing
    // falls under the shelter roof). When this runs, the post-resolve
    // screen-space streak layer is skipped; it remains the volumetric=false and
    // path-trace fallback.
    if (precip_occlusion_active_ && precip_volume_ready &&
        depth_export != kInvalidResource && motion != kInvalidResource) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      PrecipVolume::Frame vf;
      vf.view_proj = view_proj;
      vf.prev_view_proj = globals.prev_view_proj;
      vf.cam_right = right;
      vf.cam_up = Cross(right, fwd);
      vf.cam_pos = view.camera.eye;
      vf.sun_direction = applied_sun_direction_;
      vf.sun_color = applied_sun_color_;
      vf.sun_intensity = applied_sun_intensity_;
      vf.ambient = rx::Max(settings_.ambient, 0.02f);
      vf.time = static_cast<f32>(time_seconds_);
      vf.dt = view.frame_delta_seconds;
      vf.intensity = settings_.weather.precipitation;
      vf.snow = settings_.weather.snow;
      // Wind yaw is the direction the wind blows toward; decompose to xz.
      vf.wind[0] =
          ::cosf(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      vf.wind[1] =
          ::sinf(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      vf.gustiness = settings_.weather.gustiness;
      vf.lightning = settings_.weather.lightning;
      vf.jitter[0] = globals.jitter[0];
      vf.jitter[1] = globals.jitter[1];
      precip_occlusion_.Params(vf.occl);
      vf.occl_range = PrecipOcclusion::y_range();
      vf.occlusion = precip_occlusion_.view();
      vf.occlusion_sampler = precip_occlusion_.sampler();
      vf.froxel_enabled = froxel_on;
      vf.froxel_volume = froxel_fog_.integrated().view;
      vf.froxel_sampler = froxel_fog_.volume_sampler();
      vf.rt_shadows = precip_rt; // matched to the TLAS build request above
      precip_volume_.AddToGraph(graph_, lit, depth_export, motion,
                                raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), tlas_slot, vf);
      precip_volume_drawn = true;
    }

    // NIF particle emitters: a cpu pool per placed instance of an emitting
    // mesh, stepped here and appended to the frame's billboard sets (lit
    // smoke/mist plus HDR additive fire).
    base::Vector<ParticleInstance> emitter_lit;
    base::Vector<ParticleInstance> emitter_additive;
    if (!mesh_emitters_.empty()) {
      emitter_sim_.BeginFrame(view.frame_delta_seconds, view.camera.eye);
      for (const DrawItem &item : view.draws) {
        if (const auto *emitters = mesh_emitters_.find(item.mesh)) {
          emitter_sim_.AddInstance(item.mesh, *emitters, item.transform);
        }
      }
      emitter_sim_.Simulate(&emitter_lit, &emitter_additive);
    }

    // Lit billboard particles blend over the resolved scene, faded against the
    // prepass depth, before temporal reconstruction. Either a cpu-uploaded set
    // or the gpu-simulated fountain.
    if (!view.particles.empty() || !emitter_lit.empty() ||
        !emitter_additive.empty() || view.gpu_particle_count > 0) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      ParticleSystem::Frame pf;
      pf.view_proj = view_proj;
      pf.prev_view_proj = globals.prev_view_proj;
      pf.cam_right = right;
      pf.cam_up = Cross(right, fwd);
      pf.sun_direction = settings_.sun_direction;
      pf.sun_color = settings_.sun_color;
      pf.sun_intensity = settings_.sun_intensity;
      pf.ambient = rx::Max(settings_.ambient, 0.15f);
      pf.near_plane = 0.1f;
      pf.soft_fade = 0.6f;
      pf.jitter[0] = globals.jitter[0];
      pf.jitter[1] = globals.jitter[1];
      // Lit translucency inputs: clustered lights + shadows + the fog volume.
      base::MemCopy(pf.cluster_params, globals.cluster_params,
                  sizeof(pf.cluster_params));
      pf.froxel_near = FroxelFog::kNear;
      pf.froxel_far = FroxelFog::kFar;
      pf.froxel_enabled = froxel_on;
      pf.lights = frame.lights;
      pf.cluster_counts = cluster_counts_;
      pf.cluster_indices = cluster_indices_;
      pf.local_shadow_faces = local_shadows_active_
                                  ? local_shadows_.face_buffer(frame_slot)
                                  : environment_->dummy_storage();
      pf.local_shadow_atlas = local_shadows_active_
                                  ? local_shadows_.atlas().view
                                  : environment_->shadow_dummy_view();
      pf.comparison_sampler = environment_->comparison_sampler();
      pf.froxel_volume = froxel_fog_.integrated().view;
      pf.froxel_sampler = froxel_fog_.volume_sampler();
      if (view.gpu_particle_count > 0) {
        ParticleSystem::Sim sim;
        sim.emitter[0] = view.gpu_particle_emitter.x;
        sim.emitter[1] = view.gpu_particle_emitter.y;
        sim.emitter[2] = view.gpu_particle_emitter.z;
        sim.dt = view.frame_delta_seconds;
        sim.count = view.gpu_particle_count;
        sim.mode = view.gpu_particle_mode;
        sim.radius = view.gpu_particle_radius;
        sim.intensity = view.gpu_particle_intensity;
        sim.time = static_cast<f32>(time_seconds_);
        pf.emissive = view.gpu_particle_mode == 1;
        gpu::BindingSetHandle particle_bindless =
            bindless_ ? bindless_->set() : gpu::BindingSetHandle{};
        particles_.SimulateAndDraw(graph_, lit, depth_export, motion, sim, pf,
                                   frame_index_ % 2, particle_bindless);
      } else {
        base::Vector<ParticleInstance> &demo_particles =
            view.particles_emissive ? emitter_additive : emitter_lit;
        for (const ParticleInstance &inst : view.particles)
          demo_particles.push_back(inst);
        gpu::BindingSetHandle particle_bindless =
            bindless_ ? bindless_->set() : gpu::BindingSetHandle{};
        particles_.AddToGraph(graph_, lit, depth_export, motion, emitter_lit,
                              emitter_additive, pf, frame_index_ % 2,
                              particle_bindless);
      }
    }

    // 3D gaussian splats: non-triangle primitives blended over the resolved
    // scene.
    if (!view.gaussians.empty()) {
      GaussianSplat::Frame gf;
      gf.view = view_mat;
      gf.proj_x = proj.m[0];
      gf.proj_y = proj.m[5];
      gf.near_plane = 0.1f;
      gf.screen_x = static_cast<f32>(render_width_);
      gf.screen_y = static_cast<f32>(render_height_);
      gaussians_.AddToGraph(graph_, lit, view.gaussians, gf, frame_index_ % 2);
    }

    // Bounds / acceleration-structure debug view: overlay the cull bounding
    // boxes. Mesh-shader meshlet demo: clusters cull + draw on the gpu,
    // composited into the lit scene with depth. Only active when a meshlet mesh
    // was uploaded.
    if (meshlet_.active()) {
      meshlet_visible_ =
          meshlet_.last_visible(frame_index_); // fence-safe, read before reset
      f32 planes[5][4];
      ExtractFrustumPlanes(view_proj, planes);
      meshlet_.AddToGraph(graph_, lit, depth, view_proj, planes,
                          view.camera.eye, frame_index_);
    }
    // Distant foliage imposters: instanced octahedral billboards with depth.
    const char *imposters_env = ::getenv("RX_IMPOSTERS");
    if (imposters_.active() && (!imposters_env || imposters_env[0] != '0')) {
      ImposterPass::Frame imf;
      imf.view_proj = view_proj;
      imf.camera_pos = view.camera.eye;
      imf.sun_direction = applied_sun_direction_;
      imf.sun_intensity = applied_sun_intensity_;
      imf.sun_color = applied_sun_color_;
      imposters_.AddToGraph(graph_, lit, depth, {render_width_, render_height_},
                            imf);
    }

    // Strand hair: ribbon draw over the lit scene with depth, node positions
    // fed by the physics strand sim through SetHairGroomPoints. The
    // transmittance volume this shades against was built above, before the
    // passes that sample it.
    if (hair_.active()) {
      hair_.AddToGraph(graph_, lit, depth, {render_width_, render_height_}, hair_frame,
                       frame_slot);
    }

    // Virtual geometry: cluster-DAG LOD cut, two-pass occlusion cull and
    // visibility-buffer raster, all on the gpu (single-pass fallback inside).
    if (vgeo_.active()) {
      VirtualGeometryPass::Frame vf;
      vf.view_proj = view_proj;
      ExtractFrustumPlanes(view_proj, vf.planes);
      vf.eye = view.camera.eye;
      // Screen pixels per world unit at distance 1 (|proj.m5| carries the
      // vulkan y-flip, hence the fabs).
      vf.proj_scale =
          ::fabsf(proj.m[5]) * static_cast<f32>(render_height_) * 0.5f;
      vf.proj_m00 = ::fabsf(proj.m[0]);
      vf.proj_m11 = ::fabsf(proj.m[5]);
      vf.error_pixels = VgeoError.get() > 0.0f ? VgeoError.get() : 1.0f;
      // Reversed-z infinite far: proj m[14] is the near plane distance.
      vf.near_plane = proj.m[14] > 0.0f ? proj.m[14] : 0.1f;
      vf.color = lit;
      vf.depth = depth;
      vf.width = render_width_;
      vf.height = render_height_;
      vf.debug = static_cast<u32>(rx::Max(VgeoDebug.get(), 0));
      vf.slot = frame_index_;
      vgeo_.AddToGraph(*device_, graph_, vf);
    }

    if (settings_.debug_view == DebugView::kBounds) {
      gpu_cull_.AddBoundsPass(graph_, lit, view_proj, cull_instance_count,
                              cull_slot);
    }

    // App translucents over the fully composited scene (after rx transparency,
    // before post/tonemap). Blends into `lit`, depth-tested against rx
    // geometry. Motion is passed here too: this phase still precedes TAA,
    // upscaling, and motion blur, and rx's own transparent particles already
    // alpha-blend velocity into `motion`, so app translucents get the same
    // target to overwrite it on moving surfaces (no depth_export in this phase).
    if (view.scene_transparent && !path_trace && depth != kInvalidResource) {
      add_scene_hook(view.scene_transparent, ScenePhase::kTransparent, lit,
                     depth, kInvalidResource, motion);
    }

    // Overdraw debug view: clear lit and additive-replay all geometry so the
    // heat ramp shows how many layers each pixel shaded.
    if (settings_.debug_view == DebugView::kOverdraw) {
      graph_.AddPass(
          "overdraw",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Write(lit, ResourceUsage::kColorAttachment);
          },
          [this, lit, view_proj, &frame, &view](PassContext &ctx) {
            overdraw_.Render(
                *ctx.cmd, ctx.graph->image(lit).view,
                {render_width_, render_height_}, view_proj,
                [this, &frame, &view, view_proj](gpu::CommandList &cmd) {
                  // This pass borrows shadow.vs, so it borrows its arena too.
                  cmd.BindTransient(
                      ShadowPass::kDrawRecordSet,
                      {gpu::Bind::StorageBuffer(0, frame.draw_records)});
                  for (const DrawItem &item : view.draws) {
                    const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
                    if (!mesh || !mesh->indices)
                      continue;
                    // view_proj sits where the cascade matrix goes (pushed by
                    // Render); the per-draw head leads the block.
                    ShadowPass::DrawPush draw_push{};
                    draw_push.draw_index =
                        static_cast<u32>(&item - view.draws.data()) + 1u;
                    cmd.PushConstants(&draw_push, sizeof(draw_push));
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
                    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
                      cmd.DrawIndexed(submesh.index_count, 1,
                                      submesh.index_offset, 0, 0);
                    }
                  }
                  overdraw_.BindInstanced(cmd, view_proj);
                  for (const InstanceStore::Group &group :
                       instances_.groups()) {
                    if (!group.alive)
                      continue;
                    const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
                    if (!mesh || !mesh->indices)
                      continue;
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindVertexBuffer(1, group.buffer);
                    cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
                    for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
                      cmd.DrawIndexed(submesh.index_count,
                                      static_cast<u32>(group.transforms.size()),
                                      submesh.index_offset, 0, 0);
                    }
                  }
                });
          });
    }

    // Editor debug lines over the resolved scene (depth-tested + overlay), just
    // before post/UI. Only added when the app supplied lines this frame.
    if (!view.debug_lines.empty() || !view.debug_lines_overlay.empty()) {
      graph_.AddPass(
          "debug_lines",
          [lit, depth](RenderGraph::PassBuilder &builder) {
            builder.Write(lit, ResourceUsage::kColorAttachment);
            if (depth != kInvalidResource)
              builder.Write(depth, ResourceUsage::kDepthAttachment);
          },
          [this, lit, depth, view_proj, &view](PassContext &ctx) {
            const gpu::GpuImage &color = ctx.graph->image(lit);
            gpu::ColorAttachment ca{.view = color.view, .load = gpu::LoadOp::kLoad};
            const bool have_depth = depth != kInvalidResource;
            gpu::DepthAttachment da{};
            if (have_depth)
              da = {.view = ctx.graph->image(depth).view,
                    .load = gpu::LoadOp::kLoad};
            ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                     .colors = base::Span(&ca, 1),
                                     .depth = have_depth ? &da : nullptr});
            DrawDebugLines(*ctx.cmd, view, view_proj,
                           {render_width_, render_height_});
            ctx.cmd->EndRendering();
          });
    }

    // SDF clipmap debug raymarch (RX_SDF_DEBUG): replace the lit scene with a
    // view built purely from the SDF clipmap (1 = distance field tinted by
    // clip, 2 = hit albedo + gradient-normal shading). Standalone S1
    // verification.
    if (sdf_clipmap_ && sdf_available_ && SdfDebugOpt) {
      sdf_clipmap_->AddDebugPass(
          graph_, lit, {render_width_, render_height_}, globals.inv_view_proj,
          view.camera.eye, static_cast<u32>(static_cast<int>(SdfDebugOpt)),
          frame_index_);
    }
  } // end raster path

  // Water has no place in the path tracer (blend geometry never enters the
  // tlas), so composite the raster water pass over the path-traced image. A
  // small opaque depth prepass (direct draws, no gpu cull) gives water correct
  // occlusion and soft shorelines; reflections/shadows trace inline against the
  // path tracer's tlas. The atmosphere/cloud passes that normally precede water
  // are skipped under path tracing, so water is simply the last thing
  // composited.
  if (path_trace && water_pipeline_active && !transparent.empty()) {
    ResourceHandle pt_normals =
        graph_.CreateTexture({.name = "pt_water_normals",
                              .format = kNormalFormat,
                              .width = render_width_,
                              .height = render_height_});
    ResourceHandle pt_motion = graph_.CreateTexture({.name = "pt_water_motion",
                                                     .format = kMotionFormat,
                                                     .width = render_width_,
                                                     .height = render_height_});
    ResourceHandle pt_depth = graph_.CreateTexture({.name = "pt_water_depth",
                                                    .format = kDepthFormat,
                                                    .width = render_width_,
                                                    .height = render_height_});
    ResourceHandle pt_depth_export =
        graph_.CreateTexture({.name = "pt_water_depth_export",
                              .format = gpu::Format::kR32Float,
                              .width = render_width_,
                              .height = render_height_});
    graph_.AddPass(
        "pt_water_prepass",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(pt_normals, ResourceUsage::kColorAttachment);
          builder.Write(pt_motion, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth_export, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth, ResourceUsage::kDepthAttachment);
        },
        [this, pt_normals, pt_motion, pt_depth_export, pt_depth, globals_set,
         update_globals_set, frame_slot, &frame, &view,
         view_proj](PassContext &ctx) {
          // First globals-set user on the path-traced frame: uniform + tlas
          // (the transparent pass right after wants the tlas for water
          // reflections).
          update_globals_set(ctx, kInvalidResource, false, /*want_tlas=*/true);

          gpu::ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(pt_normals).view};
          colors[1] = {.view = ctx.graph->image(pt_motion).view};
          colors[2] = {.view = ctx.graph->image(pt_depth_export).view};
          gpu::DepthAttachment depth_attachment{
              .view = ctx.graph->image(pt_depth).view,
              .clear = 0.0f}; // reversed z clears to far = 0
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = base::Span(colors, 3),
                                   .depth = &depth_attachment});

          environment_->WriteEnvSet(
              env_prepass_sets_[frame_slot], gpu::TextureView{}, nullptr,
              gpu::TextureView{}, gpu::GpuBuffer{}, 0, gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, 0, gpu::TextureView{}, gpu::GpuBuffer{}, gpu::GpuBuffer{},
              gpu::GpuBuffer{}, gpu::GpuBuffer{}, gpu::TextureView{}, gpu::GpuBuffer{},
              gpu::TextureView{}, gpu::TextureView{}, gpu::TextureView{}, gpu::TextureView{},
              gpu::GpuBuffer{}, gpu::TextureView{}, gpu::TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : gpu::TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : gpu::TextureView{});
          mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set,
                                      env_prepass_sets_[frame_slot]);
          gpu::BindingSetHandle bound_material{};
          bool skinned_bound = false;
          bool masked_bound =
              false; // BindPrepass bound the opaque static variant
          for (const DrawItem &item : view.draws) {
            const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            MeshPushConstants push{};
            push.draw_index = static_cast<u32>(&item - view.draws.data()) + 1u;
            if (mesh->terrain_lod) {
              base::MemCopy(push.detail_rect, view.detail_rect,
                          sizeof(push.detail_rect));
            }
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette.address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
              push.prev_skin_offset = PrevSkinOffset(item);
            }
            if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                item.morph_count > 0) {
              push.morph_delta_address = mesh->morph_deltas.address;
              push.morph_weight_address = frame.morph_weights.address;
              push.morph_first = static_cast<u32>(item.morph_offset);
              push.morph_count = item.morph_count;
              push.morph_vertex_count = mesh->vertex_count;
            }
            mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue; // transparency owns its own depth
              if (draw_skinned != skinned_bound ||
                  submesh.alpha_mask != masked_bound) {
                mesh_pipeline_->SetPrepassVariant(*ctx.cmd, draw_skinned,
                                                  submesh.alpha_mask);
                skinned_bound = draw_skinned;
                masked_bound = submesh.alpha_mask;
              }
              gpu::BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              mesh_pipeline_->DrawSubmesh(*ctx.cmd, submesh);
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const gpu::GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            MeshPushConstants push{};
            const gpu::GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue;
              mesh_pipeline_->SetInstancedPrepass(*ctx.cmd, submesh.alpha_mask);
              const gpu::BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, 0, 0);
            }
          }
          ctx.cmd->EndRendering();
        });
    lit = add_water(scene_color, pt_depth, pt_depth_export, pt_motion,
                    kInvalidResource, kInvalidResource, false, 0u, tlas_slot,
                    /*globals_written=*/true);
  }

  // Rebuild next frame's shading rates from the finished render-res frame
  // (post-transparency, pre-resolve: what the scene pass actually shades).
  if (vrs_active_) {
    vrs_.AddToGraph(graph_, lit, motion, {render_width_, render_height_},
                    settings_.vrs_threshold, /*motion_scale=*/2.5f);
  }

  // RCGI screen cache: snapshot this frame's lit HDR colour + depth so next
  // frame's final gather can read last-frame radiance for its screen cache.
  // Only when the M2 gather ran (probes-only / off record nothing).
  if (rcgi_world && !rcgi_probes_only && lit != kInvalidResource &&
      depth_export != kInvalidResource) {
    rcgi_->AddHistoryCopy(graph_, lit, depth_export,
                          {render_width_, render_height_});
  }

  // The path tracer already resolved antialiasing through accumulation; the
  // raster path runs its temporal/upscale resolve here.
  ResourceHandle post_input = lit;
  if (!path_trace) {
    switch (settings_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(
          graph_, lit, motion, frame_index_,
          settings_.debug_view == DebugView::kTemporalHistory ? 1u
          : settings_.debug_view == DebugView::kMotionVectors ? 2u
                                                              : 0u);
      break;
    case AntiAliasingMode::kUpscaler: {
      ResourceHandle upscaled = upscaler_->AddToGraph(
          graph_, {.color = lit,
                   .depth = depth_export,
                   .motion_vectors = motion,
                   .jitter_x = jitter_x,
                   .jitter_y = jitter_y,
                   .sharpness = settings_.sharpness,
                   .frame_delta_seconds = view.frame_delta_seconds,
                   .camera_near = 0.1f,
                   .camera_fov_y = view.camera.fov_y,
                   .reset_history = first_frame,
                   .frame_index = frame_index_});
      if (upscaled != kInvalidResource)
        post_input = upscaled;
      break;
    }
    case AntiAliasingMode::kNone:
      break;
    }
  }

  // Dimensions of the aa-resolved image the post stack runs at.
  bool upscaled = !path_trace &&
                  settings_.aa_mode == AntiAliasingMode::kUpscaler &&
                  post_input != lit;
  u32 post_width = upscaled ? output_width_ : render_width_;
  u32 post_height = upscaled ? output_height_ : render_height_;

  // Depth of field on the resolved frame, before motion blur streaks over it.
  if (settings_.dof && !path_trace && depth_export != kInvalidResource) {
    DepthOfFieldPass::Frame df;
    df.aperture = settings_.dof_aperture;
    df.focus_distance = settings_.dof_focus;
    post_input = dof_.AddToGraph(graph_, post_input, depth_export,
                                 {post_width, post_height}, df);
  }

  // Motion blur right after the AA resolve (before precipitation streaks and
  // the linear-hdr export). Uses the render-res prepass velocity; uv-space
  // velocities are resolution independent so the upscaled path works too.
  if (settings_.motion_blur && !path_trace && motion != kInvalidResource) {
    MotionBlurPass::Frame mb;
    mb.shutter = settings_.motion_blur_shutter;
    mb.frame_index = frame_index_;
    if (MotionBlurDebugVel.overridden()) {
      mb.debug_velocity[0] = static_cast<f32>(double(MotionBlurDebugVel));
    }
    post_input = motion_blur_.AddToGraph(graph_, post_input, motion,
                                         {post_width, post_height}, mb);
  }

  // Screen-space precipitation streaks, composited at output resolution after
  // the AA resolve so they stay crisp (TAA would otherwise smear them) and
  // tonemap with the scene. Driven by weather; surface wetness was applied
  // pre-resolve. The non-volumetric fallback: skipped whenever the 3D particle
  // volume drew.
  if (settings_.weather.precipitation > 0.0f && !path_trace &&
      !precip_volume_drawn) {
    Precipitation::Frame pf;
    pf.inv_view_proj = globals.inv_view_proj;
    pf.camera_pos = view.camera.eye;
    pf.time = static_cast<f32>(time_seconds_);
    pf.intensity = settings_.weather.precipitation;
    pf.snow = settings_.weather.snow;
    post_input = precipitation_.AddToGraph(graph_, post_input,
                                           {post_width, post_height}, pf);
  }

  // Color-only app overlays run after temporal/depth-aware effects so they do
  // not need scene depth or motion vectors, but before exposure, bloom and
  // tonemapping so HDR sprites still participate in the presentation stack.
  if (view.hdr_overlay && post_input != kInvalidResource) {
    auto overlay = view.hdr_overlay;
    ResourceHandle overlay_source = post_input;
    ResourceHandle overlay_target = graph_.CreateTexture(
        {.name = "app_hdr_overlay",
         .format = kSceneColorFormat,
         .width = post_width,
         .height = post_height});
    graph_.AddPass(
        "app_hdr_overlay_copy",
        [overlay_source, overlay_target](RenderGraph::PassBuilder& builder) {
          builder.Read(overlay_source, ResourceUsage::kSampledFragment);
          builder.Write(overlay_target, ResourceUsage::kColorAttachment);
        },
        [this, overlay_source, overlay_target, post_width,
         post_height](PassContext& ctx) {
          const gpu::GpuImage& source = ctx.graph->image(overlay_source);
          const gpu::GpuImage& color = ctx.graph->image(overlay_target);
          gpu::ColorAttachment copy{.view = color.view,
                               .load = gpu::LoadOp::kDontCare,
                               .store = gpu::StoreOp::kStore};
          ctx.cmd->BeginRendering(
              {.extent = {post_width, post_height}, .colors = base::Span(&copy, 1)});
          ctx.cmd->BindPipeline(hdr_overlay_copy_pipeline_);
          ctx.cmd->BindTransient(
              0, {gpu::Bind::Combined(0, source.view, hdr_overlay_sampler_)});
          ctx.cmd->Draw(3);
          ctx.cmd->EndRendering();
        });
    graph_.AddPass(
        "app_hdr_overlay",
        [overlay_target](RenderGraph::PassBuilder& builder) {
          builder.Write(overlay_target, ResourceUsage::kColorAttachment);
        },
        [this, overlay = base::move(overlay), overlay_target, post_width,
         post_height](PassContext& ctx) {
          const gpu::GpuImage& color = ctx.graph->image(overlay_target);
          HdrOverlayContext hc;
          hc.cmd = ctx.cmd;
          hc.device = ctx.device;
          hc.color = &color;
          hc.color_view = color.view;
          hc.color_format = color.format;
          hc.extent = {post_width, post_height};
          hc.frame_slot = frame_index_ % kFramesInFlight;
          hc.frames_in_flight = kFramesInFlight;
          overlay(hc);
        });
    post_input = overlay_target;
  }

  // Linear-hdr export: copy the resolved scene (pre-tonemap) into a host
  // buffer.
  hdr_pending_ = false;
  if (!hdr_path_.empty() && time_seconds_ >= hdr_at_) {
    u64 need = static_cast<u64>(post_width) * post_height * sizeof(f32) * 4;
    if (hdr_readback_.size != need) {
      device_->DestroyBuffer(hdr_readback_);
      hdr_readback_ = device_->CreateBuffer(need, gpu::kBufferUsageStorage, true);
    }
    hdr_width_ = post_width;
    hdr_height_ = post_height;
    hdr_pending_ = hdr_readback_.mapped != nullptr;
    if (hdr_pending_) {
      graph_.AddPass(
          "hdr_capture",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(post_input, ResourceUsage::kSampledCompute);
          },
          [this, post_input, post_width, post_height](PassContext &ctx) {
            u32 push[2] = {post_width, post_height};
            ctx.cmd->BindPipeline(hdr_pipeline_);
            ctx.cmd->BindTransient(
                0, {gpu::Bind::StorageBuffer(0, hdr_readback_),
                    gpu::Bind::Sampled(1, ctx.graph->image(post_input))});
            ctx.cmd->PushConstants(push, sizeof(push));
            ctx.cmd->Dispatch2D({post_width, post_height});
            ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite,
                                   gpu::BarrierScope::kHostRead);
          });
    }
  }

  // Reference comparison sits here, on the scene-linear image, so the
  // reference and the render go through the SAME exposure, bloom and tonemap.
  // Comparing after the display transform would measure the tonemap.
  post_input = reference_compare_.AddToGraph(graph_, post_input, {post_width, post_height},
                                             static_cast<u32>(settings_.tonemap));

  exposure_.AddToGraph(graph_, post_input, post_width, post_height,
                       view.frame_delta_seconds);
  ResourceHandle bloom = kInvalidResource;
  ResourceHandle flare_src = kInvalidResource;
  if (settings_.bloom) {
    bloom =
        bloom_.AddToGraph(graph_, post_input, post_width, post_height,
                          settings_.lens_flare > 0.0f ? &flare_src : nullptr);
  }

  ResourceHandle backbuffer =
      capture_offscreen_
          ? graph_.ImportBackbuffer(capture_image_, gpu::ResourceState::kCopySrc)
          : graph_.ImportBackbuffer(swapchain_->image(image_index));

  post_->SetGrade(
      settings_.color_grade); // rebakes the lut only when it changes
  PostPass::Params post_params{
      static_cast<u32>(settings_.tonemap), settings_.bloom_intensity,
      bloom != kInvalidResource ? 1u : 0u,
      settings_.color_grade != ColorGrade::kNeutral ? 1u : 0u};
  switch (swapchain_->color_space()) {
  case gpu::ColorSpace::kHdr10Pq:
    post_params.output_transfer = 1;
    break;
  case gpu::ColorSpace::kScRgbLinear:
    post_params.output_transfer = 2;
    break;
  default:
    break;
  }
  if (int forced = HdrForceTransfer; forced == 1 || forced == 2) {
    post_params.output_transfer = static_cast<u32>(forced);
  }
  post_params.paper_white = settings_.hdr_paper_white;
  // Flare needs its prefiltered highlight source from the bloom chain; without
  // bloom the ghosts would mirror the raw scene.
  post_params.flare_intensity =
      flare_src != kInvalidResource ? settings_.lens_flare : 0.0f;
  post_params.aberration = settings_.chromatic_aberration;
  post_params.vignette = settings_.vignette;
  post_params.grain = settings_.film_grain;
  post_params.grain_seed = static_cast<f32>(frame_index_ % 1024) * 0.6180339f;
  // Upscalers sharpen inside their own resolve; rx's TAA has none of its own.
  if (!path_trace && settings_.aa_mode == AntiAliasingMode::kTaa)
    post_params.sharpen = settings_.sharpness;
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder &builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        if (bloom != kInvalidResource)
          builder.Read(bloom, ResourceUsage::kSampledFragment);
        if (flare_src != kInvalidResource)
          builder.Read(flare_src, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, bloom, flare_src, backbuffer,
       post_params](PassContext &ctx) {
        gpu::TextureView bloom_view = bloom != kInvalidResource
                                     ? ctx.graph->image(bloom).view
                                     : ctx.graph->image(post_input).view;
        gpu::TextureView flare_view = flare_src != kInvalidResource
                                     ? ctx.graph->image(flare_src).view
                                     : bloom_view;
        post_->Record(ctx, ctx.graph->image(post_input).view, bloom_view,
                      flare_view, exposure_.exposure_buffer(),
                      exposure_.exposure_buffer_size(),
                      ctx.graph->image(backbuffer).view,
                      ctx.graph->image(backbuffer).extent, post_params);
      });

#if defined(RX_HAS_FSR3)
  // Frame generation: snapshot the pre-UI backbuffer as the interpolation
  // source, so the generated frame carries no smeared UI (it is re-drawn
  // crisp onto the interpolated image in the present path).
  if (fg_active_frame_ && framegen_) {
    graph_.AddPass(
        "fg_hudless",
        [&](RenderGraph::PassBuilder &b) {
          b.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer](PassContext &ctx) {
          const gpu::GpuImage &src = ctx.graph->image(backbuffer);
          const gpu::GpuImage &dst = framegen_->hudless();
          gpu::TextureBarrier pre[] = {gpu::Transition(src, gpu::ResourceState::kColorTarget,
                                             gpu::ResourceState::kCopySrc),
                                  gpu::Transition(dst,
                                             gpu::ResourceState::kShaderReadCompute,
                                             gpu::ResourceState::kCopyDst)};
          ctx.cmd->TextureBarriers(pre);
          ctx.cmd->CopyTexture(src, dst);
          gpu::TextureBarrier post[] = {
              gpu::Transition(src, gpu::ResourceState::kCopySrc,
                         gpu::ResourceState::kColorTarget),
              gpu::Transition(dst, gpu::ResourceState::kCopyDst,
                         gpu::ResourceState::kShaderReadCompute)};
          ctx.cmd->TextureBarriers(post);
        });
  }
#endif

  if (view.ui_draw || view.hud_draw) {
    // Backdrop blur: when a frosted widget is present (and the surface lets us
    // sample the backbuffer), capture + Gaussian-blur the post-tonemap frame
    // into ui_frost; the UI backend samples it for frosted panels.
    ResourceHandle ui_frost = kInvalidResource;
    if (view.needs_blur && ui_blur_ && swapchain_->can_sample()) {
      ui_frost = ui_blur_->AddToGraph(graph_, backbuffer, output_width_,
                                      output_height_);
    }

    graph_.AddPass(
        "ui",
        [&](RenderGraph::PassBuilder &builder) {
          if (ui_frost != kInvalidResource)
            builder.Read(ui_frost, ResourceUsage::kSampledFragment);
          builder.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer, ui_frost, &view](PassContext &ctx) {
          gpu::ColorAttachment color{.view = ctx.graph->image(backbuffer).view,
                                .load = gpu::LoadOp::kLoad};
          // Hand the blurred backdrop to the UI before it records (the closure
          // reads view.blur_source); null when blur is not in play this frame.
          view.blur_source = ui_frost != kInvalidResource
                                 ? ctx.graph->image(ui_frost).view
                                 : gpu::TextureView{};
          view.blur_sampler = ui_frost != kInvalidResource ? ui_blur_->sampler()
                                                           : gpu::SamplerHandle{};
          ctx.cmd->BeginRendering(
              {.extent = ctx.graph->image(backbuffer).extent,
               .colors = base::Span(&color, 1)});
          if (view.hud_draw)
            view.hud_draw(*ctx.cmd);
          if (view.ui_draw)
            view.ui_draw(*ctx.cmd);
          ctx.cmd->EndRendering();
        });
  }
}

} // namespace rx::render
