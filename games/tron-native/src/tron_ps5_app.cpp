// Tron: Evolution (Xbox 360) on the PS5, native renderer build.
//
// The game code is ReXGlue's ahead-of-time translation of default.xex. The
// Xbox 360 D3D library's draws, clears, resolves and swaps are rendered by the
// native renderer (native/src/renderer, from the Dante's Inferno PS5 port) on
// DiligentCore/RADV. The SDK's Vulkan Xenos backend is still constructed, but
// only as a sync engine (gpu_host_rendering=false): it executes the command
// stream for fences, interrupts and frame pacing and draws nothing.
//
// The title opens on the launcher (src/launcher): PLAY starts the game from
// the extracted disc files in /app0/gamedata, or else from the player's disc
// image in /app0/dump, read in place.
//
// Paths:
//   /app0/dump/*.iso        the player's Xbox 360 disc image
//   /app0/gamedata          or the disc's files extracted (default.xex at its root)
//   /app0/launcher-*.png    the launcher screen (PLAY / EXIT selected)
//   /app0/userdata          profiles and saves
//   /app0/cache             shader, texture and pipeline caches
//   /app0/metadata          achievement data written out of default.xex
//   /app0/tron_evolution.toml  settings
//   /app0/tron-flags.txt    test settings, name=value per line

#include "tronevolution_init.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

#include "launcher/menu.h"
#include "tron_toast.h"
#include "renderer/renderer.h"
#include "renderer/renderer_config.h"
#include "renderer/ui_overlay.h"

extern "C" [[noreturn]] void catchReturnFromMain(int status);

namespace {

constexpr const char* kAppFolder = "/app0";

// The Xenos backend without a presenter: the native renderer owns the screen.
class SyncOnlyGraphics final : public rex::graphics::vulkan::VulkanGraphicsSystem {
 public:
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override { return 0; }
};

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

