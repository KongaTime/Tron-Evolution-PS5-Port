// Tron: Evolution achievement popup: a dark angled panel with a glowing cyan
// edge, the achievement icon in a spinning identity-disc ring, the launcher's
// lettering, and a synthesized cue (tron_sound.cpp). Replaces the SDK toast.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include <rex/system/achievement_manager.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace tron {

class AchievementPopup final : public rex::ui::AchievementNotificationDialog {
 public:
  AchievementPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* textures,
                   rex::Runtime* runtime);
  ~AchievementPopup() override;

  // Any thread.
  void Push(const rex::system::AchievementEvent& event) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Glyph {
    float x = 0, y = 0, w = 0, h = 0, advance = 0;
    bool valid = false;
  };
  struct Pending {
    rex::system::AchievementEvent event;
    std::optional<std::chrono::steady_clock::time_point> shown;
  };

  void LoadFont(rex::ui::ImmediateDrawer* textures);
  // Text with its capitals' top at y, cap height `cap` (pixels); returns width.
  float Text(ImDrawList* draw, const char* text, float x, float y, float cap, ImU32 color,
             float tracking = 0, bool measure_only = false);

  std::mutex mutex_;
  std::deque<Pending> queue_;
  rex::ui::AchievementIconCache icons_;
  std::unique_ptr<rex::ui::ImmediateTexture> font_;
  float font_width_ = 1, font_height_ = 1, font_cap_ = 62, font_pad_ = 40;
  std::array<Glyph, 128> glyphs_{};
};

}  // namespace tron
