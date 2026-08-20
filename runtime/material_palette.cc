#include "material_palette.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "core/log.h"
#include "ecs/world.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "scene_authoring.h"

namespace rx {
namespace {

// One loaded preset. The database owns the assets the loader resolves against
// and has to outlive the world that points at them, which is why this is a
// struct and not three locals.
struct Preset {
  asset::Vfs vfs;
  asset::AssetDatabase db{vfs};
  ecs::World world;
};

void PrintJsonString(std::string_view s) {
  std::putchar('"');
  for (char c : s) {
    if (c == '"' || c == '\\') std::putchar('\\');
    std::putchar(c);
  }
  std::putchar('"');
}

// The file's first comment paragraph, joined into one line: the lines from the
// first '#' up to the first line that is not one. That is the block every
// preset opens with, and it stops before the blank line ahead of `entity`, so
// the summary is what the author wrote about the material and not the whole
// header of a file that happens to carry more.
std::string LeadingComment(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  std::string summary;
  bool started = false;
  while (std::getline(in, line)) {
    const size_t a = line.find_first_not_of(" \t\r\n");
    const bool comment = a != std::string::npos && line[a] == '#';
    if (!comment) {
      if (started) break;
      continue;
    }
    started = true;
    size_t text = line.find_first_not_of("# \t", a);
    if (text == std::string::npos) continue;  // a '#' on its own separates paragraphs
    const size_t end = line.find_last_not_of(" \t\r\n");
    if (!summary.empty()) summary += ' ';
    summary.append(line, text, end - text + 1);
  }
  return summary;
}

// A prop as the loader left it, next to the same prop on a default-constructed
// component, so the dump can print what the preset SAYS rather than all 22
// fields of a Surface. Only the props a value was written to carry information;
// the rest are the defaults --dump-schema already documents.
bool SameAsDefault(const edit::PropValue& value, const edit::PropValue& fallback) {
  switch (value.type) {
    case edit::PropType::kBool: return value.b == fallback.b;
    case edit::PropType::kI32: return value.i == fallback.i;
    case edit::PropType::kU32:
    case edit::PropType::kU64:
    case edit::PropType::kAssetId: return value.u == fallback.u;
    case edit::PropType::kString: return value.s == fallback.s;
    case edit::PropType::kEntity: return value.e == fallback.e;
    default: break;
  }
  for (u32 lane = 0; lane < 4; ++lane) {
    if (value.f[lane] != fallback.f[lane]) return false;
  }
  return true;
}

// Lanes a type occupies in PropValue::f, 0 for the ones carrying no float.
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

void PrintValue(const edit::PropValue& value) {
  if (value.type == edit::PropType::kString) {
    PrintJsonString(value.s);
    return;
  }
  if (value.type == edit::PropType::kBool) {
    std::printf("%s", value.b ? "true" : "false");
    return;
  }
  if (const u32 lanes = FloatLanes(value.type); lanes != 0) {
    if (lanes == 1) {
      std::printf("%g", value.f[0]);
      return;
    }
    std::printf("[");
    for (u32 lane = 0; lane < lanes; ++lane) std::printf("%s%g", lane ? ", " : "", value.f[lane]);
    std::printf("]");
    return;
  }
  // i32 is the one integer the reader signs, and it lives in a different field.
  if (value.type == edit::PropType::kI32) {
    std::printf("%lld", static_cast<long long>(value.i));
    return;
  }
  std::printf("%llu", static_cast<unsigned long long>(value.u));
}

// One preset's components, as `{"Surface": {"roughness": 0.35, ...}, ...}`.
// `defaults` is a scratch entity of the same world, borrowed one component at a
// time to read what an unset prop would have been.
void PrintComponents(ecs::World& world, ecs::Entity entity, ecs::Entity defaults) {
  std::printf("{");
  bool first_comp = true;
  for (const edit::ComponentDesc* comp : edit::ComponentsOn(world, entity)) {
    // A component the registry cannot default-construct has no "unset" to
    // compare against, so every prop of it is printed rather than the component
    // being dropped: a listing that silently omits what it cannot summarize is
    // worse than a verbose one.
    const bool has_defaults = edit::AddComponentByDesc(world, defaults, *comp);
    bool first_prop = true;
    std::printf("%s\n        ", first_comp ? "" : ",");
    PrintJsonString(comp->name);
    std::printf(": {");
    for (u32 p = 0; p < comp->prop_count; ++p) {
      const edit::PropDesc& prop = comp->props[p];
      edit::PropValue value;
      edit::PropValue fallback;
      if (!edit::GetProp(world, entity, *comp, prop, &value)) continue;
      const bool compare = has_defaults && edit::GetProp(world, defaults, *comp, prop, &fallback);
      // A name is never noise, so a string prop prints whenever it has one:
      // Pattern.kind = "checker" IS the pattern even though it is also the
      // default, and a listing that dropped it would read as no kind at all.
      const bool named = prop.type == edit::PropType::kString && !value.s.empty();
      if (!named && compare && SameAsDefault(value, fallback)) continue;
      std::printf("%s", first_prop ? "" : ", ");
      first_prop = false;
      PrintJsonString(prop.name);
      std::printf(": ");
      PrintValue(value);
    }
    std::printf("}");
    first_comp = false;
    if (has_defaults) edit::RemoveComponentByDesc(world, defaults, *comp);
  }
  std::printf("%s}", first_comp ? "" : "\n      ");
}

}  // namespace

bool DumpMaterialPalette(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    RX_ERROR("no material palette at '{}' (the path is relative to the working directory)", dir);
    return false;
  }
  std::vector<std::filesystem::path> files;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".rxscene") {
      files.push_back(entry.path());
    }
  }
  if (files.empty()) {
    RX_ERROR("no .rxscene presets in '{}'", dir);
    return false;
  }
  // Directory order is whatever the filesystem hands back; sorting is what
  // makes two dumps of the same palette diffable.
  std::sort(files.begin(), files.end());

  // Every preset is loaded before anything is printed, so a palette with a bad
  // entry in it fails with no output rather than with half a json document a
  // caller then has to parse to find out it is half.
  RegisterSceneComponents();
  std::vector<std::unique_ptr<Preset>> presets;
  for (const std::filesystem::path& file : files) {
    // One world per preset: the first entity of the file is the preset (the
    // same INVARIANT Prefab.path merges by - World::Create hands out ascending
    // indices and LoadScene calls it once per `entity` block), and a shared
    // world would make "first" mean the first entity of the first file.
    auto preset = std::make_unique<Preset>();
    std::string error;
    if (!edit::LoadScene(preset->world, preset->db, file.string(), &error, /*strict=*/true)) {
      RX_ERROR("material preset '{}' does not load: {}", file.string(), error);
      return false;
    }
    if (!preset->world.IsAlive(ecs::Entity{0, 0})) {
      RX_ERROR("material preset '{}' declares no entity", file.string());
      return false;
    }
    presets.push_back(std::move(preset));
  }

  std::printf("{\n  \"directory\": ");
  PrintJsonString(dir);
  std::printf(",\n  \"materials\": [\n");
  for (size_t i = 0; i < files.size(); ++i) {
    ecs::World& world = presets[i]->world;
    std::printf("    {\"name\": ");
    PrintJsonString(files[i].stem().string());
    std::printf(", \"path\": ");
    PrintJsonString(files[i].generic_string());
    std::printf(", \"summary\": ");
    PrintJsonString(LeadingComment(files[i]));
    std::printf(", \"sets\": ");
    // A scratch entity to default-construct each component onto, which is where
    // "the author did not set this" comes from: the registry knows the
    // defaults, so nothing here has to repeat them.
    PrintComponents(world, ecs::Entity{0, 0}, world.Create());
    std::printf("}%s\n", i + 1 < files.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
  return true;
}

}  // namespace rx
