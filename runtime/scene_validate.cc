#include "scene_validate.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "asset/materialx.h"
#include "asset/procedural_texture.h"
#include "asset/vfs.h"
#include "core/log.h"
#include "ecs/world.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "scene/components.h"
#include "scene_authoring.h"

namespace rx {
namespace {

struct Finding {
  bool error = true;
  const char* check = "";
  std::string entity;  // Name.value, empty when the entity is unnamed
  int line = 0;        // source line of the entity's `entity` keyword, 0 = file-wide
  std::string message;
};

// One `Component.prop = value` assignment as it was written.
struct SourceAssign {
  u32 entity_index = 0;
  int line = 0;
  std::string comp;
  std::string prop;
  std::string raw;
};

struct Source {
  std::vector<int> entity_lines;  // source line of each `entity` keyword, in order
  std::vector<SourceAssign> assigns;
};

// Re-reads the file the way the loader tokenizes it (trim, skip blank and #/;
// comments, split the key on the first dot). Two things need the raw text that
// LoadScene throws away: pointing a finding at a line rather than an entity
// ordinal, and seeing what the value parser silently DID with a literal, which
// is not recoverable from the value it produced.
Source ScanSource(const std::string& path) {
  Source source;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) continue;
    const size_t b = line.find_last_not_of(" \t\r\n");
    const std::string_view t(line.data() + a, b - a + 1);
    if (t[0] == '#' || t[0] == ';') continue;
    if (t == "entity") {
      source.entity_lines.push_back(line_no);
      continue;
    }
    if (source.entity_lines.empty()) continue;  // stray line before the first entity
    const size_t eq = t.find('=');
    if (eq == std::string_view::npos) continue;  // tag component
    std::string_view key = t.substr(0, eq);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
    const size_t dot = key.find('.');
    if (dot == std::string_view::npos) continue;
    std::string_view raw = t.substr(eq + 1);
    const size_t raw_start = raw.find_first_not_of(" \t");
    raw = raw_start == std::string_view::npos ? std::string_view{} : raw.substr(raw_start);
    source.assigns.push_back({static_cast<u32>(source.entity_lines.size() - 1), line_no,
                              std::string(key.substr(0, dot)), std::string(key.substr(dot + 1)),
                              std::string(raw)});
  }
  return source;
}

// INVARIANT this file rests on throughout: the validator owns a freshly
// constructed World that nothing else has touched, so World::Create hands out
// entity indices 0, 1, 2, ... with generation 0, and LoadScene calls it once per
// `entity` block in file order. An entity's index is therefore its block
// ordinal, which is what turns a finding into a file:line, what lets a source
// assignment name the entity it belongs to, and what lets the walk below reach
// every entity at all (the ecs has no all-entities iterator, only Each<T...>).
std::vector<ecs::Entity> AllEntities(ecs::World& world) {
  std::vector<ecs::Entity> out;
  const size_t count = world.entity_count();
  for (size_t index = 0; index < count; ++index) {
    const ecs::Entity entity{static_cast<u32>(index), 0};
    if (world.IsAlive(entity)) out.push_back(entity);
  }
  return out;
}

class Report {
 public:
  Report(ecs::World& world, std::vector<int> entity_lines)
      : world_(world), entity_lines_(std::move(entity_lines)) {}

  void Error(ecs::Entity entity, const char* check, std::string message) {
    Add(true, entity, 0, check, std::move(message));
  }
  void Warn(ecs::Entity entity, const char* check, std::string message) {
    Add(false, entity, 0, check, std::move(message));
  }
  // For the checks that read the source text and so know the exact assignment,
  // not just the entity block it sits in.
  void ErrorAt(ecs::Entity entity, int line, const char* check, std::string message) {
    Add(true, entity, line, check, std::move(message));
  }
  // A finding about the file as a whole rather than about one entity.
  void FileError(const char* check, std::string message) {
    findings_.push_back({true, check, {}, 0, std::move(message)});
    ++errors_;
  }
  void FileWarn(const char* check, std::string message) {
    findings_.push_back({false, check, {}, 0, std::move(message)});
  }

