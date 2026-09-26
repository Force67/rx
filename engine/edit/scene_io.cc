#include "edit/scene_io.h"

#include <math.h>
#include <stdlib.h>

#include "asset/asset_id.h"
#include "base/containers/unordered_map.h"
#include "base/containers/unordered_set.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/format.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/text_reader.h"
#include "core/text_writer.h"
#include "edit/hierarchy.h"
#include "edit/reflect.h"
#include "scene/components.h"
#include "core/sort.h"

namespace rx::edit {
namespace {

constexpr int kSceneVersion = 1;

base::String Trim(base::StringRef s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == base::StringRef::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return base::String(s.substr(a, b - a + 1));
}

base::String QuoteString(base::StringRef s) {
  base::String out = "\"";
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

base::String Unquote(base::StringRef raw) {
  base::String s = Trim(raw);
  base::StringRef sv = s;
  if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"') sv = sv.substr(1, sv.size() - 2);
  base::String out;
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

base::String FloatStr(f32 v) { return rx::StrFormat("{}", v); }

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
const char* ReadFloat(const base::String& token, f32* out) {
  char* end = nullptr;
  const f32 v = ::strtof(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0') return "is not a number";
  if (!isfinite(v)) return "is not finite";
  *out = v;
  return nullptr;
}

// Reads up to `n` whitespace-separated floats, zero-padding a short list (a
// plane is authored "Shape.size = 9 0"). Stops at the first token that is not a
// finite number and reports it through `error`; the lanes from there on stay 0,
// which is what a lenient load keeps and what a strict load refuses.
//
// A list LONGER than the prop is refused as well. The surplus used to be
// dropped in silence, which is how every city prefab came to carry
// "Pattern.scale = 5 6" - meaning 5 bays across and 6 floors up - against a
// scalar prop that read the 5 and discarded the rest. The facade that came back
// was one nobody had authored, and no load, no --validate and no warning said
// so. A short list pads because the format asks it to; a long one is the author
// believing in a prop that is not there.
base::Vector<f32> ParseFloats(base::StringRef s, size_t n, base::String* error = nullptr) {
  base::Vector<f32> out;
  TokenReader in(s);
  base::String token;
  while (out.size() < n && in.Next(&token)) {
    f32 v = 0.f;
    if (const char* why = ReadFloat(token, &v)) {
      if (error) *error = "'" + token + "' " + why;
      break;
    }
    out.push_back(v);
  }
  // Only reached with n lanes in hand, so a token left over is surplus rather
  // than the tail of a list that stopped early on a bad number.
  if (out.size() == n && in.Next(&token)) {
    if (error)
      *error = rx::StrFormat("takes {} value{} and was given more, starting at '{}'", n,
                           n == 1 ? "" : "s", token);
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
base::String QuatProblem(const base::Vector<f32>& v) {
  const f32 length_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
  if (length_sq < 1e-8f)
    return "the zero quaternion collapses the mesh to a point (identity is 0 0 0 1)";
  // 5% is far outside anything hand-rounding a unit quaternion produces
  // (0.7 0 0 0.7 is only 1% short) and well inside a visible mis-scale. Same
  // tolerance as --validate's non_unit_rotation, so the two agree on what is
  // merely rounded and what is wrong.
  const f32 length = ::sqrtf(length_sq);
  if (::fabsf(length - 1.0f) > 0.05f)
    return rx::StrFormat("length {} scales the mesh by that on top of Transform.scale", length);
  return {};
}

u64 ParseHexOrDec(base::StringRef s) {
  base::String t = Trim(s);
  return static_cast<u64>(::strtoull(t.c_str(), nullptr, 0));  // base 0: 0x -> hex
}

// Renders one field of a component to a literal. Entity/AssetId need world/db
// context, handled by the caller before falling through here.
base::String FormatValue(const PropValue& v) {
  switch (v.type) {
    case PropType::kBool: return v.b ? "true" : "false";
    case PropType::kI32: return rx::StrFormat("{}", static_cast<i32>(v.i));
    case PropType::kU32: return rx::StrFormat("{}", static_cast<u32>(v.u));
    case PropType::kU64: return rx::StrFormat("0x{:016x}", v.u);
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
      return rx::StrFormat("hash:0x{:016x}", v.u);
    }
    case PropType::kEntity: return "null";  // resolved by caller
  }
  return {};
}

u64 PackKey(ecs::Entity e) { return static_cast<u64>(e.generation) << 32 | e.index; }

// Names an entity in a save error the way the author would look for it. Only
// called after EnsureGuid, so the guid fallback is always there.
base::String EntityLabel(ecs::World& world, ecs::Entity e) {
  if (const scene::Name* name = world.Get<scene::Name>(e)) return "'" + name->value + "'";
  return rx::StrFormat("guid:0x{:016x}", world.Get<scene::Guid>(e)->value);
}

}  // namespace

bool SaveScene(ecs::World& world, const base::String& file_path, base::String* error) {
  // Union of identity-bearing entities (Guid, Name or Transform).
  base::UnorderedSet<u64> seen;
  base::Vector<ecs::Entity> entities;
  auto collect = [&](ecs::Entity e) {
    if (world.Has<scene::Transient>(e)) return;
    if (seen.insert(PackKey(e))) entities.push_back(e);
  };
  world.Each<scene::Guid>([&](ecs::Entity e, scene::Guid&) { collect(e); });
  world.Each<scene::Name>([&](ecs::Entity e, scene::Name&) { collect(e); });
  world.Each<scene::Transform>([&](ecs::Entity e, scene::Transform&) { collect(e); });

  // Every saved entity needs a guid so references resolve on reload; sort by it
  // for a stable, diff-friendly ordering.
  for (ecs::Entity e : entities) EnsureGuid(world, e);
  // Stable, as a loaded scene may repeat a guid: tied entities keep walk order.
  rx::StableSort(entities.data(), entities.data() + entities.size(),
                 [&](ecs::Entity a, ecs::Entity b) {
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
          if (isfinite(value.f[lane])) continue;
          if (error)
            *error = rx::StrFormat("{}.{} on entity {} is {}; the scene format has no literal for "
                                 "it, so nothing was written",
                                 comp->name, prop.name, EntityLabel(world, e), value.f[lane]);
          return false;
        }
      }
    }
  }

  TextWriter out;
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
        base::String literal;
        if (prop.type == PropType::kEntity) {
          if (value.e && world.IsAlive(value.e)) {
            u64 g = EnsureGuid(world, value.e);
            literal = rx::StrFormat("guid:0x{:016x}", g);
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
  if (!fs::WriteTextFile(file_path, out.str())) {
    if (error) *error = "cannot open '" + file_path + "' for writing";
    return false;
  }
  return true;
}

namespace {

// `line` is the 1-based source line, carried only so strict mode can point at
// the offending assignment.
struct Assign {
  base::String comp;
  base::String prop;
  base::String raw;
  int line = 0;
};

struct BareComp {
  base::String name;
  int line = 0;
};

struct ParsedEntity {
  u64 guid = 0;
  base::Vector<Assign> assigns;
  base::Vector<BareComp> bare_comps;
};

// Resolves a prop by name within a component, or null.
const PropDesc* FindProp(const ComponentDesc& comp, base::StringRef name) {
  for (u32 p = 0; p < comp.prop_count; ++p) {
    if (name == comp.props[p].name) return &comp.props[p];
  }
  return nullptr;
}

}  // namespace

bool LoadScene(ecs::World& world, asset::AssetDatabase& db, const base::String& file_path,
               base::String* error, bool strict) {
  base::String text;
  if (!fs::ReadTextFile(file_path, &text)) {
    if (error) *error = "cannot open '" + file_path + "' for reading";
    return false;
  }

  LineReader in(text);
  base::StringRef line;
  if (!in.Next(&line)) {
    if (error) *error = "empty scene file";
    return false;
  }
  {
    const base::String trimmed = Trim(line);
    TokenReader header(trimmed);
    base::String magic;
    int version = 0;
    header.Next(&magic);
    header.Next(&version);
    if (magic != "rxscene") {
      if (error) *error = "not an rxscene file";
      return false;
    }
    if (version > kSceneVersion) {
      if (error) *error = rx::StrFormat("scene version {} newer than supported {}", version,
                                     kSceneVersion);
      return false;
    }
  }

  base::Vector<ParsedEntity> parsed;
  ParsedEntity* current = nullptr;
  int line_no = 1;  // the header consumed line 1
  while (in.Next(&line)) {
    ++line_no;
    base::String t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t == "entity") {
      parsed.emplace_back();
      current = &parsed.back();
      continue;
    }
    if (!current) continue;  // stray line before first entity

    size_t eq = t.find('=');
    if (eq == base::String::npos) {
      current->bare_comps.push_back({t, line_no});  // tag component
      continue;
    }
    base::String key = Trim(t.substr(0, eq));
    base::String raw = Trim(t.substr(eq + 1));
    size_t dot = key.find('.');
    if (dot == base::String::npos) {
      if (strict) {
        if (error)
          *error = rx::StrFormat("{}:{}: '{}' is not a Component.prop assignment", file_path,
                               line_no, key);
        return false;
      }
      continue;  // malformed key
    }
    Assign a{key.substr(0, dot), key.substr(dot + 1), raw, line_no};
    if (a.comp == "Guid" && a.prop == "value") current->guid = ParseHexOrDec(raw);
    current->assigns.push_back(base::move(a));
  }

  // Strict mode resolves every name BEFORE anything is created, so a typo
  // leaves the world exactly as it was instead of half-populated.
  if (strict) {
    for (const ParsedEntity& pe : parsed) {
      for (const BareComp& bare : pe.bare_comps) {
        if (!FindComponentByName(bare.name)) {
          if (error)
            *error = rx::StrFormat("{}:{}: unknown component '{}'", file_path, bare.line,
                                 bare.name);
          return false;
        }
      }
      for (const Assign& a : pe.assigns) {
        const ComponentDesc* comp = FindComponentByName(a.comp);
        if (!comp) {
          if (error)
            *error = rx::StrFormat("{}:{}: unknown component '{}'", file_path, a.line, a.comp);
          return false;
        }
        const PropDesc* prop = FindProp(*comp, a.prop);
        if (!prop) {
          if (error)
            *error = rx::StrFormat("{}:{}: component '{}' has no prop '{}'", file_path, a.line,
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
        base::String why;
        const base::Vector<f32> v = ParseFloats(a.raw, lanes, &why);
        if (why.empty() && prop->type == PropType::kQuat) why = QuatProblem(v);
        if (!why.empty()) {
          if (error)
            *error = rx::StrFormat("{}:{}: {}.{} = {}: {}", file_path, a.line, a.comp, a.prop,
                                 a.raw, why);
          return false;
        }
      }
    }
  }

  // Pass 1: create entities and map guids.
  base::Vector<ecs::Entity> created;
  created.reserve(parsed.size());
  base::UnorderedMap<u64, ecs::Entity> by_guid;
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
        base::String why;
        base::Vector<f32> v = ParseFloats(a.raw, n, &why);
        if (why.empty() && prop->type == PropType::kQuat) why = QuatProblem(v);
        if (!why.empty())
          RX_WARN("rxscene: {}:{}: {}.{} = {}: {}", file_path, a.line, a.comp, a.prop, a.raw, why);
        return v;
      };

      PropValue value;
      value.type = prop->type;
      switch (prop->type) {
        case PropType::kBool: value = PropValue::Bool(a.raw == "true"); break;
        case PropType::kI32: value = PropValue::I32(static_cast<i32>(::strtol(a.raw.c_str(), nullptr, 0))); break;
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
            value = PropValue::AssetIdV(ParseHexOrDec(base::StringRef(a.raw).substr(5)));
          } else {
            base::String path = asset::NormalizePath(Unquote(a.raw));
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
            u64 g = ParseHexOrDec(base::StringRef(a.raw).substr(5));
            if (const ecs::Entity* found = by_guid.find(g)) ref = *found;
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
