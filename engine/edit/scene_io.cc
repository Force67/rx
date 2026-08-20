#include "edit/scene_io.h"

#include <algorithm>
#include <cmath>
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

// How many of PropValue::f a prop type uses; 0 for the types that carry no
// float. Keeps the number handling generic over the registry instead of
// listing components by hand.
u32 FloatLanes(PropType type) {
  switch (type) {
    case PropType::kF32: return 1;
    case PropType::kVec2: return 2;
    case PropType::kVec3: return 3;
    case PropType::kVec4:
    case PropType::kQuat:
    case PropType::kColor: return 4;
    default: return 0;
  }
}

// The one float reader for the format. Null on success, else the clause saying
// why the token was refused, for a caller to put behind a `path:line:`.
//
// strtof for every float, scalar prop and vector lane alike: the lanes used to
// go through an istringstream, which rejects "nan", "inf" and anything past the
// f32 range where strtof takes them, so the same literal meant one thing in
// `scale` and another in `position`. Neither may accept a non-finite value: it
// poisons every matrix and lighting term it reaches, and FloatStr cannot write
// one back out in a form this reader would take.
const char* ReadFloat(const std::string& token, f32* out) {
  char* end = nullptr;
  const f32 v = std::strtof(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0') return "is not a number";
  if (!std::isfinite(v)) return "is not finite";
  *out = v;
  return nullptr;
}

// Reads up to `n` whitespace-separated floats, zero-padding a short list (a
// plane is authored "Shape.size = 9 0"). Stops at the first token that is not a
// finite number and reports it through `error`; the lanes from there on stay 0,
// which is what a lenient load keeps and what a strict load refuses.
std::vector<f32> ParseFloats(std::string_view s, size_t n, std::string* error = nullptr) {
  std::vector<f32> out;
  std::istringstream in{std::string(s)};
  std::string token;
  while (out.size() < n && (in >> token)) {
    f32 v = 0.f;
    if (const char* why = ReadFloat(token, &v)) {
      if (error) *error = "'" + token + "' " + why;
      break;
    }
    out.push_back(v);
  }
  out.resize(n, 0.f);
  return out;
}

// Why an authored quaternion is not usable as a rotation, or empty. MakeFromQuat
// is the raw quaternion-to-matrix form with no normalize (24 call sites, one of
// them the per-frame transform build), so the length of the authored quaternion
// scales the mesh on top of Transform.scale, and all zeros - what "rotation =
// 0 0 0" pads to, since a short list pads with 0 and not with an identity w -
// yields a zero 3x3 and the mesh vanishes. The authoring boundary is the only
// place this can be caught without putting a square root in that hot path.
std::string QuatProblem(const std::vector<f32>& v) {
  const f32 length_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
  if (length_sq < 1e-8f)
    return "the zero quaternion collapses the mesh to a point (identity is 0 0 0 1)";
  // 5% is far outside anything hand-rounding a unit quaternion produces
  // (0.7 0 0 0.7 is only 1% short) and well inside a visible mis-scale. Same
  // tolerance as --validate's non_unit_rotation, so the two agree on what is
  // merely rounded and what is wrong.
  const f32 length = std::sqrt(length_sq);
  if (std::abs(length - 1.0f) > 0.05f)
    return std::format("length {} scales the mesh by that on top of Transform.scale", length);
  return {};
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

// Names an entity in a save error the way the author would look for it. Only
// called after EnsureGuid, so the guid fallback is always there.
std::string EntityLabel(ecs::World& world, ecs::Entity e) {
  if (const scene::Name* name = world.Get<scene::Name>(e)) return "'" + name->value + "'";
  return std::format("guid:0x{:016x}", world.Get<scene::Guid>(e)->value);
}

}  // namespace

bool SaveScene(ecs::World& world, const std::string& file_path, std::string* error) {
  // Union of identity-bearing entities (Guid, Name or Transform).
  std::unordered_set<u64> seen;
  std::vector<ecs::Entity> entities;
  auto collect = [&](ecs::Entity e) {
    if (world.Has<scene::Transient>(e)) return;
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

  // Refuse before the file is touched, so a rejected save leaves whatever was
  // on disk intact rather than a truncated document. There is no literal for
  // nan or inf that ReadFloat takes back, so writing one (FloatStr emits "nan"
  // and "inf" happily) means a scene that reloads as 0 with no signal. Failing
  // here names the component still holding the value, which is the last point
  // where it can be traced back to whatever produced it.
  for (ecs::Entity e : entities) {
    for (const ComponentDesc* comp : ComponentsOn(world, e)) {
      for (u32 i = 0; i < comp->prop_count; ++i) {
        const PropDesc& prop = comp->props[i];
        const u32 lanes = FloatLanes(prop.type);
        if (lanes == 0) continue;
        PropValue value;
        if (!GetProp(world, e, *comp, prop, &value)) continue;
        for (u32 lane = 0; lane < lanes; ++lane) {
          if (std::isfinite(value.f[lane])) continue;
          if (error)
            *error = std::format("{}.{} on entity {} is {}; the scene format has no literal for "
                                 "it, so nothing was written",
                                 comp->name, prop.name, EntityLabel(world, e), value.f[lane]);
          return false;
        }
      }
    }
  }

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

// `line` is the 1-based source line, carried only so strict mode can point at
// the offending assignment.
struct Assign {
  std::string comp;
  std::string prop;
  std::string raw;
  int line = 0;
};

struct BareComp {
  std::string name;
  int line = 0;
};

struct ParsedEntity {
  u64 guid = 0;
  std::vector<Assign> assigns;
  std::vector<BareComp> bare_comps;
};

// Resolves a prop by name within a component, or null.
const PropDesc* FindProp(const ComponentDesc& comp, std::string_view name) {
  for (u32 p = 0; p < comp.prop_count; ++p) {
    if (name == comp.props[p].name) return &comp.props[p];
  }
  return nullptr;
}

}  // namespace

bool LoadScene(ecs::World& world, asset::AssetDatabase& db, const std::string& file_path,
               std::string* error, bool strict) {
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
  int line_no = 1;  // the header consumed line 1
  while (std::getline(in, line)) {
    ++line_no;
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
      current->bare_comps.push_back({t, line_no});  // tag component
      continue;
    }
    std::string key = Trim(t.substr(0, eq));
    std::string raw = Trim(t.substr(eq + 1));
    size_t dot = key.find('.');
    if (dot == std::string::npos) {
      if (strict) {
        if (error)
          *error = std::format("{}:{}: '{}' is not a Component.prop assignment", file_path,
                               line_no, key);
        return false;
      }
      continue;  // malformed key
    }
    Assign a{key.substr(0, dot), key.substr(dot + 1), raw, line_no};
    if (a.comp == "Guid" && a.prop == "value") current->guid = ParseHexOrDec(raw);
    current->assigns.push_back(std::move(a));
  }

  // Strict mode resolves every name BEFORE anything is created, so a typo
  // leaves the world exactly as it was instead of half-populated.
  if (strict) {
    for (const ParsedEntity& pe : parsed) {
      for (const BareComp& bare : pe.bare_comps) {
        if (!FindComponentByName(bare.name)) {
          if (error)
            *error = std::format("{}:{}: unknown component '{}'", file_path, bare.line,
                                 bare.name);
          return false;
        }
      }
      for (const Assign& a : pe.assigns) {
        const ComponentDesc* comp = FindComponentByName(a.comp);
        if (!comp) {
          if (error)
            *error = std::format("{}:{}: unknown component '{}'", file_path, a.line, a.comp);
          return false;
        }
        const PropDesc* prop = FindProp(*comp, a.prop);
        if (!prop) {
          if (error)
            *error = std::format("{}:{}: component '{}' has no prop '{}'", file_path, a.line,
                                 a.comp, a.prop);
          return false;
        }
        // Numbers are checked here with the names, before pass 1 creates
        // anything, so a bad literal leaves the world exactly as it was. The
        // alternative is what this used to do: read the lanes it could, pad the
        // rest with zeros and hand back a value nothing downstream can tell
        // from an authored one.
        const u32 lanes = FloatLanes(prop->type);
        if (lanes == 0) continue;
        std::string why;
        const std::vector<f32> v = ParseFloats(a.raw, lanes, &why);
        if (why.empty() && prop->type == PropType::kQuat) why = QuatProblem(v);
        if (!why.empty()) {
          if (error)
            *error = std::format("{}:{}: {}.{} = {}: {}", file_path, a.line, a.comp, a.prop,
                                 a.raw, why);
          return false;
        }
      }
    }
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

    for (const BareComp& bare : pe.bare_comps) {
      const ComponentDesc* comp = FindComponentByName(bare.name);
      if (!comp) {
        RX_WARN("rxscene: unknown component '{}', skipped", bare.name);
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

      const PropDesc* prop = FindProp(*comp, a.prop);
      if (!prop) {
        RX_WARN("rxscene: unknown prop '{}.{}', skipped", a.comp, a.prop);
        continue;
      }

      // Strict already refused every literal this reports on, so the warning is
      // for the lenient callers (the editor), which keep the zero-padded
      // remainder and would otherwise get no signal at all.
      auto floats = [&](size_t n) {
        std::string why;
        std::vector<f32> v = ParseFloats(a.raw, n, &why);
        if (why.empty() && prop->type == PropType::kQuat) why = QuatProblem(v);
        if (!why.empty())
          RX_WARN("rxscene: {}:{}: {}.{} = {}: {}", file_path, a.line, a.comp, a.prop, a.raw, why);
        return v;
      };

      PropValue value;
      value.type = prop->type;
      switch (prop->type) {
        case PropType::kBool: value = PropValue::Bool(a.raw == "true"); break;
        case PropType::kI32: value = PropValue::I32(static_cast<i32>(std::strtol(a.raw.c_str(), nullptr, 0))); break;
        case PropType::kU32: value = PropValue::U32(static_cast<u32>(ParseHexOrDec(a.raw))); break;
        case PropType::kU64: value = PropValue::U64(ParseHexOrDec(a.raw)); break;
        case PropType::kF32: value = PropValue::F32(floats(1)[0]); break;
        case PropType::kVec2: {
          auto v = floats(2);
          value = PropValue::Vec2(v[0], v[1]);
          break;
        }
        case PropType::kVec3: {
          auto v = floats(3);
          value = PropValue::Vec3(v[0], v[1], v[2]);
          break;
        }
        case PropType::kVec4: {
          auto v = floats(4);
          value = PropValue::Vec4(v[0], v[1], v[2], v[3]);
          break;
        }
        case PropType::kQuat: {
          auto v = floats(4);
          value = PropValue::Quat(v[0], v[1], v[2], v[3]);
          break;
        }
        case PropType::kColor: {
          auto v = floats(4);
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