  // File order, so the report reads down the scene the way the author wrote it.
  // File-wide findings carry no line and go last.
  void SortByLine() {
    std::stable_sort(findings_.begin(), findings_.end(), [](const Finding& a, const Finding& b) {
      return (a.line ? a.line : INT_MAX) < (b.line ? b.line : INT_MAX);
    });
  }

  const std::vector<Finding>& findings() const { return findings_; }
  u32 errors() const { return errors_; }
  u32 warnings() const { return static_cast<u32>(findings_.size()) - errors_; }

 private:
  void Add(bool error, ecs::Entity entity, int line, const char* check, std::string message) {
    Finding finding;
    finding.error = error;
    finding.check = check;
    if (const scene::Name* name = world_.Get<scene::Name>(entity)) finding.entity = name->value;
    finding.line = line;
    if (line == 0 && entity.index < entity_lines_.size())
      finding.line = entity_lines_[entity.index];
    finding.message = std::move(message);
    findings_.push_back(std::move(finding));
    if (error) ++errors_;
  }

  ecs::World& world_;
  std::vector<int> entity_lines_;
  std::vector<Finding> findings_;
  u32 errors_ = 0;
};

// How many of PropValue::f a type actually uses. Zero for the types that carry
// no float, which is what keeps the non-finite sweep generic over the registry
// instead of listing components by hand.
u32 FloatLanes(edit::PropType type) {
  switch (type) {
    case edit::PropType::kF32: return 1;
    case edit::PropType::kVec2: return 2;
    case edit::PropType::kVec3: return 3;
    case edit::PropType::kVec4:
    case edit::PropType::kQuat:
    case edit::PropType::kColor: return 4;
    default: return 0;
  }
}

// True when the loader's integer reader would consume the whole literal, which
// is the only way to tell a number from something that merely starts with one:
// strtoll stops at the 'x' of "12x" and hands back 12 without complaint. Base 0,
// matching the loader: an 0x-prefixed guid is hex.
bool FullyParsedInteger(const std::string& text) {
  char* end = nullptr;
  std::strtoll(text.c_str(), &end, 0);
  return end != text.c_str() && *end == '\0';
}

// A literal the loader's own reader refuses. Its float reader is strtof per
// whitespace token, for a scalar prop and a vector lane alike, and it takes only
// a token it consumes whole and whose value is finite. Running out of tokens is
// the legal short form ("Shape.size = 9 0" for a plane) and not a finding.
//
// A strict load stops at the first of these, so a report that walked the loaded
// world instead of the text would see none of them: leniently the lane and every
// lane after it read 0, and nothing downstream can tell that from an authored
// zero. The text is the only place they stay visible.
void CheckNumberLiterals(ecs::World& world, const Source& source, Report& report) {
  for (const SourceAssign& assign : source.assigns) {
    const edit::ComponentDesc* comp = edit::FindComponentByName(assign.comp);
    if (!comp) continue;  // the load finding already named it
    const edit::PropDesc* prop = nullptr;
    for (u32 i = 0; i < comp->prop_count && !prop; ++i) {
      if (assign.prop == comp->props[i].name) prop = &comp->props[i];
    }
    if (!prop) continue;
    const ecs::Entity entity{assign.entity_index, 0};
    if (!world.IsAlive(entity)) continue;

    if (prop->type == edit::PropType::kI32 || prop->type == edit::PropType::kU32 ||
        prop->type == edit::PropType::kU64) {
      if (!FullyParsedInteger(assign.raw)) {
        report.ErrorAt(entity, assign.line, "unparsed_number",
                       std::format("{}.{} = {} is not a number; the loader keeps whatever prefix "
                                   "parsed and discards the rest",
                                   comp->name, prop->name, assign.raw));
      }
      continue;
    }
    const u32 lanes = FloatLanes(prop->type);
    if (lanes == 0) continue;
    std::istringstream in{assign.raw};
    std::string token;
    for (u32 lane = 0; lane < lanes && (in >> token); ++lane) {
      char* end = nullptr;
      // strtof, not strtod: 1e40 is a perfectly good double and an inf f32, and
      // it is the f32 the loader keeps.
      const f32 value = std::strtof(token.c_str(), &end);
      if (end != token.c_str() && *end == '\0' && std::isfinite(value)) continue;
      const bool number = end != token.c_str() && *end == '\0';
      report.ErrorAt(entity, assign.line, number ? "non_finite" : "unparsed_number",
                     std::format("{}.{} = {}: '{}' {}; a strict load refuses the file and a "
                                 "lenient one reads 0 from here on",
                                 comp->name, prop->name, assign.raw, token,
                                 number ? "is not finite (nan, inf, or past the f32 range)"
                                        : "is not a number"));
      break;  // one finding per assignment, not one per lane after the bad one
    }
  }
}

