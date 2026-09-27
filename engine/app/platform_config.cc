#include "app/platform_config.h"

#include <stdlib.h>

#include <base/option.h>

#include "core/file_system.h"
#include "core/format.h"
#include "core/log.h"
#include "core/text_reader.h"
#include "render/core/settings.h"
#include "render/core/settings_ini.h"

namespace rx::app {
namespace {

constexpr int kMaxIncludeDepth = 8;

// RX_CONFIG=<file> layers one more platform config over everything, for tuning
// on a device without a rebuild: a disk path, or a vfs path with a mount.
base::Option<const char*> ExtraConfig{"config", nullptr, "RX_CONFIG"};

base::String Trim(base::StringRef s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return base::String(s.data() + b, e - b);
}

bool Read(const asset::Vfs& vfs, base::StringRef path, base::String* text) {
  if (path.find("://") == base::StringRef::npos) return fs::ReadTextFile(path, text);
  base::Optional<base::Vector<u8>> bytes = vfs.Read(path);
  if (!bytes) return false;
  *text = base::String(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  return true;
}

bool IsMemoryGroup(base::StringRef group) {
  return group == "arena" || group == "pools" || group == "budgets";
}

bool ReadAt(const asset::Vfs& vfs, base::StringRef path, PlatformConfig* out, int depth) {
  base::String text;
  if (!Read(vfs, path, &text)) return false;

  base::String section;
  LineReader lines(text);
  base::StringRef piece;
  int line_no = 0;
  auto problem = [&](const char* what, const base::String& detail) {
    RX_ERROR("{}:{}: {} '{}'", path, line_no, what, detail);
    ++out->problems;
  };
  while (lines.Next(&piece)) {
    ++line_no;
    base::String line(piece.data(), piece.size());
    if (auto hash = line.find_first_of(";#"); hash != base::String::npos) line.resize(hash);
    const base::String t = Trim(line);
    if (t.empty()) continue;
    if (t[0] == '[') {
      section = Trim(base::StringRef(t).substr(1, t.find(']') - 1));
      const bool known = section == "render" || section.starts_with("render.") ||
                         section == "options" ||
                         (section.starts_with("memory.") &&
                          IsMemoryGroup(base::StringRef(section).substr(7)));
      if (!known) problem("unknown section", section);
      continue;
    }
    const size_t eq = t.find('=');
    if (eq == base::String::npos) {
      problem("not a key = value line", t);
      continue;
    }
    const base::String key = Trim(base::StringRef(t).substr(0, eq));
    const base::String value = Trim(base::StringRef(t).substr(eq + 1));

    if (key == "include") {
      // Where it stands in the file does not matter: an include applies before
      // the keys around it, which is what "start from that file" means.
      if (depth >= kMaxIncludeDepth)
        problem("include nested too deep, a cycle?", value);
      else if (!ReadAt(vfs, value, out, depth + 1))
        problem("include not found", value);
      continue;
    }
    if (section == "render" || section.starts_with("render.")) {
      const base::String kv = key + " = " + value + "\n";
      render::RenderSettings scratch;
      if (render::ApplyIni(kv, scratch) != 1)
        problem("unknown render key or bad value", key + " = " + value);
      else
        out->render += kv;
    } else if (section.starts_with("memory.")) {
      out->memory += "[" + base::String(base::StringRef(section).substr(7)) + "]\n" + key +
                     " = " + value + "\n";
    } else if (section == "options") {
      out->options.push_back({key, value, base::String(path) + ":" + ToString(line_no)});
    } else {
      problem("key outside a known section", key);
    }
  }
  return true;
}

}  // namespace

bool ReadPlatformConfig(const asset::Vfs& vfs, base::StringRef path, PlatformConfig* out) {
  return ReadAt(vfs, path, out, 0);
}

bool ReadPlatformChain(const asset::Vfs& vfs, base::StringRef title,
                       render::QualityPreset tier, PlatformConfig* out) {
  const bool tiered = tier != render::QualityPreset::kAuto;
  const base::String tier_file = base::String(render::PresetName(tier)) + ".ini";
  const base::String game = title.empty() ? base::String() : base::String(title) + "://config/";
  ReadPlatformConfig(vfs, "rxe://config/default.ini", out);
  if (tiered && !ReadPlatformConfig(vfs, "rxe://config/" + tier_file, out)) {
    RX_ERROR("rxe://config/{} not found: rxe/config/ must sit beside the executable "
             "(docs/CONFIG.md)", tier_file);
    return false;
  }
  if (!game.empty()) {
    ReadPlatformConfig(vfs, game + "default.ini", out);
    if (tiered) ReadPlatformConfig(vfs, game + tier_file, out);
  }
  if (const char* extra = ExtraConfig.get(); extra && *extra && !ReadPlatformConfig(vfs, extra, out)) {
    RX_ERROR("RX_CONFIG: cannot read '{}'", extra);
    ++out->problems;
  }
  return true;
}

void ApplyPlatformOptions(PlatformConfig& config) {
  for (const PlatformConfig::Option& entry : config.options) {
    bool found = false;
    base::OptionBase::VisitAll([&](const base::OptionBase* registered) {
      if (found || entry.name != registered->name()) return;
      found = true;
      const char* env = registered->env();
      if (env && ::getenv(env)) return;  // the environment has the last word
      // The chain hands out const pointers; options are mutable globals, which
      // is how base::InitOptionsFromEnv writes them too.
      if (!const_cast<base::OptionBase*>(registered)->SetFromString(entry.value.c_str())) {
        RX_ERROR("{}: bad value '{}' for option '{}'", entry.where, entry.value, entry.name);
        ++config.problems;
      }
    });
    if (!found) {
      RX_ERROR("{}: no registered option '{}'", entry.where, entry.name);
      ++config.problems;
    }
  }
}

}  // namespace rx::app
