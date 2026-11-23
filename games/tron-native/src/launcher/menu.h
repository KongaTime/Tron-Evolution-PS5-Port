// The title's launcher: the start screen with PLAY, SETTINGS and EXIT. PLAY
// finds the game (an extracted folder, or the player's Xbox 360 disc image
// read in place) and keeps the screen until the game renderer takes it over.
// From the Dante's Inferno PS5 port (ps5/launcher/menu.h); the settings page
// follows the Oogie's Revenge PS5 port's.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>

namespace rex::ui {
class Window;
}

namespace tron::ps5 {

// The player's choices, kept in /app0/userdata/launcher.txt.
struct LauncherSettings {
  bool uhd = false;          // render and present at 3840x2160 (else 2560x1440)
  bool fps30 = false;        // the game's own 30 fps (else 60)
  int brightness = 100;      // percent; 100 = the port's tuned picture
  bool smaa = true;          // anti-aliasing of the finished frame
  bool high_shadows = true;  // shadow maps at twice the Xbox 360's size
  bool xbox_icons = false;   // the game's own Xbox button icons

  uint32_t width() const { return uhd ? 3840 : 2560; }
  uint32_t height() const { return uhd ? 2160 : 1440; }
  // Presentation gamma for `brightness` (100% = 1.19, as tuned on the console;
  // each doubling of brightness lowers it by 1).
  float present_gamma() const { return 1.19f - std::log2(float(brightness) / 100.0f); }
};

LauncherSettings LoadLauncherSettings();

// Returns the game root when the player selects PLAY, or empty on EXIT: the
// extracted game folder `extracted` when it holds default.xex, else a verified
// disc image from the app's dump/ folder (mounted in place by the runtime).
// `settings` holds the choices when it returns (saved as they change); the
// screen is `active`'s size, so a new resolution applies at the next start.
// The launcher keeps the screen after PLAY; call HandOffLauncher before the
// game renderer starts.
std::filesystem::path RunLauncher(rex::ui::Window* window, const std::filesystem::path& extracted,
                                  const LauncherSettings& active, LauncherSettings& settings);

// Gives the launcher's swap chain to the game renderer. Safe to call twice.
void HandOffLauncher();

}  // namespace tron::ps5
