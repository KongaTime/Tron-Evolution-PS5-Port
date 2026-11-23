// The title's launcher (see menu.h). Drawn with the native renderer's device
// and overlay, as in the Dante's Inferno PS5 port (ps5/launcher/menu.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include "iso_import.h"
#include "platform.h"
#include "renderer/renderer.h"
#include "renderer/ui_overlay.h"
#include "renderer/vulkan_device.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <imgui.h>
#include <xxhash.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C" void title_note(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace tron::ps5 {
namespace {

namespace fs = std::filesystem;

// XXH64 of the default.xex the port is generated from: Disney Tron: Evolution
// (USA, Europe), Xbox 360, title 425607ED, no title update.
constexpr uint64_t kBaseXexHash = 0x105cf1194012c498ULL;

// The disc image goes in dump/ inside the app's folder (or the folder itself).
const std::vector<fs::path> kIsoRoots = {"/app0/dump", "/app0"};

constexpr const char* kSettingsFile = "/app0/userdata/launcher.txt";

enum MainRow { kPlay, kSettings, kExit, kMainRows };
enum SettingRow { kResolution, kFrameRate, kBrightness, kSmaa, kShadows, kIcons, kBack, kSettingRows };
constexpr int kBrightnessMin = 60, kBrightnessMax = 150, kBrightnessStep = 5;

// The art's layout (its pixels, 1672 x 941). The font was drawn for 2160p.
constexpr float kArtWidth = 1672, kArtHeight = 941;
constexpr float kFontScale = 2160.0f / kArtHeight;  // font pixels per art pixel
constexpr float kLabelLeft = 195, kValueRight = 612;
constexpr float kBarLeft = 76, kBarTop = -49;  // the bar sprite, from the row centre
constexpr float kArrowLeft = 134, kArrowTop = -19;
constexpr float kMainRow0 = 531, kMainStep = 72;
constexpr float kSetRow0 = 520, kSetStep = 50, kSetSize = 0.72f;
constexpr float kCapHeight = 27;  // art pixels at size 1

bool ExtractedReady(const fs::path& root) {
  std::error_code error;
  return !root.empty() && fs::is_regular_file(root / "default.xex", error);
}

bool SaveSettings(const LauncherSettings& s) {
  std::ofstream out(kSettingsFile, std::ios::trunc);
  out << "resolution=" << (s.uhd ? "2160p" : "1440p") << "\n"
      << "frame_rate=" << (s.fps30 ? 30 : 60) << "\n"
      << "brightness=" << s.brightness << "\n"
      << "smaa=" << (s.smaa ? "on" : "off") << "\n"
      << "shadows=" << (s.high_shadows ? "high" : "normal") << "\n"
      << "button_icons=" << (s.xbox_icons ? "xbox" : "playstation") << "\n";
  out.close();
  return bool(out);
}

// The first disc image in dump/ whose default.xex is the supported build.
// Throws std::runtime_error with a message for the player.
fs::path FindDisc() {
  const auto isos = findGameIsos(kIsoRoots);
  if (isos.empty()) {
    throw std::runtime_error("No game found. Put your Xbox 360 ISO in the dump folder.");
  }
  std::string rejection;
  for (const auto& iso : isos) {
    try {
      inspectGameIso(iso, [](std::span<const uint8_t> xex) {
        if (XXH64(xex.data(), xex.size(), 0) != kBaseXexHash) {
          throw std::runtime_error("This disc is not the supported release.");
        }
      });
      return iso;
    } catch (const std::exception& error) {
      title_note("launcher: ISO %s rejected: %s", iso.filename().c_str(), error.what());
      rejection = error.what();
    }
  }
  throw std::runtime_error(isos.size() == 1 ? rejection
                                            : "No ISO in the dump folder is the supported release.");
}

struct Sprite {
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
  float width = 0, height = 0;  // pixels
  explicit operator bool() const { return bool(texture); }
};

struct Glyph {
  float x = 0, y = 0, w = 0, h = 0, advance = 0;
  bool valid = false;
};

class Menu final : public rex::ui::ImGuiDialog {
 public:
  Menu(rex::ui::ImGuiDrawer* drawer, renderer::OverlayDrawer& overlay) : ImGuiDialog(drawer) {
    background_ = Load(overlay, "bg");
    bar_ = Load(overlay, "bar");
    arrow_ = Load(overlay, "arrow");
    font_ = Load(overlay, "font");
    LoadFontMetrics();
  }