void CheckTransform(ecs::World& world, ecs::Entity entity, Report& report) {
  const scene::Transform* transform = world.Get<scene::Transform>(entity);
  if (!transform) return;
  if (transform->scale <= 0.0f) {
    report.Error(entity, "degenerate_scale",
                 std::format("Transform.scale is {}; the mesh collapses to a point (negative "
                             "also turns it inside out)", transform->scale));
  }
  // MakeFromQuat is the raw quaternion-to-matrix form with no normalize, so the
  // length of the authored quaternion scales the object on top of
  // Transform.scale. All zeros - what "rotation = 0 0 0" parses to, since
  // missing components pad with zero rather than with an identity w - yields a
  // zero 3x3 and the entity disappears. A strict load refuses both; this fires
  // on the lenient reload below, which is what keeps the explanation available
  // once the load has already said no.
  const f32* r = transform->rotation;
  const f32 length_sq = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
  if (length_sq < 1e-8f) {
    report.Error(entity, "degenerate_rotation",
                 "Transform.rotation is the zero quaternion; the mesh collapses to a point "
                 "(identity is 0 0 0 1)");
  } else if (std::abs(std::sqrt(length_sq) - 1.0f) > 0.05f) {
    // 5% is far outside anything hand-rounding a unit quaternion produces
    // (0.7 0 0 0.7 is only 1% short) and well inside a visible mis-scale.
    report.Warn(entity, "non_unit_rotation",
                std::format("Transform.rotation has length {}, so it scales the mesh by that "
                            "on top of Transform.scale", std::sqrt(length_sq)));
  }
}

void CheckShape(ecs::World& world, ecs::Entity entity, Report& report) {
  const SceneShape* shape = world.Get<SceneShape>(entity);
  if (!shape) return;
  const u32 required = ShapeRequiredSizeAxes(shape->kind);
  if (required == 0) {
    report.Error(entity, "unknown_shape_kind",
                 "unknown Shape.kind '" + shape->kind + "' (--dump-schema lists the kinds)");
    return;  // without a kind there is no telling which size axes matter
  }
  // Only the axes this kind reads: a plane's y and a cylinder's z are idiomatic
  // zeros (see the scenes checked in), and condemning them would make the whole
  // report noise.
  for (u32 axis = 0; axis < 3; ++axis) {
    if ((required & (1u << axis)) == 0) continue;
    if (shape->size[axis] > 0.0f) continue;
    report.Error(entity, "degenerate_shape_size",
                 std::format("Shape.size {} is {} for kind '{}', which needs it positive "
                             "(--dump-schema documents the axes per kind)",
                             "xyz"[axis], shape->size[axis], shape->kind));
  }
}

void CheckSurface(ecs::World& world, ecs::Entity entity, Report& report) {
  const SceneSurface* surface = world.Get<SceneSurface>(entity);
  if (!surface) return;
  if (!world.Has<SceneShape>(entity)) {
    report.Warn(entity, "surface_without_shape",
                "Surface on an entity with no Shape; nothing consumes it");
  }
  if (surface->materialx.empty()) return;
  // Exactly the call BuildSceneShapes makes, so a document that validates here
  // is one that loads there. Paths are relative to the working directory, which
  // makes this the one check whose answer depends on where it is run from.
  asset::Material discarded;
  if (!asset::LoadMaterialX(surface->materialx, &discarded)) {
    report.Error(entity, "materialx_not_loaded",
                 "Surface.materialx '" + surface->materialx +
                     "' does not load (path is relative to the working directory)");
  }
}

