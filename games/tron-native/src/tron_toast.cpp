// Tron: Evolution achievement popup (see tron_toast.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tron_toast.h"

#include "tron_sound.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <string>

#include <imgui.h>
#include <rex/ui/immediate_drawer.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace tron {
namespace {

constexpr float kOpenSeconds = 0.45f;   // a line of light, then the panel opens
constexpr float kShowSeconds = 5.5f;
constexpr float kCloseSeconds = 0.4f;
constexpr float kTotalSeconds = kOpenSeconds + kShowSeconds + kCloseSeconds;

float Smooth(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3 - 2 * x);
}

ImU32 Rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255)); }

}  // namespace

AchievementPopup::AchievementPopup(rex::ui::ImGuiDrawer* drawer,
                                   rex::ui::ImmediateDrawer* textures, rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icons_(textures, runtime) {
  LoadFont(textures);
  PrepareAchievementSound();
}

AchievementPopup::~AchievementPopup() = default;

void AchievementPopup::LoadFont(rex::ui::ImmediateDrawer* textures) {
  int w = 0, h = 0, channels = 0;
  auto* pixels = stbi_load("/app0/launcher-font.png", &w, &h, &channels, 4);
  if (!pixels || !textures) {
    if (pixels) stbi_image_free(pixels);
    return;
  }
  font_ = textures->CreateTexture(uint32_t(w), uint32_t(h),
                                  rex::ui::ImmediateTextureFilter::kLinear, false, pixels);
  stbi_image_free(pixels);
  font_width_ = float(w);
  font_height_ = float(h);
  std::ifstream in("/app0/launcher-font.txt");
  in >> font_cap_ >> font_pad_;
  int code;
  Glyph g;
  while (in >> code >> g.x >> g.y >> g.w >> g.h >> g.advance) {
    if (code >= 0 && code < 128) {
      g.valid = true;
      glyphs_[size_t(code)] = g;
    }
  }
}

float AchievementPopup::Text(ImDrawList* draw, const char* text, float x, float y, float cap,
                             ImU32 color, float tracking, bool measure_only) {
  const float s = cap / std::max(font_cap_, 1.0f);
  const float x0 = x;
  for (const char* p = text; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c >= 128 || !glyphs_[c].valid) c = '?';
    if (!glyphs_[c].valid) continue;
    const Glyph& g = glyphs_[c];
    if (!measure_only && font_) {
      const float gx = x - font_pad_ * s, gy = y - font_pad_ * s;
      draw->AddImage(reinterpret_cast<ImTextureID>(font_.get()), ImVec2(gx, gy),
                     ImVec2(gx + g.w * s, gy + g.h * s),
                     ImVec2(g.x / font_width_, g.y / font_height_),
                     ImVec2((g.x + g.w) / font_width_, (g.y + g.h) / font_height_), color);
    }
    x += g.advance * s + tracking;
  }
  return x - x0;
}

void AchievementPopup::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard lock(mutex_);
  queue_.push_back({event, std::nullopt});
}