  bool in_settings = false;
  int main_row = kPlay;
  int setting_row = kResolution;
  LauncherSettings settings;
  LauncherSettings active;
  std::string status;

 private:
  static Sprite Load(renderer::OverlayDrawer& overlay, const char* name) {
    const std::string path = std::string("/app0/launcher-") + name + ".png";
    int width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    Sprite sprite;
    if (!pixels) {
      title_note("launcher: no art at %s", path.c_str());
      return sprite;
    }
    sprite.texture = overlay.CreateTexture(width, height, rex::ui::ImmediateTextureFilter::kLinear,
                                           false, pixels);
    sprite.width = float(width);
    sprite.height = float(height);
    stbi_image_free(pixels);
    return sprite;
  }

  // launcher-font.txt: "cap pad", then "code x y w h advance" per character.
  void LoadFontMetrics() {
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

  // Width of `text` in art pixels at `size`.
  float TextWidth(std::string_view text, float size) const {
    float w = 0;
    for (unsigned char c : text) {
      if (c < 128 && glyphs_[c].valid) w += glyphs_[c].advance;
    }
    return w / kFontScale * size * (kCapHeight * kFontScale / std::max(font_cap_, 1.0f));
  }

  void OnDraw(ImGuiIO& io) override {
    const auto size = io.DisplaySize;
    draw_ = ImGui::GetForegroundDrawList();
    sx_ = size.x / kArtWidth;
    sy_ = size.y / kArtHeight;
    if (background_) {
      draw_->AddImage(reinterpret_cast<ImTextureID>(background_.texture.get()), {0, 0}, size);
    }
    if (in_settings) {
      DrawSettings();
    } else {
      DrawMain();
    }
  }

  void Image(const Sprite& s, float x, float y, float w, float h) {
    if (!s) return;
    draw_->AddImage(reinterpret_cast<ImTextureID>(s.texture.get()), ImVec2(x * sx_, y * sy_),
                    ImVec2((x + w) * sx_, (y + h) * sy_));
  }

  // The highlight bar and arrow around a row centred at y, `scale` tall.
  void Highlight(float y, float scale) {
    Image(bar_, kBarLeft, y + kBarTop * scale, bar_.width, bar_.height * scale);
    Image(arrow_, kArrowLeft, y + kArrowTop * scale, arrow_.width * scale,
          arrow_.height * scale);
  }

  // Text with its capitals centred on y; `right`: x is the right edge.
  void Text(std::string_view text, float x, float y, float size, bool right = false,
            ImU32 tint = IM_COL32_WHITE) {
    if (!font_) return;
    const float px = size * kCapHeight / std::max(font_cap_, 1.0f);  // art px per font px
    if (right) x -= TextWidth(text, size);
    const float top = y - size * kCapHeight * 0.5f;
    for (unsigned char c : text) {
      if (c >= 128 || !glyphs_[c].valid) continue;
      const Glyph& g = glyphs_[c];
      const float gx = x - font_pad_ * px, gy = top - font_pad_ * px;
      draw_->AddImage(reinterpret_cast<ImTextureID>(font_.texture.get()), ImVec2(gx * sx_, gy * sy_),
                      ImVec2((gx + g.w * px) * sx_, (gy + g.h * px) * sy_),
                      ImVec2(g.x / font_.width, g.y / font_.height),
                      ImVec2((g.x + g.w) / font_.width, (g.y + g.h) / font_.height), tint);
      x += g.advance * px;
    }
  }

  void DrawMain() {
    const char* labels[kMainRows] = {"PLAY", "SETTINGS", "EXIT"};
    Highlight(kMainRow0 + kMainStep * float(main_row), 1.0f);
    for (int i = 0; i < kMainRows; ++i) Text(labels[i], kLabelLeft, kMainRow0 + kMainStep * i, 1.0f);
    if (!status.empty()) Text(status, kLabelLeft, kMainRow0 + kMainStep * kMainRows, 0.55f);
  }

  void DrawSettings() {
    const ImU32 cyan = IM_COL32(150, 238, 255, 255);
    // A dark panel keeps the rows readable over the art's bright floor lines.
    draw_->AddRectFilled(ImVec2(150 * sx_, (kSetRow0 - kSetStep * 1.7f) * sy_),
                         ImVec2(680 * sx_, (kSetRow0 + kSetStep * (kSettingRows + 0.6f)) * sy_),
                         IM_COL32(2, 10, 16, 175), 14 * sy_);
    Text("SETTINGS", kLabelLeft, kSetRow0 - kSetStep * 1.1f, 0.8f, false, cyan);
    Highlight(kSetRow0 + kSetStep * float(setting_row), 0.72f);
    const LauncherSettings& s = settings;
    const std::string brightness = std::to_string(s.brightness) + "%";
    const char* labels[kSettingRows] = {"RESOLUTION", "FRAME RATE",    "BRIGHTNESS", "ANTI-ALIASING",
                                        "SHADOWS",    "BUTTON ICONS", "BACK"};
    const std::string values[kSettingRows] = {
        s.uhd ? "4K" : "1440p",   s.fps30 ? "30 FPS" : "60 FPS",       brightness,
        s.smaa ? "ON" : "OFF",    s.high_shadows ? "HIGH" : "NORMAL", s.xbox_icons ? "XBOX" : "PS5",
        ""};
    for (int i = 0; i < kSettingRows; ++i) {
      const float y = kSetRow0 + kSetStep * i;
      Text(labels[i], kLabelLeft, y, kSetSize);
      if (!values[i].empty()) Text(values[i], kValueRight, y, kSetSize, true, cyan);
    }
    const std::string hint = !status.empty() ? status : "LEFT / RIGHT CHANGE     CIRCLE BACK";
    Text(hint, kLabelLeft, kSetRow0 + kSetStep * kSettingRows + 4, 0.5f);
  }

  Sprite background_, bar_, arrow_, font_;
  std::array<Glyph, 128> glyphs_{};
  float font_cap_ = 62, font_pad_ = 40;
  ImDrawList* draw_ = nullptr;
  float sx_ = 1, sy_ = 1;
};

// Everything the launcher draws with. It outlives RunLauncher so the screen
// stays up while the game loads, until HandOffLauncher.
struct LauncherSession {
  LauncherSession(rex::ui::Window* window, uint32_t width, uint32_t height) : window(window) {
    // Heap-allocated so the game renderer can take over the very same swap chain.
    device = std::make_unique<renderer::VulkanDevice>();
    if (!device->Initialize(nullptr, width, height)) {
      throw std::runtime_error("Could not initialize the launcher display.");
    }
    overlay_renderer = std::make_unique<renderer::OverlayRenderer>(device.get());
    drawer = std::make_unique<rex::ui::ImGuiDrawer>(window, 64);
    drawer->SetImmediateDrawer(&overlay_drawer);
    menu = std::make_unique<Menu>(drawer.get(), overlay_drawer);
    device->SetBeforePresent([this](auto* back_buffer) {
      overlay_renderer->Draw(device->Context(), back_buffer);
    });
  }

