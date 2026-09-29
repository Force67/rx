#include "material_palette.h"

#include <stdio.h>

#include "base/algorithm.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/files/file_system.h"
#include "foundation/logging/log.h"
#include "foundation/strings/text_reader.h"
#include "rxe/asset/asset_database.h"
#include "rxe/asset/vfs.h"
#include "rxe/ecs/world.h"
#include "rxe/scene/reflect.h"
#include "rxe/scene/scene_io.h"
#include "scene_authoring.h"

namespace rx::shell {
namespace {

// One loaded preset. The database owns the assets the loader resolves against
// and has to outlive the world that points at them, which is why this is a
// struct and not three locals.
struct Preset {
  asset::Vfs vfs;
  asset::AssetDatabase db{vfs};
  ecs::World world;
};

void PrintJsonString(base::StringRef s) {
  ::putchar('"');
  for (char c : s) {
    if (c == '"' || c == '\\') ::putchar('\\');
    ::putchar(c);
  }
  ::putchar('"');
}

// The file's first comment paragraph, joined into one line: the lines from the
// first '#' up to the first line that is not one. That is the block every
// preset opens with, and it stops before the blank line ahead of `entity`, so
// the summary is what the author wrote about the material and not the whole
// header of a file that happens to carry more.
base::String LeadingComment(const base::String& path) {
  base::String contents;
  fs::ReadTextFile(path, &contents);
  LineReader in(contents);
  base::StringRef line;
  base::String summary;
  bool started = false;
  while (in.Next(&line)) {
    const size_t a = line.find_first_not_of(" \t\r\n");
    const bool comment = a != base::StringRef::npos && line[a] == '#';
    if (!comment) {
      if (started) break;
      continue;
    }
    started = true;
    size_t text = line.find_first_not_of("# \t", a);
    if (text == base::StringRef::npos) continue;  // a '#' on its own separates paragraphs
    const size_t end = line.find_last_not_of(" \t\r\n");
    if (!summary.empty()) summary += ' ';
    summary.append(line.data() + text, end - text + 1);
  }
  return summary;
}

// A prop as the loader left it, next to the same prop on a default-constructed
// component, so the dump can print what the preset SAYS rather than all 22
// fields of a Surface. Only the props a value was written to carry information;
// the rest are the defaults --dump-schema already documents.
bool SameAsDefault(const scene::PropValue& value, const scene::PropValue& fallback) {
  switch (value.type) {
    case scene::PropType::kBool: return value.b == fallback.b;
    case scene::PropType::kI32: return value.i == fallback.i;
    case scene::PropType::kU32:
    case scene::PropType::kU64:
    case scene::PropType::kAssetId: return value.u == fallback.u;
    case scene::PropType::kString: return value.s == fallback.s;
    case scene::PropType::kEntity: return value.e == fallback.e;
    default: break;
  }
  for (u32 lane = 0; lane < 4; ++lane) {
    if (value.f[lane] != fallback.f[lane]) return false;
  }
  return true;
}

// Lanes a type occupies in PropValue::f, 0 for the ones carrying no float.
u32 FloatLanes(scene::PropType type) {
  switch (type) {
    case scene::PropType::kF32: return 1;
    case scene::PropType::kVec2: return 2;
    case scene::PropType::kVec3: return 3;
    case scene::PropType::kVec4:
    case scene::PropType::kQuat:
    case scene::PropType::kColor: return 4;
    default: return 0;
  }
}

void PrintValue(const scene::PropValue& value) {
  if (value.type == scene::PropType::kString) {
    PrintJsonString(value.s);
    return;
  }
  if (value.type == scene::PropType::kBool) {
    ::printf("%s", value.b ? "true" : "false");
    return;
  }
  if (const u32 lanes = FloatLanes(value.type); lanes != 0) {
    if (lanes == 1) {
      ::printf("%g", value.f[0]);
      return;
    }
    ::printf("[");
    for (u32 lane = 0; lane < lanes; ++lane) ::printf("%s%g", lane ? ", " : "", value.f[lane]);
    ::printf("]");
    return;
  }
  // i32 is the one integer the reader signs, and it lives in a different field.
  if (value.type == scene::PropType::kI32) {
    ::printf("%lld", static_cast<long long>(value.i));
    return;
  }
  ::printf("%llu", static_cast<unsigned long long>(value.u));
}

// One preset's components, as `{"Surface": {"roughness": 0.35, ...}, ...}`.
// `defaults` is a scratch entity of the same world, borrowed one component at a
// time to read what an unset prop would have been.
void PrintComponents(ecs::World& world, ecs::Entity entity, ecs::Entity defaults) {
  ::printf("{");
  bool first_comp = true;
  for (const scene::ComponentDesc* comp : scene::ComponentsOn(world, entity)) {
    // A component the registry cannot default-construct has no "unset" to
    // compare against, so every prop of it is printed rather than the component
    // being dropped: a listing that silently omits what it cannot summarize is
    // worse than a verbose one.
    const bool has_defaults = scene::AddComponentByDesc(world, defaults, *comp);
    bool first_prop = true;
    ::printf("%s\n        ", first_comp ? "" : ",");
    PrintJsonString(comp->name);
    ::printf(": {");
    for (u32 p = 0; p < comp->prop_count; ++p) {
      const scene::PropDesc& prop = comp->props[p];
      scene::PropValue value;
      scene::PropValue fallback;
      if (!scene::GetProp(world, entity, *comp, prop, &value)) continue;
      const bool compare = has_defaults && scene::GetProp(world, defaults, *comp, prop, &fallback);
      // A name is never noise, so a string prop prints whenever it has one:
      // Pattern.kind = "checker" IS the pattern even though it is also the
      // default, and a listing that dropped it would read as no kind at all.
      const bool named = prop.type == scene::PropType::kString && !value.s.empty();
      if (!named && compare && SameAsDefault(value, fallback)) continue;
      ::printf("%s", first_prop ? "" : ", ");
      first_prop = false;
      PrintJsonString(prop.name);
      ::printf(": ");
      PrintValue(value);
    }
    ::printf("}");
    first_comp = false;
    if (has_defaults) scene::RemoveComponentByDesc(world, defaults, *comp);
  }
  ::printf("%s}", first_comp ? "" : "\n      ");
}

}  // namespace

bool DumpMaterialPalette(const base::String& dir) {
  if (!fs::IsDirectory(dir)) {
    RX_ERROR("no material palette at '{}' (the path is relative to the working directory)", dir);
    return false;
  }
  base::Vector<base::String> files;
  base::Vector<fs::DirEntry> entries;
  fs::ListDirectory(dir, &entries);
  for (const fs::DirEntry& entry : entries) {
    if (entry.is_regular && fs::Extension(entry.path) == ".rxscene") {
      files.push_back(entry.path);
    }
  }
  if (files.empty()) {
    RX_ERROR("no .rxscene presets in '{}'", dir);
    return false;
  }
  // Directory order is whatever the filesystem hands back; sorting is what
  // makes two dumps of the same palette diffable.
  // One directory, so the paths differ only in their unique file names and a
  // byte-wise sort orders them as path comparison did.
  base::Sort(files.data(), files.data() + files.size());

  // Every preset is loaded before anything is printed, so a palette with a bad
  // entry in it fails with no output rather than with half a json document a
  // caller then has to parse to find out it is half.
  RegisterSceneComponents();
  base::Vector<base::UniquePointer<Preset>> presets;
  for (const base::String& file : files) {
    // One world per preset: the first entity of the file is the preset (the
    // same INVARIANT Prefab.path merges by - World::Create hands out ascending
    // indices and LoadScene calls it once per `entity` block), and a shared
    // world would make "first" mean the first entity of the first file.
    auto preset = base::MakeUnique<Preset>();
    base::String error;
    if (!scene::LoadScene(preset->world, preset->db, file, &error, /*strict=*/true)) {
      RX_ERROR("material preset '{}' does not load: {}", file, error);
      return false;
    }
    if (!preset->world.IsAlive(ecs::Entity{0, 0})) {
      RX_ERROR("material preset '{}' declares no entity", file);
      return false;
    }
    presets.push_back(base::move(preset));
  }

  ::printf("{\n  \"directory\": ");
  PrintJsonString(dir);
  ::printf(",\n  \"materials\": [\n");
  for (size_t i = 0; i < files.size(); ++i) {
    ecs::World& world = presets[i]->world;
    ::printf("    {\"name\": ");
    PrintJsonString(base::String(fs::Stem(files[i])));
    ::printf(", \"path\": ");
    PrintJsonString(fs::GenericString(files[i]));
    ::printf(", \"summary\": ");
    PrintJsonString(LeadingComment(files[i]));
    ::printf(", \"sets\": ");
    // A scratch entity to default-construct each component onto, which is where
    // "the author did not set this" comes from: the registry knows the
    // defaults, so nothing here has to repeat them.
    PrintComponents(world, ecs::Entity{0, 0}, world.Create());
    ::printf("}%s\n", i + 1 < files.size() ? "," : "");
  }
  ::printf("  ]\n}\n");
  return true;
}

}  // namespace rx::shell
