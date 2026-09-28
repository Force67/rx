#include "base/containers/unordered_map.h"
#include "base/strings/xstring.h"
#include "rxe/app/platform_config.h"
#include "rxe/asset/vfs.h"
#include "rxe/render/core/presets.h"
#include "rxe/render/core/settings.h"

#include <stdio.h>
#include <string.h>

#include <initializer_list>

namespace {

int failures = 0;

void Check(bool condition, const char* what, const char* detail = "") {
  if (condition) return;
  ::fprintf(stderr, "platform_config_test: FAIL: %s %s\n", what, detail);
  ++failures;
}

// Files held in memory, so the include and section rules can be tested without
// a directory on disk.
class MemoryProvider final : public rx::asset::FileProvider {
 public:
  void Add(const char* path, const char* text) { files_.insert(path, text); }
  bool Contains(base::StringRef path) const override {
    return files_.find(base::String(path)) != nullptr;
  }
  base::Optional<base::Vector<rx::u8>> Read(base::StringRef path) const override {
    const base::String* text = files_.find(base::String(path));
    if (!text) return {};
    base::Vector<rx::u8> bytes;
    for (char c : *text) bytes.push_back(static_cast<rx::u8>(c));
    return bytes;
  }
  void Enumerate(base::FunctionRef<void(base::StringRef)>) const override {}
  base::String name() const override { return "memory"; }

 private:
  base::UnorderedMap<base::String, base::String> files_;
};

void TestSections() {
  auto files = base::MakeUnique<MemoryProvider>();
  files->Add("base.ini",
             "[render.post]\nbloom = false\nsss = false\n"
             "[memory.arena]\nframe_mb = 4\n"
             "[options]\nunfocused.fps = 5\n");
  files->Add("game.ini",
             "include = t://base.ini\n"
             "[render]\nbloom = true\n"
             "[memory.budgets]\nassets = 100\n");
  files->Add("bad.ini",
             "[sparkles]\nx = 1\n"
             "[render]\nnot_a_key = 3\nbloom = maybe\n"
             "include = t://missing.ini\n"
             "loose line\n");
  files->Add("loop.ini", "include = t://loop.ini\n");
  rx::asset::Vfs vfs;
  vfs.Mount("t://", base::move(files));

  rx::app::PlatformConfig good;
  Check(rx::app::ReadPlatformConfig(vfs, "t://game.ini", &good), "reads a file");
  Check(good.problems == 0, "a clean file has no problems");
  rx::render::RenderSettings s = rx::render::PresetSettings(good.render, {});
  Check(s.bloom, "the including file's keys override the included ones");
  Check(!s.sss, "included keys apply");
  Check(good.memory.find("frame_mb = 4") != base::String::npos &&
            good.memory.find("[budgets]\nassets = 100") != base::String::npos,
        "memory sections carry through without their memory. prefix");
  Check(good.options.size() == 1 && good.options[0].name == "unfocused.fps",
        "options are collected by name");

  rx::app::PlatformConfig bad;
  rx::app::ReadPlatformConfig(vfs, "t://bad.ini", &bad);
  // unknown section, key in it, unknown render key, bad value, missing
  // include, line without '='.
  Check(bad.problems == 6, "every line that goes nowhere is a problem");

  rx::app::PlatformConfig loop;
  rx::app::ReadPlatformConfig(vfs, "t://loop.ini", &loop);
  Check(loop.problems == 1, "a cyclic include stops at the depth limit");

  rx::app::PlatformConfig none;
  Check(!rx::app::ReadPlatformConfig(vfs, "t://nope.ini", &none), "a missing file is false");
}

// Every shipped tier resolves through rxe://config with nothing left over.
void TestShippedTiers() {
  rx::asset::Vfs vfs;
  vfs.Mount("rxe://config/", rx::asset::MakeLooseFileProvider(RX_CONFIG_SOURCE_DIR));
  using QP = rx::render::QualityPreset;
  for (QP tier : {QP::kAndroidLow, QP::kAndroidMedium, QP::kAndroidHigh, QP::kSteamDeck,
                  QP::kLowEnd, QP::kConsole, QP::kMedium, QP::kHigh, QP::kUltra}) {
    rx::app::PlatformConfig config;
    const char* name = rx::render::PresetName(tier);
    Check(rx::app::ReadPlatformChain(vfs, "", tier, &config), "the tier file exists", name);
    Check(config.problems == 0, "the tier resolves without problems", name);
    Check(!config.render.empty(), "the tier sets render keys", name);
  }
}

void TestAndroidDetection() {
  struct Case {
    const char* adapter;
    const char* tier;
  };
  const Case cases[] = {
      {"Adreno (TM) 740", "android_high"},   {"Adreno (TM) 650", "android_medium"},
      {"Adreno (TM) 618", "android_low"},    {"Mali-G715 MC7", "android_high"},
      {"Mali-G610 MC6", "android_medium"},   {"Mali-G78 MP20", "android_medium"},
      {"Mali-G57 MC2", "android_low"},       {"Mali-T880", "android_low"},
      {"Immortalis-G720", "android_high"},   {"Samsung Xclipse 940", "android_high"},
      {"PowerVR Rogue GE8320", "android_low"},
  };
  for (const Case& c : cases) {
    rx::gpu::DeviceCaps caps{};
    caps.ray_query = true;
    caps.adapter_name = c.adapter;
    Check(::strcmp(rx::render::PresetName(rx::render::DetectPreset(caps)), c.tier) == 0,
          "mobile gpu lands in its tier", c.adapter);
  }
}

}  // namespace

int main() {
  TestSections();
  TestShippedTiers();
  TestAndroidDetection();
  if (failures != 0) return 1;
  ::printf("platform_config_test: PASS\n");
  return 0;
}