  void Paint() {
    rex::ui::AppUIDrawContext context(window->GetActualPhysicalWidth(),
                                      window->GetActualPhysicalHeight());
    drawer->Draw(context);
    device->BeginFrame();
    device->Clear(.01f, .04f, .06f, 1.0f);
    device->Present(1);
  }

  void HandOff() {
    // The game renderer reads the same overlay frame mailbox.
    overlay_drawer.PublishEmpty();
    // A swap chain created after this one reports successful flips but never
    // reaches the panel, so the game presents on this one.
    device->SetBeforePresent(nullptr);
    renderer::AdoptDevice(std::move(device));
    title_note("launcher: handed the swap chain to the game renderer");
  }

  rex::ui::Window* window;
  std::unique_ptr<renderer::VulkanDevice> device;
  std::unique_ptr<renderer::OverlayRenderer> overlay_renderer;
  renderer::OverlayDrawer overlay_drawer;
  std::unique_ptr<rex::ui::ImGuiDrawer> drawer;
  std::unique_ptr<Menu> menu;
};

// Set when the player chooses PLAY; cleared by HandOffLauncher.
std::unique_ptr<LauncherSession> g_session;

// One setting changed by `direction` (-1 left, +1 right).
void ChangeSetting(LauncherSettings& s, int row, int direction) {
  switch (row) {
    case kResolution: s.uhd = !s.uhd; break;
    case kFrameRate: s.fps30 = !s.fps30; break;
    case kBrightness:
      s.brightness = std::clamp(s.brightness + direction * kBrightnessStep, kBrightnessMin,
                                kBrightnessMax);
      break;
    case kSmaa: s.smaa = !s.smaa; break;
    case kShadows: s.high_shadows = !s.high_shadows; break;
    case kIcons: s.xbox_icons = !s.xbox_icons; break;
    default: break;
  }
}

}  // namespace

LauncherSettings LoadLauncherSettings() {
  LauncherSettings s;
  std::ifstream in(kSettingsFile);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "resolution") s.uhd = value == "2160p";
    if (key == "frame_rate") s.fps30 = value == "30";
    if (key == "brightness") {
      int b = std::atoi(value.c_str());
      if (b >= kBrightnessMin && b <= kBrightnessMax) s.brightness = b;
    }
    if (key == "smaa") s.smaa = value != "off";
    if (key == "shadows") s.high_shadows = value != "normal";
    if (key == "button_icons") s.xbox_icons = value == "xbox";
  }
  return s;
}

