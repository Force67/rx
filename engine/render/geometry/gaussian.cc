#include "render/geometry/gaussian.h"

#include <math.h>
#include <string.h>

#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/strings/xstring.h"
#include "foundation/algorithm/sort.h"
#include "foundation/files/file_system.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/strings/text_reader.h"
#include "shaders/gsplat_ps_hlsl.h"
#include "shaders/gsplat_vs_hlsl.h"

namespace rx::render {
namespace {

struct GaussianPush {
  Mat4 view;
  f32 proj_x;
  f32 proj_y;
  f32 near_plane;
  f32 screen_x;
  f32 screen_y;
  f32 pad[3];
};

u32 PlyTypeSize(const base::String& t) {
  if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
  if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
  if (t == "int" || t == "uint" || t == "int32" || t == "uint32" || t == "float" || t == "float32")
    return 4;
  if (t == "double" || t == "float64") return 8;
  return 0;
}

f32 Sigmoid(f32 x) { return 1.0f / (1.0f + ::expf(-x)); }

}  // namespace

bool GaussianSplat::Initialize(Device& device, Format color_format) {
  // TODO(rhi): blend preset mismatch: old alpha factors were ZERO/ONE (dst alpha
  // preserved); kAlpha uses ONE/ONE_MINUS_SRC_ALPHA.
  pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_gsplat_vs_hlsl),
      .fragment = RX_SHADER(k_gsplat_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format},
      .blend = {BlendMode::kAlpha},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}}}},
      .push_constant_size = PushSize<GaussianPush>(),
      .debug_name = "gaussian_splat",
  });
  if (!pipeline_) {
    RX_ERROR("gaussian pipeline creation failed");
    return false;
  }

  for (u32 i = 0; i < kFramesInFlight; ++i) {
    buffers_[i] = device.CreateBuffer(static_cast<u64>(kMaxGaussians) * sizeof(GaussianInstance),
                                      kBufferUsageStorage, true);
    if (!buffers_[i].mapped) return false;
  }
  return true;
}

void GaussianSplat::AddToGraph(RenderGraph& graph, ResourceHandle color,
                               const base::Vector<GaussianInstance>& gaussians, const Frame& frame,
                               u32 frame_slot) {
  if (gaussians.empty()) return;
  u32 count = rx::Min(static_cast<u32>(gaussians.size()), kMaxGaussians);

  // Sort back-to-front by view depth (front = -z, so most-negative first).
  base::Vector<u32> order(count);
  for (u32 i = 0; i < count; ++i) order[i] = i;
  const Mat4& v = frame.view;
  auto view_z = [&](u32 i) {
    const GaussianInstance& g = gaussians[i];
    return v.m[2] * g.position[0] + v.m[6] * g.position[1] + v.m[10] * g.position[2] + v.m[14];
  };
  // Stable: equal depths keep splat order.
  rx::StableSort(order.data(), order.data() + order.size(),
                 [&](u32 a, u32 b) { return view_z(a) < view_z(b); });

  GaussianInstance* dst = static_cast<GaussianInstance*>(buffers_[frame_slot].mapped);
  for (u32 i = 0; i < count; ++i) dst[i] = gaussians[order[i]];
  GpuBuffer buffer = buffers_[frame_slot];

  graph.AddPass(
      "gaussian_splat",
      [&](RenderGraph::PassBuilder& builder) { builder.Write(color, ResourceUsage::kColorAttachment); },
      [this, color, buffer, count, frame](PassContext& ctx) {
        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachment[] = {{.view = target.view, .load = LoadOp::kLoad}};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachment});

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, buffer, 0, count * sizeof(GaussianInstance))});

        GaussianPush push{};
        push.view = frame.view;
        push.proj_x = frame.proj_x;
        push.proj_y = frame.proj_y;
        push.near_plane = frame.near_plane;
        push.screen_x = frame.screen_x;
        push.screen_y = frame.screen_y;
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, count, 0, 0);
        ctx.cmd->EndRendering();
      });
}

void GaussianSplat::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (u32 i = 0; i < kFramesInFlight; ++i) device.DestroyBuffer(buffers_[i]);
}