void CheckPattern(ecs::World& world, ecs::Entity entity, Report& report) {
  const ScenePattern* pattern = world.Get<ScenePattern>(entity);
  if (!pattern) return;
  if (!world.Has<SceneShape>(entity)) {
    report.Warn(entity, "pattern_without_shape",
                "Pattern on an entity with no Shape; nothing consumes it");
  }
  asset::PatternKind kind;
  if (!asset::ParsePatternKind(pattern->kind, &kind)) {
    report.Error(entity, "unknown_pattern_kind",
                 "unknown Pattern.kind '" + pattern->kind + "' (--dump-schema lists the kinds)");
  }
}

void CheckModel(ecs::World& world, ecs::Entity entity, Report& report) {
  const SceneModel* model = world.Get<SceneModel>(entity);
  if (!model) return;
  // Exactly the resolution BuildSceneModels performs, so a reference that
  // validates here is one that places geometry there. It imports the file,
  // which makes this (with Surface.materialx) one of the two checks whose
  // answer depends on where the tool is run from.
  const std::string problem = SceneModelProblem(model->path);
  if (!problem.empty()) {
    report.Error(entity, "unresolved_mesh",
                 std::format("Model.path '{}' {}", model->path, problem));
  }
}

void CheckLight(ecs::World& world, ecs::Entity entity, Report& report) {
  const SceneLight* light = world.Get<SceneLight>(entity);
  if (!light) return;
  // The viewer collects lights with Each<Light, Transform>: no Transform, no
  // light, silently and with no position to fall back on.
  if (!world.Has<scene::Transform>(entity)) {
    report.Error(entity, "light_without_transform",
                 "Light without a Transform; the viewer collects Each<Light, Transform>, so "
                 "this light is dropped");
  }
  if (light->intensity <= 0.0f) {
    report.Error(entity, "light_cannot_contribute",
                 std::format("Light.intensity is {}; the light contributes nothing",
                             light->intensity));
  }
  if (light->radius <= 0.0f) {
    report.Error(entity, "light_cannot_contribute",
                 std::format("Light.radius is {}; the influence cutoff excludes every point",
                             light->radius));
  }
}

void CheckCamera(ecs::World& world, ecs::Entity entity, Report& report) {
  const SceneCamera* camera = world.Get<SceneCamera>(entity);
  if (!camera) return;
  const scene::Transform* transform = world.Get<scene::Transform>(entity);
  if (!transform) {
    report.Error(entity, "camera_without_transform",
                 "Camera without a Transform; the viewer applies Each<Camera, Transform>, so "
                 "this viewpoint is dropped for the default one");
  } else {
    const f32* eye = transform->position;
    if (eye[0] == camera->target[0] && eye[1] == camera->target[1] &&
        eye[2] == camera->target[2]) {
      report.Error(entity, "degenerate_camera",
                   "Camera.target equals Transform.position; the view direction is the zero "
                   "vector and the view matrix is undefined");
    }
  }
  if (camera->fov_degrees <= 0.0f || camera->fov_degrees >= 180.0f) {
    report.Error(entity, "degenerate_camera_fov",
                 std::format("Camera.fov_degrees is {}; a projection needs it in (0, 180)",
                             camera->fov_degrees));
  }
}

void CheckRenderable(ecs::World& world, asset::AssetDatabase& db, ecs::Entity entity,
                     Report& report) {
  const scene::Renderable* renderable = world.Get<scene::Renderable>(entity);
  if (!renderable) return;
  // BuildSceneShapes and BuildSceneModels write both the Renderable and a
  // default Transform for a Shape/Model entity, so it is only a hand-written
  // Renderable that can be missing either. That is the general case this check
  // is here for.
  if (world.Has<SceneShape>(entity) || world.Has<SceneModel>(entity)) return;

  if (!world.Has<scene::Transform>(entity)) {
    report.Error(entity, "renderable_without_transform",
                 "Renderable without a Transform; the frame walk is Each<Transform, "
                 "Renderable>, so this entity uploads and then never draws");
  }
  if (renderable->mesh.hash == 0) {
    report.Error(entity, "unresolved_mesh", "Renderable.mesh is empty; there is nothing to draw");
    return;
  }
  if (db.FindMesh(renderable->mesh)) return;
  // LoadScene resolves a Renderable path through AssetDatabase::LoadMesh, which
  // needs a mesh converter for the extension, and the engine registers none. A
  // text scene reaches real geometry through Model instead, which imports the
  // file itself and hands the meshes to the database; a hand-written
  // Renderable path still resolves to nothing.
  const std::optional<std::string> path = asset::LookupAssetPath(renderable->mesh);
  report.Error(entity, "unresolved_mesh",
               std::format("Renderable.mesh '{}' resolves to no uploaded mesh; a .rxscene has "
                           "no mesh converters, so name the file from a Model (or author a "
                           "Shape) instead",
                           path ? *path : std::format("hash:0x{:016x}", renderable->mesh.hash)));
}