void HandOffLauncher() {
  if (!g_session) return;
  g_session->HandOff();
  g_session.reset();
}

fs::path RunLauncher(rex::ui::Window* window, const fs::path& extracted,
                     const LauncherSettings& active, LauncherSettings& settings) {
  auto session = std::make_unique<LauncherSession>(window, active.width(), active.height());
  Menu& menu = *session->menu;
  menu.settings = settings;
  menu.active = active;
  auto paint = [&] { session->Paint(); };

  struct pad input {};
  pad_poll(&input);
  title_note("launcher: open (%s, %ux%u, %d fps)",
             ExtractedReady(extracted) ? "extracted game folder" : "disc image from dump/",
             active.width(), active.height(), active.fps30 ? 30 : 60);
  while (true) {
    paint();
    pad_poll(&input);
    const uint32_t p = input.pressed;
    if (menu.in_settings) {
      if (p & PAD_UP) menu.setting_row = (menu.setting_row + kSettingRows - 1) % kSettingRows;
      if (p & PAD_DOWN) menu.setting_row = (menu.setting_row + 1) % kSettingRows;
      const bool back = (p & PAD_CIRCLE) || (menu.setting_row == kBack && (p & PAD_CROSS));
      if (back) {
        menu.in_settings = false;
        menu.status = menu.settings.uhd != active.uhd
                          ? std::string("Close and reopen the game to use ") +
                                (menu.settings.uhd ? "4K." : "1440p.")
                          : std::string();
        continue;
      }
      int direction = (p & PAD_RIGHT) || (p & PAD_CROSS) ? 1 : (p & PAD_LEFT) ? -1 : 0;
      if (direction && menu.setting_row != kBack) {
        ChangeSetting(menu.settings, menu.setting_row, direction);
        menu.status = SaveSettings(menu.settings) ? "" : "Could not save the settings.";
        if (menu.setting_row == kResolution && menu.settings.uhd != active.uhd && menu.status.empty()) {
          menu.status = "Applies when the game is reopened.";
        }
      }
      continue;
    }
    if (p & PAD_UP) menu.main_row = (menu.main_row + kMainRows - 1) % kMainRows;
    if (p & PAD_DOWN) menu.main_row = (menu.main_row + 1) % kMainRows;
    if (p & PAD_CIRCLE) continue;
    if (!(p & PAD_CROSS)) continue;
    if (menu.main_row == kExit) {
      title_note("launcher: exit");
      settings = menu.settings;
      return {};
    }
    if (menu.main_row == kSettings) {
      menu.in_settings = true;
      menu.setting_row = kResolution;
      menu.status.clear();
      continue;
    }
    fs::path game;
    if (ExtractedReady(extracted)) {
      game = extracted;
    } else {
      menu.status = "Checking the disc...";
      paint();
      try {
        game = FindDisc();
      } catch (const std::exception& error) {
        menu.status = error.what();
        title_note("launcher: %s", error.what());
        pad_poll(&input);
        continue;
      }
    }
    title_note("launcher: playing from %s", game.c_str());
    menu.status = "Loading...";
    paint();
    settings = menu.settings;
    // The screen stays up until the game renderer takes it (HandOffLauncher).
    g_session = std::move(session);
    return game;
  }
}

}  // namespace tron::ps5