bool LoadGaussianPly(const base::String& path, base::Vector<GaussianInstance>* out) {
  base::Vector<u8> bytes;
  if (!fs::ReadFile(path, &bytes)) {
    RX_WARN("gaussian ply: cannot open {}", path);
    return false;
  }
  // The header is text lines, the body binary or more lines, both read from
  // this one cursor.
  LineReader lines(base::StringRef(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  base::StringRef line;
  if (!lines.Next(&line) || !line.starts_with("ply")) {
    RX_WARN("gaussian ply: {} is not a ply file", path);
    return false;
  }

  struct Prop {
    base::String name;
    u32 size = 0;
    u32 offset = 0;
  };
  struct Elem {
    base::String name;
    u64 count = 0;
    base::Vector<Prop> props;
    u32 stride = 0;
    bool has_list = false;
  };
  base::Vector<Elem> elems;
  bool binary = false, little = true;
  while (lines.Next(&line)) {
    TokenReader ls(line);
    base::String tok;
    ls.Next(&tok);
    if (tok == "end_header") break;
    if (tok == "format") {
      base::String fmt;
      ls.Next(&fmt);
      binary = fmt.starts_with("binary");
      little = fmt != "binary_big_endian";
    } else if (tok == "element") {
      Elem e;
      ls.Next(&e.name);
      ls.Next(&e.count);
      elems.push_back(base::move(e));
    } else if (tok == "property" && !elems.empty()) {
      base::String type;
      ls.Next(&type);
      Elem& e = elems.back();
      if (type == "list") {  // face index lists; not present on splat vertices
        base::String a, b, nm;
        ls.Next(&a);
        ls.Next(&b);
        ls.Next(&nm);
        e.props.push_back({nm, 0, 0});
        e.has_list = true;
      } else {
        base::String nm;
        ls.Next(&nm);
        u32 sz = PlyTypeSize(type);
        e.props.push_back({nm, sz, e.stride});
        e.stride += sz;
      }
    }
  }
  if (binary && !little) {
    RX_WARN("gaussian ply: big-endian binary is not supported");
    return false;
  }
  u64 body = lines.position();

  const f32 kC0 = 0.28209479177387814f;  // sh band 0 constant
  const u64 kCap = 1u << 18;             // matches the renderer's gaussian budget

  for (Elem& e : elems) {
    if (e.name != "vertex") {  // skip other elements (e.g. faces) to reach vertex
      if (binary) {
        if (e.has_list) {
          RX_WARN("gaussian ply: variable-size element before vertex is unsupported");
          return false;
        }
        body += e.count * e.stride;
      } else {
        for (u64 i = 0; i < e.count && lines.Next(&line); ++i) {
        }
      }
      continue;
    }

    auto index_of = [&](const char* nm) -> int {
      for (size_t i = 0; i < e.props.size(); ++i)
        if (e.props[i].name == nm) return static_cast<int>(i);
      return -1;
    };
    int ix = index_of("x"), iy = index_of("y"), iz = index_of("z");
    if (ix < 0 || iy < 0 || iz < 0) {
      RX_WARN("gaussian ply: vertex element has no x/y/z");
      return false;
    }
    int iop = index_of("opacity");
    int is0 = index_of("scale_0"), is1 = index_of("scale_1"), is2 = index_of("scale_2");
    int ir0 = index_of("rot_0"), ir1 = index_of("rot_1"), ir2 = index_of("rot_2"),
        ir3 = index_of("rot_3");
    int if0 = index_of("f_dc_0"), if1 = index_of("f_dc_1"), if2 = index_of("f_dc_2");

    auto decode = [](const u8* rec, const Prop& p) -> f32 {
      if (p.size == 4) {
        float v;
        base::MemCopy(&v, rec + p.offset, 4);
        return v;
      }
      if (p.size == 8) {
        double v;
        base::MemCopy(&v, rec + p.offset, 8);
        return static_cast<f32>(v);
      }
      if (p.size == 2) {
        u16 v;
        base::MemCopy(&v, rec + p.offset, 2);
        return static_cast<f32>(v);
      }
      if (p.size == 1) return static_cast<f32>(rec[p.offset]);
      return 0.0f;
    };

    base::Vector<u8> rec(e.stride);
    base::Vector<f32> vals(e.props.size());  // every entry is rewritten per record
    bool truncated = false;
    for (u64 i = 0; i < e.count; ++i) {
      if (binary) {
        if (body > bytes.size() || bytes.size() - body < e.stride) break;
        base::MemCopy(rec.data(), bytes.data() + body, e.stride);
        body += e.stride;
        for (size_t k = 0; k < e.props.size(); ++k) vals[k] = decode(rec.data(), e.props[k]);
      } else {
        if (!lines.Next(&line)) break;
        TokenReader vs(line);
        for (size_t k = 0; k < e.props.size(); ++k) {
          f32 v = 0.0f;
          vs.Next(&v);
          vals[k] = v;
        }
      }
      if (out->size() >= kCap) {
        truncated = true;
        break;
      }
      auto at = [&](int j) { return j >= 0 ? vals[static_cast<size_t>(j)] : 0.0f; };
      GaussianInstance g;  // defaults cover any missing properties
      g.position[0] = at(ix);
      g.position[1] = at(iy);
      g.position[2] = at(iz);
      if (iop >= 0) g.opacity = Sigmoid(at(iop));
      if (is0 >= 0 && is1 >= 0 && is2 >= 0) {
        g.scale[0] = ::expf(at(is0));
        g.scale[1] = ::expf(at(is1));
        g.scale[2] = ::expf(at(is2));
      }
      if (if0 >= 0 && if1 >= 0 && if2 >= 0) {
        g.color[0] = rx::Clamp(0.5f + kC0 * at(if0), 0.0f, 1.0f);
        g.color[1] = rx::Clamp(0.5f + kC0 * at(if1), 0.0f, 1.0f);
        g.color[2] = rx::Clamp(0.5f + kC0 * at(if2), 0.0f, 1.0f);
      }
      if (ir0 >= 0 && ir1 >= 0 && ir2 >= 0 && ir3 >= 0) {
        f32 w = at(ir0), x = at(ir1), y = at(ir2), z = at(ir3);  // inria stores wxyz
        f32 len = ::sqrtf(w * w + x * x + y * y + z * z);
        if (len < 1e-8f) len = 1.0f;
        g.rotation[0] = x / len;
        g.rotation[1] = y / len;
        g.rotation[2] = z / len;
        g.rotation[3] = w / len;
      }
      out->push_back(g);
    }
    if (truncated) RX_WARN("gaussian ply: clamped to {} splats", kCap);
    break;  // vertex element handled; ignore anything after it
  }

  if (out->empty()) {
    RX_WARN("gaussian ply: no vertices read from {}", path);
    return false;
  }
  RX_INFO("gaussian ply: loaded {} splats from {}", out->size(), path);
  return true;
}

}  // namespace rx::render
