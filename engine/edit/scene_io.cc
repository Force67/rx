#include "edit/scene_io.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "asset/asset_id.h"
#include <format>
#include "core/log.h"
#include "edit/hierarchy.h"
#include "edit/reflect.h"
#include "scene/components.h"

namespace rx::edit {
namespace {

constexpr int kSceneVersion = 1;

std::string Trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(a, b - a + 1));
}

std::string QuoteString(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += '"';
  return out;
}

std::string Unquote(std::string_view raw) {
  std::string s = Trim(raw);
  std::string_view sv = s;
  if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"') sv = sv.substr(1, sv.size() - 2);
  std::string out;
  for (size_t i = 0; i < sv.size(); ++i) {
    if (sv[i] == '\\' && i + 1 < sv.size()) {
      char n = sv[++i];
      switch (n) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        default: out += n;
      }
    } else {
      out += sv[i];
    }
  }
  return out;
}

std::string FloatStr(f32 v) { return std::format("{}", v); }

std::vector<f32> ParseFloats(std::string_view s, size_t n) {
  std::vector<f32> out;
  std::istringstream in{std::string(s)};
  f32 v;
  while (out.size() < n && (in >> v)) out.push_back(v);
  while (out.size() < n) out.push_back(0.f);
  return out;
}

u64 ParseHexOrDec(std::string_view s) {
  std::string t = Trim(s);
  return static_cast<u64>(std::strtoull(t.c_str(), nullptr, 0));  // base 0: 0x -> hex
}

// Renders one field of a component to a literal. Entity/AssetId need world/db
// context, handled by the caller before falling through here.
std::string FormatValue(const PropValue& v) {
  switch (v.type) {
    case PropType::kBool: return v.b ? "true" : "false";
    case PropType::kI32: return std::format("{}", static_cast<i32>(v.i));
    case PropType::kU32: return std::format("{}", static_cast<u32>(v.u));
    case PropType::kU64: return std::format("0x{:016x}", v.u);
    case PropType::kF32: return FloatStr(v.f[0]);
    case PropType::kVec2: return FloatStr(v.f[0]) + " " + FloatStr(v.f[1]);
    case PropType::kVec3:
      return FloatStr(v.f[0]) + " " + FloatStr(v.f[1]) + " " + FloatStr(v.f[2]);
    case PropType::kVec4:
    case PropType::kQuat:
    case PropType::kColor:
      return FloatStr(v.f[0]) + " " + FloatStr(v.f[1]) + " " + FloatStr(v.f[2]) + " " +
             FloatStr(v.f[3]);
    case PropType::kString: return QuoteString(v.s);
    case PropType::kAssetId: {
      if (v.u == 0) return "\"\"";
      if (auto path = asset::LookupAssetPath(asset::AssetId{v.u})) return QuoteString(*path);
      return std::format("hash:0x{:016x}", v.u);
    }
    case PropType::kEntity: return "null";  // resolved by caller
  }
  return {};
}

u64 PackKey(ecs::Entity e) { return static_cast<u64>(e.generation) << 32 | e.index; }

}  // namespace

bool SaveScene(ecs::World& world, const std::string& file_path, std::string* error) {
  // Union of identity-bearing entities (Guid, Name or Transform).
  std::unordered_set<u64> seen;
  std::vector<ecs::Entity> entities;
  auto collect = [&](ecs::Entity e) {
    if (seen.insert(PackKey(e)).second) entities.push_back(e);
  };
  world.Each<scene::Guid>([&](ecs::Entity e, scene::Guid&) { collect(e); });
  world.Each<scene::Name>([&](ecs::Entity e, scene::Name&) { collect(e); });
  world.Each<scene::Transform>([&](ecs::Entity e, scene::Transform&) { collect(e); });

  // Every saved entity needs a guid so references resolve on reload; sort by it
  // for a stable, diff-friendly ordering.
  for (ecs::Entity e : entities) EnsureGuid(world, e);
  std::sort(entities.begin(), entities.end(), [&](ecs::Entity a, ecs::Entity b) {
    return world.Get<scene::Guid>(a)->value < world.Get<scene::Guid>(b)->value;
  });

  std::ofstream out(file_path, std::ios::binary);
  if (!out) {
    if (error) *error = "cannot open '" + file_path + "' for writing";
    return false;
  }
  out << "rxscene " << kSceneVersion << "\n";

  for (ecs::Entity e : entities) {
    out << "\nentity\n";
    for (const ComponentDesc* comp : ComponentsOn(world, e)) {
      if (comp->prop_count == 0) {
        out << comp->name << "\n";  // tag component: presence is the state
        continue;
      }
      for (u32 i = 0; i < comp->prop_count; ++i) {
        const PropDesc& prop = comp->props[i];
        PropValue value;
        if (!GetProp(world, e, *comp, prop, &value)) continue;
        std::string literal;
        if (prop.type == PropType::kEntity) {
          if (value.e && world.IsAlive(value.e)) {
            u64 g = EnsureGuid(world, value.e);
            literal = std::format("guid:0x{:016x}", g);
          } else {
            literal = "null";
          }
        } else {
          literal = FormatValue(value);
        }
        out << comp->name << "." << prop.name << " = " << literal << "\n";
      }
    }
  }
  return static_cast<bool>(out);
}

namespace {

struct Assign {
  std::string comp;
  std::string prop;
  std::string raw;
};

struct ParsedEntity {
  u64 guid = 0;
  std::vector<Assign> assigns;
  std::vector<std::string> bare_comps;
};

}  // namespace