  // The launcher picks the game source; EXIT closes the title.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)>) override {
    auto paths = defaults;
    // The screen opens at the saved resolution; a change applies next start.
    launcher_active_ = tron::ps5::LoadLauncherSettings();
    tron::ps5::LauncherSettings chosen = launcher_active_;
    try {
      paths.game_data_root =
          tron::ps5::RunLauncher(window(), defaults.game_data_root, launcher_active_, chosen);
    } catch (const std::exception& error) {
      REXLOG_ERROR("TRON: launcher failed: {}", error.what());
      catchReturnFromMain(1);
    }
    if (paths.game_data_root.empty()) catchReturnFromMain(0);
    // 60 fps: every vblank; 30: the game's own interval (see frame_rate.cpp).
    rex::cvar::SetFlagByName("tron_present_interval", chosen.fps30 ? "2" : "1");
    // (OnPreSetup ran before the launcher.)
    rex::cvar::SetFlagByName("dante_resolution", launcher_active_.uhd ? "3840x2160" : "2560x1440");
    rex::cvar::SetFlagByName("dante_smaa", chosen.smaa ? "true" : "false");
    rex::cvar::SetFlagByName("dante_shadow_scale", chosen.high_shadows ? "2" : "1");
    rex::cvar::SetFlagByName("tron_present_gamma_x100",
                             std::to_string(int(chosen.present_gamma() * 100.0f + 0.5f)));
    xbox_icons_ = chosen.xbox_icons;
    REXLOG_INFO("TRON: {}x{} at {} fps, brightness {}%, SMAA {}, {} shadows, {} icons",
                launcher_active_.width(), launcher_active_.height(), chosen.fps30 ? 30 : 60,
                chosen.brightness, chosen.smaa ? "on" : "off",
                chosen.high_shadows ? "high" : "normal", chosen.xbox_icons ? "Xbox" : "PlayStation");
    REXLOG_INFO("TRON: game from {}", paths.game_data_root.string());
    return paths;
  }

  // The SDK's ImGui overlay (achievement popups) records into this drawer and
  // the native renderer draws it over each presented frame. ReXApp owns it.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    auto drawer = std::make_unique<renderer::OverlayDrawer>();
    overlay_drawer_ = drawer.get();
    return drawer;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    rex::cvar::SetFlagByName("mnk_mode", "false");
    // No developer settings overlay on L1 + R1 + Options: players use the
    // launcher's settings.
    rex::cvar::SetFlagByName("prospero_settings_chord", "false");
    // Occlusion queries: the native renderer does not count samples, and the
    // runtime's default answer (1000 visible) left the game's lens flares and
    // light glare drawn through walls as a blue haze whenever the camera faced
    // the light. Report them hidden instead (the smallest count allowed).
    rex::cvar::SetFlagByName("query_occlusion_fake_sample_count", "1");
    rex::cvar::SetFlagByName("fullscreen", "true");
    rex::cvar::SetFlagByName("vfetch_oob_trace", "false");
    rex::cvar::SetFlagByName("renderer", "dante");
    rex::cvar::SetFlagByName("dante_resolution", "2560x1440");  // the launcher's choice follows
    ApplyFlagsFile(std::filesystem::path(kAppFolder) / "tron-flags.txt");
    renderer::ConfigureSyncOnlyGpu();
    config.gpu_plugin.clear();
    config.graphics = std::make_unique<SyncOnlyGraphics>();
    REXLOG_INFO("TRON: native renderer build");
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
    auto render_cache = cache_root();
    if (!render_cache.empty()) render_cache /= launcher_active_.uhd ? "native-2160p" : "native";
    tron::ps5::HandOffLauncher();
    // PlayStation button symbols in place of the game's Xbox ones.
    const bool xbox_icons = xbox_icons_;
    renderer::SetButtonGlyphStyleSource([xbox_icons] {
      return xbox_icons ? renderer::GlyphStyle::kXbox : renderer::GlyphStyle::kPlayStation5;
    });
    if (!renderer::Start(window(), render_cache)) {
      REXLOG_ERROR("TRON: native renderer failed to start");
      std::_Exit(1);
    }
    REXLOG_INFO("TRON: native renderer ready");
    // The SDK presenter is detached: ImGui (the achievement popups) records on
    // the UI thread and the renderer draws it over each presented frame, as in
    // the Dante's Inferno PS5 port.
    if (imgui_drawer()) {
      imgui_drawer()->SetPresenterAndImmediateDrawer(nullptr, overlay_drawer_);
      renderer::SetOverlayFrameRequest([this] {
        if (overlay_frame_pending_.exchange(true)) return;
        app_context().CallInUIThread([this] {
          overlay_frame_pending_ = false;
          DrawOverlayFrame();
        });
      });
    }
  }

  // Achievement popups: Tron-styled, with icons drawn through the overlay.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    if (!imgui_drawer() || !runtime() || !overlay_drawer_) {
      REXLOG_WARN("TRON: no achievement popup (overlay not ready)");
      return nullptr;
    }
    REXLOG_INFO("TRON: achievement popup ready ({} achievements)",
                achievements().ListAchievements().size());
    return std::make_unique<tron::AchievementPopup>(imgui_drawer(), overlay_drawer_, runtime());
  }

  void DrawOverlayFrame() {
    if (shutting_down_ || !overlay_drawer_ || !imgui_drawer() || !window()) return;
    if (!imgui_drawer()->HasDialogs()) {
      overlay_drawer_->PublishEmpty();
      return;
    }
    rex::ui::AppUIDrawContext context(window()->GetActualPhysicalWidth(),
                                      window()->GetActualPhysicalHeight());
    imgui_drawer()->Draw(context);
  }

  void OnShutdown() override {
    shutting_down_ = true;
    renderer::SetOverlayFrameRequest(nullptr);
    renderer::Stop();
  }

  renderer::OverlayDrawer* overlay_drawer_ = nullptr;
  std::atomic<bool> overlay_frame_pending_{false};
  bool shutting_down_ = false;

  tron::ps5::LauncherSettings launcher_active_;
  bool xbox_icons_ = false;
};

}  // namespace

REX_DEFINE_APP(tronevolution, TronPs5App::Create);