void CheckParent(ecs::World& world, ecs::Entity entity, Report& report) {
  const scene::Parent* parent = world.Get<scene::Parent>(entity);
  if (!parent) return;
  if (!parent->value || !world.IsAlive(parent->value)) {
    report.Error(entity, "dangling_parent",
                 "Parent.value resolves to no entity; the guid names nothing in this file and "
                 "the child silently keeps its local transform as world space");
    return;
  }
  // Host::GatherEntityDraws walks the Parent chain with a 4096 iteration guard,
  // so a cycle does not hang, it just composes a meaningless matrix. Cheap to
  // catch here and impossible to diagnose from the render.
  auto step = [&](ecs::Entity from, ecs::Entity* to) {
    const scene::Parent* link = world.Get<scene::Parent>(from);
    if (!link || !link->value || !world.IsAlive(link->value)) return false;
    *to = link->value;
    return true;
  };
  ecs::Entity slow = entity;
  ecs::Entity fast = entity;
  while (step(fast, &fast) && step(fast, &fast) && step(slow, &slow)) {
    if (fast != slow) continue;
    report.Error(entity, "parent_cycle",
                 "Parent chain from this entity is a cycle; the frame walk composes 4096 "
                 "matrices and gives up on a meaningless one");
    return;
  }
}

void CheckDuplicateGuids(ecs::World& world, const std::vector<ecs::Entity>& entities,
                         Report& report) {
  // A guid is how a scene names an entity across a save/load, so two entities
  // sharing one make every reference to it resolve to whichever the loader
  // mapped last, and make a re-save drop the other.
  std::unordered_map<u64, ecs::Entity> by_guid;
  for (ecs::Entity entity : entities) {
    const scene::Guid* guid = world.Get<scene::Guid>(entity);
    if (!guid || guid->value == 0) continue;
    auto [it, inserted] = by_guid.emplace(guid->value, entity);
    if (inserted) continue;
    report.Error(entity, "duplicate_guid",
                 std::format("Guid.value 0x{:016x} is already used by entity index {}; "
                             "references to it resolve to only one of the two",
                             guid->value, it->second.index));
  }
}

void PrintJsonString(std::string_view s) {
  std::putchar('"');
  for (char c : s) {
    if (c == '"' || c == '\\') std::putchar('\\');
    std::putchar(c);
  }
  std::putchar('"');
}

void PrintJson(const std::string& path, const Report& report) {
  std::printf("{\n  \"scene\": ");
  PrintJsonString(path);
  std::printf(",\n  \"errors\": %u,\n  \"warnings\": %u,\n  \"findings\": [", report.errors(),
              report.warnings());
  const std::vector<Finding>& findings = report.findings();
  for (size_t i = 0; i < findings.size(); ++i) {
    const Finding& finding = findings[i];
    std::printf("%s\n    {\"severity\": \"%s\", \"check\": ", i ? "," : "",
                finding.error ? "error" : "warning");
    PrintJsonString(finding.check);
    std::printf(", \"line\": %d, \"entity\": ", finding.line);
    PrintJsonString(finding.entity);
    std::printf(", \"message\": ");
    PrintJsonString(finding.message);
    std::printf("}");
  }
  std::printf("%s]\n}\n", findings.empty() ? "" : "\n  ");
}

// Compiler-style, so an editor and a grep both find the offending line.
void PrintHuman(const std::string& path, const Report& report) {
  for (const Finding& finding : report.findings()) {
    std::printf("%s:", path.c_str());
    if (finding.line) std::printf("%d:", finding.line);
    std::printf(" %s: %s: ", finding.error ? "error" : "warning", finding.check);
    if (!finding.entity.empty()) std::printf("'%s': ", finding.entity.c_str());
    std::printf("%s\n", finding.message.c_str());
  }
  std::printf("%s: %u error(s), %u warning(s)\n", path.c_str(), report.errors(),
              report.warnings());
}

}  // namespace