void AchievementPopup::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  Pending current;
  float age = 0;
  {
    std::lock_guard lock(mutex_);
    while (!queue_.empty()) {
      auto& front = queue_.front();
      if (!front.shown) {
        front.shown = now;
        PlayAchievementSound();
      }
      age = std::chrono::duration<float>(now - *front.shown).count();
      if (age < kTotalSeconds) break;
      queue_.pop_front();
    }
    if (queue_.empty()) return;
    current = queue_.front();
  }

  // Open: a line of light grows from the centre, then the panel opens to full
  // height. Close: the reverse.
  float width_t = 1, height_t = 1;
  if (age < kOpenSeconds) {
    width_t = Smooth(age / (kOpenSeconds * 0.5f));
    height_t = Smooth((age - kOpenSeconds * 0.5f) / (kOpenSeconds * 0.5f));
  } else if (age > kOpenSeconds + kShowSeconds) {
    const float c = (age - kOpenSeconds - kShowSeconds) / kCloseSeconds;
    height_t = 1 - Smooth(c / 0.5f);
    width_t = 1 - Smooth((c - 0.5f) / 0.5f);
  }
  const float content = Smooth((age - kOpenSeconds * 0.7f) / 0.25f) *
                        (1 - Smooth((age - kOpenSeconds - kShowSeconds) / (kCloseSeconds * 0.4f)));

  const float u = io.DisplaySize.y / 1080.0f;  // design in 1080p units
  const float full_w = 760 * u, full_h = 118 * u, cut = 26 * u;
  const float cx = io.DisplaySize.x * 0.5f, top = 40 * u;
  const float w = std::max(full_w * width_t, 2 * u), h = std::max(full_h * height_t, 3 * u);
  const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
  const float y0 = top + (full_h - h) * 0.5f, y1 = y0 + h;
  const float c = std::min(cut, std::min(w, h) * 0.45f);

  auto* draw = ImGui::GetForegroundDrawList();
  const ImVec2 poly[6] = {{x0 + c, y0}, {x1, y0}, {x1, y1 - c}, {x1 - c, y1}, {x0, y1}, {x0, y0 + c}};

  // Glow, panel, scan light, edge, accent.
  for (int i = 3; i >= 1; --i) {
    draw->AddPolyline(poly, 6, Rgba(70, 225, 255, 0.10f), ImDrawFlags_Closed, (4 + 5 * i) * u);
  }
  draw->AddConvexPolyFilled(poly, 6, Rgba(3, 12, 18, 0.9f));
  if (height_t > 0.99f) {
    // A band of light that crosses the panel every 1.6 s.
    const float phase = std::fmod(age - kOpenSeconds, 1.6f) / 1.6f;
    const float band_x = x0 - 80 * u + (w + 160 * u) * phase;
    for (int i = 0; i < 12; ++i) {
      const float bx = band_x + i * 4 * u;
      const float a = 0.10f * std::sin(std::numbers::pi_v<float> * i / 12.0f);
      const float l = std::clamp(bx, x0 + c, x1 - c), r = std::clamp(bx + 4 * u, x0 + c, x1 - c);
      if (r > l) draw->AddRectFilled(ImVec2(l, y0 + 2 * u), ImVec2(r, y1 - 2 * u), Rgba(150, 240, 255, a));
    }
  }
  draw->AddPolyline(poly, 6, Rgba(90, 232, 255, 1.0f), ImDrawFlags_Closed, 2.5f * u);
  const ImVec2 accent[3] = {{x0, y1}, {x0, y0 + c}, {x0 + c, y0}};
  draw->AddPolyline(accent, 3, Rgba(255, 214, 60, 1.0f), 0, 3.5f * u);

  if (content <= 0.01f) return;

  // Identity disc: icon in a ring with spinning dashes.
  const float dx = cx - full_w * 0.5f + 70 * u, dy = top + full_h * 0.5f, r = 40 * u;
  const float spin = age * 1.6f;
  draw->AddCircleFilled(ImVec2(dx, dy), r, Rgba(10, 40, 55, content), 48);
  if (auto* icon = icons_.GetIcon(current.event.achievement)) {
    draw->AddImageRounded(reinterpret_cast<ImTextureID>(icon), ImVec2(dx - r + 4 * u, dy - r + 4 * u),
                          ImVec2(dx + r - 4 * u, dy + r - 4 * u), ImVec2(0, 0), ImVec2(1, 1),
                          Rgba(255, 255, 255, content), r);
  }
  draw->AddCircle(ImVec2(dx, dy), r + 8 * u, Rgba(90, 232, 255, content), 64, 2.5f * u);
  for (int k = 0; k < 8; ++k) {
    const float a0 = spin + k * std::numbers::pi_v<float> / 4;
    draw->PathArcTo(ImVec2(dx, dy), r + 15 * u, a0, a0 + 0.38f, 10);
    draw->PathStroke(Rgba(90, 232, 255, 0.9f * content), 0, 2.5f * u);
  }

  // Text.
  const float tx = cx - full_w * 0.5f + 140 * u;
  Text(draw, "ACHIEVEMENT UNLOCKED", tx, top + 24 * u, 15 * u, Rgba(150, 238, 255, content), 3 * u);
  const std::string score = std::to_string(current.event.achievement.gamerscore) + "G";
  const float score_w = Text(draw, score.c_str(), 0, 0, 22 * u, 0, 0, true);
  Text(draw, score.c_str(), cx + full_w * 0.5f - 34 * u - score_w, top + full_h - 44 * u, 22 * u,
       Rgba(255, 214, 60, content));
  // The name, smaller when it would run into the score.
  const char* name = current.event.achievement.label.c_str();
  float name_cap = 30 * u;
  const float room = full_w - 140 * u - score_w - 60 * u;
  const float name_w = Text(draw, name, 0, 0, name_cap, 0, 0, true);
  if (name_w > room) name_cap *= std::max(0.55f, room / name_w);
  Text(draw, name, tx, top + 56 * u + (30 * u - name_cap) * 0.5f, name_cap, Rgba(242, 248, 252, content));
}

}  // namespace tron