bool LoadScene(ecs::World& world, asset::AssetDatabase& db, const std::string& file_path,
               std::string* error) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open '" + file_path + "' for reading";
    return false;
  }

  std::string line;
  if (!std::getline(in, line)) {
    if (error) *error = "empty scene file";
    return false;
  }
  {
    std::istringstream header{Trim(line)};
    std::string magic;
    int version = 0;
    header >> magic >> version;
    if (magic != "rxscene") {
      if (error) *error = "not an rxscene file";
      return false;
    }
    if (version > kSceneVersion) {
      if (error) *error = std::format("scene version {} newer than supported {}", version,
                                     kSceneVersion);
      return false;
    }
  }

  std::vector<ParsedEntity> parsed;
  ParsedEntity* current = nullptr;
  while (std::getline(in, line)) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t == "entity") {
      parsed.emplace_back();
      current = &parsed.back();
      continue;
    }
    if (!current) continue;  // stray line before first entity

    size_t eq = t.find('=');
    if (eq == std::string::npos) {
      current->bare_comps.push_back(t);  // tag component
      continue;
    }
    std::string key = Trim(t.substr(0, eq));
    std::string raw = Trim(t.substr(eq + 1));
    size_t dot = key.find('.');
    if (dot == std::string::npos) continue;  // malformed key
    Assign a{key.substr(0, dot), key.substr(dot + 1), raw};
    if (a.comp == "Guid" && a.prop == "value") current->guid = ParseHexOrDec(raw);
    current->assigns.push_back(std::move(a));
  }

  // Pass 1: create entities and map guids.
  std::vector<ecs::Entity> created;
  created.reserve(parsed.size());
  std::unordered_map<u64, ecs::Entity> by_guid;
  for (ParsedEntity& pe : parsed) {
    ecs::Entity e = world.Create();
    created.push_back(e);
    if (pe.guid != 0) by_guid.emplace(pe.guid, e);
  }

  // Pass 2: materialize components and resolve references.
  for (size_t i = 0; i < parsed.size(); ++i) {
    ecs::Entity e = created[i];
    const ParsedEntity& pe = parsed[i];

    for (const std::string& name : pe.bare_comps) {
      const ComponentDesc* comp = FindComponentByName(name);
      if (!comp) {
        RX_WARN("rxscene: unknown component '{}', skipped", name);
        continue;
      }
      AddComponentByDesc(world, e, *comp);
    }

    for (const Assign& a : pe.assigns) {
      const ComponentDesc* comp = FindComponentByName(a.comp);
      if (!comp) {
        RX_WARN("rxscene: unknown component '{}', skipped", a.comp);
        continue;
      }
      if (!world.HasRaw(e, comp->id)) AddComponentByDesc(world, e, *comp);

      const PropDesc* prop = nullptr;
      for (u32 p = 0; p < comp->prop_count; ++p) {
        if (a.prop == comp->props[p].name) { prop = &comp->props[p]; break; }
      }
      if (!prop) {
        RX_WARN("rxscene: unknown prop '{}.{}', skipped", a.comp, a.prop);
        continue;
      }

      PropValue value;
      value.type = prop->type;
      switch (prop->type) {
        case PropType::kBool: value = PropValue::Bool(a.raw == "true"); break;
        case PropType::kI32: value = PropValue::I32(static_cast<i32>(std::strtol(a.raw.c_str(), nullptr, 0))); break;
        case PropType::kU32: value = PropValue::U32(static_cast<u32>(ParseHexOrDec(a.raw))); break;
        case PropType::kU64: value = PropValue::U64(ParseHexOrDec(a.raw)); break;
        case PropType::kF32: value = PropValue::F32(std::strtof(a.raw.c_str(), nullptr)); break;
        case PropType::kVec2: {
          auto v = ParseFloats(a.raw, 2);
          value = PropValue::Vec2(v[0], v[1]);
          break;
        }
        case PropType::kVec3: {
          auto v = ParseFloats(a.raw, 3);
          value = PropValue::Vec3(v[0], v[1], v[2]);
          break;
        }
        case PropType::kVec4: {
          auto v = ParseFloats(a.raw, 4);
          value = PropValue::Vec4(v[0], v[1], v[2], v[3]);
          break;
        }
        case PropType::kQuat: {
          auto v = ParseFloats(a.raw, 4);
          value = PropValue::Quat(v[0], v[1], v[2], v[3]);
          break;
        }
        case PropType::kColor: {
          auto v = ParseFloats(a.raw, 4);
          value = PropValue::Color(v[0], v[1], v[2], v[3]);
          break;
        }
        case PropType::kString: value = PropValue::String(Unquote(a.raw)); break;
        case PropType::kAssetId: {
          if (a.raw.rfind("hash:", 0) == 0) {
            value = PropValue::AssetIdV(ParseHexOrDec(std::string_view(a.raw).substr(5)));
          } else {
            std::string path = asset::NormalizePath(Unquote(a.raw));
            asset::AssetId id = asset::MakeAssetId(path);
            if (!path.empty()) {
              asset::RecordAssetPath(id, path);
              if (db.vfs().Contains(path)) db.LoadMesh(path);  // resolve through the db
            }
            value = PropValue::AssetIdV(id.hash);
          }
          break;
        }
        case PropType::kEntity: {
          ecs::Entity ref = ecs::kInvalidEntity;
          if (a.raw.rfind("guid:", 0) == 0) {
            u64 g = ParseHexOrDec(std::string_view(a.raw).substr(5));
            auto it = by_guid.find(g);
            if (it != by_guid.end()) ref = it->second;
          }
          value = PropValue::EntityV(ref);
          break;
        }
      }
      SetProp(world, e, *comp, *prop, value);
    }
  }
  return true;
}

}  // namespace rx::edit
