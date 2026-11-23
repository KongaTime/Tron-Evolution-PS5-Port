// Tron: Evolution (Xbox 360) on the PS5.
//
// The game code is ReXGlue's ahead-of-time translation of default.xex. A PS5
// title cannot load the desktop rexgpu-xenos plugin, so the Vulkan Xenos
// backend is linked into the title and constructed here.
//
// Paths:
//   /app0/gamedata          the player's disc files (default.xex at its root)
//   /app0/userdata          profiles and saves
//   /app0/cache             shader and texture caches
//   /app0/metadata          achievement data written out of default.xex
//   /app0/tron_evolution.toml  settings

#include "tronevolution_init.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

namespace {

constexpr const char* kAppFolder = "/app0";

class TronPs5App final : public rex::ReXApp {
 public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& context) {
    return std::unique_ptr<TronPs5App>(new TronPs5App(context, "tronevolution", PPCImageConfig));
  }

 private:
  TronPs5App(rex::ui::WindowedAppContext& context, std::string_view name, rex::PPCImageInfo image)
      : rex::ReXApp(context, name, image) {}

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path app(kAppFolder);
    paths.game_data_root = app / "gamedata";
    paths.user_data_root = app / "userdata";
    paths.cache_root = app / "cache";
    paths.metadata_root = app / "metadata";
    paths.config_path = app / "tron_evolution.toml";

    std::error_code ec;
    for (const auto& folder : {paths.user_data_root, paths.cache_root, paths.metadata_root})
      std::filesystem::create_directories(folder, ec);
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin.clear();
    config.graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
    rex::cvar::SetFlagByName("mnk_mode", "false");
    rex::cvar::SetFlagByName("fullscreen", "true");
    // Emulate EDRAM render targets exactly (fragment shader interlock, which
    // RADV has on the PS5). The host-render-target path approximates the
    // 360's float formats, and UE3's bloom/HDR passes then blow out to white.
    // Bully needed the same setting.
    rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi");
    // Compile shaders before drawing. With async compilation a new effect is
    // drawn with a placeholder pipeline for a few frames; those frames are not
    // presented, but what the placeholder wrote stays in render targets and
    // resolved textures the next frames read, so the title screen's light
    // cones, mist and horizon flashed solid white or black. Pipelines are
    // cached in /app0/cache, so the hitch only happens the first time.
    rex::cvar::SetFlagByName("async_shader_compilation", "false");
    REXLOG_INFO("TRON: PS5 Vulkan Xenos backend (render targets: fsi), game data {}/gamedata", kAppFolder);
    ApplyFlagsFile(std::filesystem::path(kAppFolder) / "tron-flags.txt");
  }

  // Test settings without a rebuild: one `name=value` per line ('#' comments),
  // applied over the defaults above. Edit it over FTP between runs.
  static void ApplyFlagsFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t\r");
        const auto e = s.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
      };
      line = trim(line);
      if (line.empty() || line[0] == '#') continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string name = trim(line.substr(0, eq));
      const std::string value = trim(line.substr(eq + 1));
      const bool ok = rex::cvar::SetFlagByName(name, value);
      REXLOG_INFO("TRON: flag {}={} ({})", name, value, ok ? "set" : "unknown or rejected");
    }
  }

  void OnPostSetup() override {
    REXLOG_INFO("TRON: setup done; game data {}, saves {}", game_data_root().string(),
                user_data_root().string());
  }
};

}  // namespace

REX_DEFINE_APP(tronevolution, TronPs5App::Create);