bool ValidateSceneFile(const std::string& path, bool json) {
  // RX_INFO writes to stdout, which the json report has to have to itself.
  if (json) SetLogLevel(LogLevel::kWarn);

  RegisterSceneComponents();
  ecs::World world;
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  const Source source = ScanSource(path);

  // Strict first, exactly like the viewer: whatever fails the load there has to
  // be an error here too rather than a finding this tool invents its own
  // severity for. A strict load stops at the first bad line and creates
  // nothing, though, and a report that stopped there would cost the author the
  // other twenty findings, so fall back to a lenient load of the same file: the
  // gate says no, and this still explains the whole document.
  std::string error;
  const bool strict_loaded = edit::LoadScene(world, db, path, &error, /*strict=*/true);
  if (!strict_loaded) edit::LoadScene(world, db, path, nullptr, /*strict=*/false);

  Report report(world, source.entity_lines);
  if (!strict_loaded) report.FileError("load", error);

  // The two passes that change what an entity IS have to run before the checks
  // below, or every prefab instance is judged on the half of itself the file
  // wrote: a cell that takes its Shape from a prefab would be condemned as a
  // Surface with nothing to shade. Both are the viewer's own passes rather than
  // a structural mirror of them, so a layout this accepts is one that loads.
  // Failures land as file-wide findings for the same reason `load` does: the
  // message already carries its own `path:line:`, and the entity it names may
  // be one the expansion created, which has no line at all.
  if (!BuildSceneGrids(world, path, &error)) report.FileError("grid", error);
  if (!BuildScenePrefabs(world, path, &error)) report.FileError("prefab", error);

  const std::vector<ecs::Entity> entities = AllEntities(world);
  u32 cameras = 0;
  u32 drawables = 0;
  for (ecs::Entity entity : entities) {
    CheckTransform(world, entity, report);
    CheckShape(world, entity, report);
    CheckSurface(world, entity, report);
    CheckPattern(world, entity, report);
    CheckModel(world, entity, report);
    CheckLight(world, entity, report);
    CheckCamera(world, entity, report);
    CheckRenderable(world, db, entity, report);
    CheckParent(world, entity, report);
    cameras += world.Has<SceneCamera>(entity) ? 1 : 0;
    drawables += world.Has<SceneShape>(entity) || world.Has<SceneModel>(entity) ||
                         world.Has<scene::Renderable>(entity)
                     ? 1
                     : 0;
  }
  CheckDuplicateGuids(world, entities, report);
  CheckNumberLiterals(world, source, report);

  // Anchors are measured from built geometry, so the meshes have to exist
  // before they resolve. Built here, after the per-entity checks, so a scene
  // the builder refuses still gets the full report explaining why (its own
  // finding already named the bad Shape.kind or Model.path) instead of one
  // file-wide failure. No gpu: `renderer` null is the headless path the
  // viewer's own --headless takes.
  if (BuildSceneShapes(world, db, /*renderer=*/nullptr, &error) &&
      BuildSceneModels(world, db, /*renderer=*/nullptr, path, &error)) {
    if (!BuildSceneAnchors(world, path, &error)) report.FileError("anchor", error);
  }

  // Suppressed when nothing loaded at all (a bad header, an unreadable file):
  // there the load error is the finding and "no geometry" on top of it is noise.
  if (drawables == 0 && (strict_loaded || !entities.empty())) {
    report.FileWarn("no_geometry", "no Shape, Model or Renderable in the scene; the render is "
                                   "the empty sky");
  }
  if (cameras > 1) {
    // The viewer takes the first Camera its Each walk reaches, and that walk is
    // in archetype order, not file order, so which one wins is not something
    // the file decides.
    report.FileWarn("multiple_cameras",
                    std::format("{} Camera components; which one the viewer takes is "
                                "archetype order, not file order", cameras));
  }

  report.SortByLine();
  if (json) {
    PrintJson(path, report);
  } else {
    PrintHuman(path, report);
  }
  return report.errors() == 0;
}

}  // namespace rx
