#include "renderer.h"
#include "path_util.h"

#include "button_glyphs.h"
#include "draw_renderer.h"
#include "guest_gpu_state.h"
#include "mod_loader.h"
#include "png_dump.h"
#include "render_stats.h"
#include "pipeline_storage.h"
#include "shader_cache.h"
#include "smaa_pass.h"
#include "swizzle.h"
#include "texture_cache.h"
#include "thread_cpu.h"
#include "ui_overlay.h"
#include "vulkan_device.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Primitives/interface/DataBlob.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/Fence.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/PipelineStateCache.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <rex/graphics/command_processor.h>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(dante_precompile_threads, 0, "Diagnostics",
                     "renderer=dante: threads building the recorded shaders and pipelines at "
                     "launch (0 = all cores but one)")
    .range(0, 64);

REXCVAR_DEFINE_INT32(dante_debug_view, 0, "Diagnostics",
                     "renderer=dante: 0 presents the front buffer, N presents the N-th "
                     "color host render target (F11 cycles)")
    .range(0, 64);

REXCVAR_DEFINE_INT32(dante_resolution_scale, 1, "Graphics",
                     "renderer=dante: render targets at N times the game's resolution "
                     "(2: 1280x720 -> 2560x1440); read at startup")
    .range(1, 4);

REXCVAR_DEFINE_STRING(dante_resolution, "", "Graphics",
                      "renderer=dante: render resolution, 'WxH' (e.g. 1920x1080, 3440x1440) or "
                      "'auto' (window size). Wider than 16:9 sets the game's display aspect. "
                      "Overrides dante_resolution_scale; read at startup");

REXCVAR_DEFINE_INT32(dante_shadow_scale, 2, "Graphics",
                     "renderer=dante: shadow map resolution relative to the frame's (1 = "
                     "original, 2 or 4 = sharper shadows); read at startup")
    .range(1, 4);

REXCVAR_DEFINE_BOOL(dante_smaa, true, "Graphics",
                    "renderer=dante: SMAA anti-aliasing of the presented frame");

REXCVAR_DEFINE_BOOL(dante_depth_float, false, "Diagnostics",
                    "renderer=dante: 32-bit float depth for every depth buffer, as on GPUs "
                    "without 24-bit depth (AMD); read at startup");

REXCVAR_DEFINE_INT32(dante_replay_frames, 0, "Diagnostics",
                     "renderer=dante: keep the last N presented frames (640x360) for an "
                     "instant replay; 0 disables")
    .range(0, 1200);

REXCVAR_DEFINE_BOOL(dante_replay_dump, false, "Diagnostics",
                    "renderer=dante: write the instant replay to frame_dump/ (Home)");

REXCVAR_DEFINE_INT32(tron_present_gamma_x100, 119, "Graphics",
                     "renderer=dante: presentation gamma x100 (the launcher's brightness; "
                     "119 = 100%)");
REXCVAR_DEFINE_INT32(dante_record_frames, 0, "Diagnostics",
                     "renderer=dante: save the next N presented frames as PNG to "
                     "frame_dump/record/ (F5 starts / stops a 3600 frame recording)")
    .range(0, 1000000);

namespace renderer {
namespace {

using namespace guest;
namespace dl = Diligent;

// Surface object (docs/NATIVE_D3D_MAP.md): +0x18 RB_SURFACE_INFO value,
// +0x1C RB_COLOR_INFO / RB_DEPTH_INFO value, +0x34 size in EDRAM tiles.
struct SurfaceDesc {
  uint32_t base_tile = 0;
  uint32_t pitch = 0;
  uint32_t format = 0;
  bool depth = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Host render target identity: what the EDRAM region holds.
struct RenderTargetKey {
  uint32_t base_tile;
  uint32_t pitch;
  uint32_t format;
  bool depth;
  bool operator==(const RenderTargetKey&) const = default;
};
struct RenderTargetKeyHash {
  size_t operator()(const RenderTargetKey& k) const {
    return (size_t(k.base_tile) << 32) ^ (size_t(k.pitch) << 16) ^ (k.format << 1) ^ k.depth;
  }
};

// Host pixels per guest pixel of a target or resolve texture.
struct Scale {
  float x = 1.0f, y = 1.0f;
  bool operator==(const Scale&) const = default;
  uint32_t HostX(uint32_t size) const { return uint32_t(std::lround(double(size) * x)); }
  uint32_t HostY(uint32_t size) const { return uint32_t(std::lround(double(size) * y)); }
};

struct RenderTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
  Scale scale;
  bool guest_float_depth = false;                    // guest D24FS8
  dl::RefCntAutoPtr<dl::ITextureView> stencil_view;  // depth: stencil SRV, on demand
};

// Native stand-in for a guest texture written by Resolve; no copy goes back to
// guest memory. The game resolves different sizes/formats to the same address
// within a frame, so the key is the full texture description.
struct ResolveTargetKey {
  uint32_t address;
  uint32_t width;
  uint32_t height;
  uint32_t guest_format;
  // 0 for 2D destinations, 1 + slice for a slice of a 3D texture.
  uint32_t slice = 0;
  bool operator==(const ResolveTargetKey&) const = default;
};
struct ResolveTargetKeyHash {
  size_t operator()(const ResolveTargetKey& k) const {
    return (size_t(k.address) << 24) ^ (size_t(k.width) << 12) ^ k.height ^
           (size_t(k.guest_format) << 40) ^ (size_t(k.slice) << 48);
  }
};
struct ResolveTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t guest_format = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
  // Last resolve had RB_COPY_DEST_INFO.copy_dest_swap: guest memory holds
  // red and blue exchanged relative to the host texture.
  bool rb_swap = false;
  // Write-back to guest memory: staging copy, fence value it waits for (0 =
  // none pending) and the destination fetch constant.
  dl::RefCntAutoPtr<dl::ITexture> staging;
  uint64_t readback_fence = 0;
  uint32_t readback_fetch[6] = {};
  bool written_back = false;
  // Destination is slice `slice` of a 3D texture `volume_depth` deep (0: 2D).
  uint32_t volume_depth = 0, slice = 0;
  Scale scale;
  // Depth resolves (k_24_8) the game also samples as k_8_8_8_8: the guest
  // memory holds the raw D24S8 words, so those reads get this RGBA8 copy
  // (R = stencil, G / B / A = depth bits 0:7 / 8:15 / 16:23).
  bool depth_bits_wanted = false;
  dl::RefCntAutoPtr<dl::ITexture> depth_bits;
};

// Last non-zero value written to a register in (start, end], or ~0u (Resolve
// resets the copy registers to 0 after the copy).
uint32_t FindRegisterWrite(const uint8_t* base, uint32_t start, uint32_t end, uint32_t reg) {
  uint32_t value = ~0u;
  if (!IsPlausibleAddress(start) || !IsPlausibleAddress(end) || end <= start ||
      end - start > 0x40000)
    return value;
  for (uint32_t p = start + 4; p <= end;) {
    uint32_t header = Load32(base, p);
    uint32_t type = header >> 30;
    uint32_t count = type == 2 ? 0 : type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
    if (p + count * 4 > end) break;
    if (type == 0) {
      uint32_t first = header & 0x7FFF;
      bool one_reg = (header >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = Load32(base, p + 4 + i * 4);
        if ((one_reg ? first : first + i) == reg && v) value = v;
      }
    } else if (type == 3 && ((header >> 8) & 0x7F) == 0x2D && count >= 2) {
      // SET_CONSTANT, type 4 = registers from 0x2000.
      uint32_t d1 = Load32(base, p + 4);
      if (((d1 >> 16) & 0xFF) == 4) {
        uint32_t first = 0x2000 + (d1 & 0x7FF);
        for (uint32_t i = 1; i < count; ++i) {
          uint32_t v = Load32(base, p + 4 + i * 4);
          if (first + i - 1 == reg && v) value = v;
        }
      }
    }
    p += 4 + count * 4;
  }
  return value;
}

std::mutex g_mutex;
// Host render targets and resolve targets are round(guest size * scale).
float g_scale_x = 1.0f, g_scale_y = 1.0f;
// Render and resolve targets created since the last swap (replay notes).
uint32_t g_swap_new_targets = 0;

struct RenderScale {
  float x = 1.0f, y = 1.0f;
  double display_aspect = 16.0 / 9.0;
};

// Render scale from dante_resolution ('WxH' / 'auto') or dante_resolution_scale.
// The picture aspect is ultrawide_target_aspect, or the resolution's when 0;
// the picture fills the largest box of that aspect within the resolution.
// 4:3 (or narrower) uses the game's own standard-definition mode, chosen from
// a 640x480 video mode: UI laid out for 4:3, 3D still rendered into the 16:9
// 1280x720 targets and squeezed to 4:3 like the console's scaler. Wider than
// 16:9 sets the game's display aspect. Both are rendered anamorphically.
// Aspects between 4:3 and 16:9 stay 16:9, letterboxed.
RenderScale ComputeRenderScale(rex::ui::Window* window) {
  std::string mode = REXCVAR_GET(dante_resolution);
  uint32_t width = 0, height = 0;
  if (mode == "auto") {
    if (window) {
      width = window->GetActualPhysicalWidth();
      height = window->GetActualPhysicalHeight();
    }
  } else if (!mode.empty()) {
    std::sscanf(mode.c_str(), "%ux%u", &width, &height);
  }
  if (!mode.empty() && !(width && height)) {
    REXLOG_WARN("DanteDevice: dante_resolution '{}' not understood", mode);
  }
  RenderScale scale;
  double aspect = rex::cvar::Query<double>("ultrawide_target_aspect");
  if (aspect <= 0.0) aspect = width && height ? double(width) / height : 16.0 / 9.0;
  // Height of the picture's box within the resolution.
  auto box_height = [&](double box_aspect) {
    if (width && height) return float(std::min(double(height), width / box_aspect));
    return 720.0f * float(std::clamp(REXCVAR_GET(dante_resolution_scale), 1, 4));
  };
  if (aspect <= 4.0 / 3.0 + 0.01) {
    rex::cvar::SetFlagByName("video_mode_width", "640");
    rex::cvar::SetFlagByName("video_mode_height", "480");
    rex::cvar::SetFlagByName("ultrawide_target_aspect", "0");
    scale.y = std::max(1.0f, box_height(4.0 / 3.0) / 720.0f);
    scale.x = std::max(1.0f, scale.y * 0.75f);
    scale.display_aspect = 4.0 / 3.0;
  } else if (aspect > 16.0 / 9.0 + 0.01) {
    rex::cvar::SetFlagByName("ultrawide_target_aspect", fmt::format("{:.4f}", aspect));
    scale.y = std::max(1.0f, box_height(aspect) / 720.0f);
    scale.x = std::max(1.0f, float(720.0 * scale.y * aspect) / 1280.0f);
    scale.display_aspect = aspect;
  } else {
    rex::cvar::SetFlagByName("ultrawide_target_aspect", "0");
    scale.x = scale.y = std::max(1.0f, box_height(16.0 / 9.0) / 720.0f);
  }
  return scale;
}

std::unique_ptr<VulkanDevice> g_device;
// The launcher's device, taken over by Start instead of creating a second swap chain.
std::unique_ptr<VulkanDevice> g_adopted_device;
uint64_t g_frame = 0;
bool g_log_this_frame = false;
// First frame of an F8 capture: every resolve output and the front buffer are
// written to FrameDumpDirectory() as PNGs.
bool g_dump_this_frame = false;
uint32_t g_dump_seq = 0;
uint32_t g_logged_run_draws = 0;
RenderTargetKey g_log_draw_target{};
std::unordered_map<RenderTargetKey, RenderTarget, RenderTargetKeyHash> g_render_targets;
// Depth targets drawn without color targets (shadow maps): rendered at
// ShadowMapScale(), above the frame's resolution.
std::unordered_set<RenderTargetKey, RenderTargetKeyHash> g_shadow_maps;
float g_shadow_map_factor = 1.0f;

Scale ShadowMapScale() {
  float s = std::max(g_scale_x, g_scale_y) * g_shadow_map_factor;
  return {s, s};
}
std::unordered_map<ResolveTargetKey, ResolveTarget, ResolveTargetKeyHash> g_resolve_targets;
std::set<uint64_t> g_reported_conversions;
std::unique_ptr<ShaderCache> g_shader_cache;
// Recorded shaders and pipelines installed next to the exe.
constexpr char kShippedList[] = "dante_pipelines.bin";
std::unique_ptr<PipelineStorage> g_pipeline_storage;
std::filesystem::path g_startup_marker;
bool g_safe_start = false;
dl::RefCntAutoPtr<dl::IPipelineStateCache> g_pso_cache;
std::filesystem::path g_pso_cache_path;
uint64_t g_pso_cache_saved_pipelines = 0;
std::chrono::steady_clock::time_point g_pso_cache_saved_time;

// Building the stored shaders and pipelines (Start). The guest waits for it
// (RunWhenPrecompiled); draws wait too, in case it was not held back.
struct Precompile {
  std::thread thread;
  std::atomic<bool> cancel{false};
  std::atomic<bool> pipelines{false};
  std::atomic<uint32_t> shaders_done{0}, pipelines_done{0};
  uint32_t shaders_total = 0, pipelines_total = 0;
  std::chrono::steady_clock::time_point start;
  std::mutex mutex;
  std::condition_variable finished_cv;
  bool finished = true;
  std::function<void()> on_finished;
};
Precompile g_precompile;
std::unique_ptr<TextureCache> g_texture_cache;
std::unique_ptr<DrawRenderer> g_draw_renderer;
std::unique_ptr<SmaaPass> g_smaa;
std::unique_ptr<OverlayRenderer> g_overlay;
std::mutex g_present_mutex;
PresentCount g_present_count;
std::unique_ptr<ButtonGlyphs> g_button_glyphs;
std::unique_ptr<ModLoader> g_mod_loader;
std::function<GlyphStyle()> g_glyph_style_source;
std::function<KeyBindings()> g_glyph_bindings_source;
std::unique_ptr<rex::graphics::RegisterFile> g_register_file;
// Capture side (the game's render thread): register state for shader selection.
std::unique_ptr<rex::graphics::RegisterFile> g_capture_regs;
uint32_t g_draws_without_shaders = 0;
uint32_t g_draws_unparsed = 0;

// Last BeginVertices: drawn at EndVertices, or by the enclosing DrawVerticesUP.
struct InlineDraw {
  bool pending = false;
  CallArgs args{};
  uint32_t prim = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint32_t data = 0;
};
InlineDraw g_pending_inline_draw;

// Tron: BeginIndexedVertices (0x82BDEA70) writes a DRAW_INDX whose indices and
// vertex stream (fetch constant 95) point into the command buffer; the engine
// fills both and ends the draw inline (no EndVertices to hook).
struct IndexedInlineDraw {
  bool pending = false;
  uint32_t dev = 0;
  uint32_t initiator = 0, dma_base = 0, dma_size = 0;
  InlineDraw vertices;
};
IndexedInlineDraw g_pending_indexed_inline;
uint32_t g_indexed_inline_unparsed = 0;

// VGT_DRAW_INITIATOR of an inline draw: auto-indexed (source select 2).
uint32_t InlineDrawInitiator(const InlineDraw& draw) {
  return (draw.count << 16) | (2u << 6) | (draw.prim & 0x3F);
}

bool Is64bpp(uint32_t color_format) {
  return color_format == 5 || color_format == 7 || color_format == 15;
}

dl::TEXTURE_FORMAT ColorFormat(uint32_t format) {
  switch (format) {
    case 0:  // 8_8_8_8
    case 1:  // 8_8_8_8_GAMMA
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 2:   // 2_10_10_10
    case 10:  // 2_10_10_10_AS_10_10_10_10
      return dl::TEX_FORMAT_RGB10A2_UNORM;
    case 4:  // 16_16 (fixed -32..32)
    case 6:  // 16_16_FLOAT
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 3:   // 2_10_10_10_FLOAT
    case 5:   // 16_16_16_16 (fixed -32..32)
    case 7:   // 16_16_16_16_FLOAT
    case 12:  // 2_10_10_10_FLOAT_AS_16_16_16_16
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 14: return dl::TEX_FORMAT_R32_FLOAT;
    case 15: return dl::TEX_FORMAT_RG32_FLOAT;
    default: return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

// Guest D24S8 uses host D24S8 when the GPU can render to and sample it (AMD
// can't), otherwise D32S8 like D24FS8. The 2^24 depth bias factor suits both.
bool g_use_d24_depth = true;

dl::TEXTURE_FORMAT DepthFormat(uint32_t format) {
  return format == 1 || !g_use_d24_depth ? dl::TEX_FORMAT_D32_FLOAT_S8X24_UINT
                                         : dl::TEX_FORMAT_D24_UNORM_S8_UINT;
}

// Guest texture format (fetch constant dword1 & 0x3F) as a resolve destination.
dl::TEXTURE_FORMAT ResolveTargetFormat(uint32_t format) {
  switch (format) {
    case 2:  // k_8
      return dl::TEX_FORMAT_R8_UNORM;
    case 6:  // k_8_8_8_8
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16 (Tron: the main scene)
      return dl::TEX_FORMAT_RGB10A2_UNORM;
    case 27:  // k_16_EXPAND (float16, as the SDK)
    case 30:  // k_16_FLOAT
      return dl::TEX_FORMAT_R16_FLOAT;
    case 28:  // k_16_16_EXPAND
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 29:  // k_16_16_16_16_EXPAND (Tron: bloom)
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 22:  // k_24_8 (depth)
    case 23:  // k_24_8_FLOAT
    case 36:  // k_32_FLOAT
      return dl::TEX_FORMAT_R32_FLOAT;
    case 37:  // k_32_32_FLOAT
      return dl::TEX_FORMAT_RG32_FLOAT;
    default: return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

bool ReadSurface(const uint8_t* base, uint32_t surface, bool depth, SurfaceDesc& out) {
  if (!IsPlausibleAddress(surface)) return false;
  uint32_t surface_info = Load32(base, surface + 0x18);
  uint32_t info = Load32(base, surface + 0x1C);
  // Tron's surface objects keep the EDRAM size in bytes at +0x2C (as Dante's),
  // but +0x30/+0x34 hold other data, so the height comes from the byte size.
  uint32_t size_bytes = Load32(base, surface + 0x2C);
  out.depth = depth;
  out.base_tile = info & 0xFFF;
  out.format = (info >> 16) & 0xF;
  out.pitch = surface_info & 0x3FFF;
  if (!out.pitch) return false;
  uint32_t bytes_per_sample = (!depth && Is64bpp(out.format)) ? 8 : 4;
  out.width = out.pitch;
  out.height = size_bytes / ((out.pitch + 79) / 80 * 80 * bytes_per_sample);
  // EDRAM allocations are whole 80x16 tiles.
  out.height = std::min<uint32_t>(out.height & ~15u, 8192);
  if (!out.height) out.height = 16;
  return true;
}

// New textures start with undefined contents; guest memory starts zeroed.
void ClearNewTexture(dl::ITexture* texture, bool depth) {
  auto* context = g_device->Context();
  if (depth) {
    context->ClearDepthStencil(texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL),
                               dl::CLEAR_DEPTH_FLAG | dl::CLEAR_STENCIL_FLAG, 1.0f, 0,
                               dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else {
    const float zero[4] = {};
    context->ClearRenderTarget(texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET), zero,
                               dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
}

RenderTarget* GetRenderTarget(const SurfaceDesc& s) {
  RenderTargetKey key{s.base_tile, s.pitch, s.format, s.depth};
  Scale scale{g_scale_x, g_scale_y};
  if (g_shadow_maps.count(key)) {
    scale = ShadowMapScale();
    // Within the device's largest texture.
    float limit = 16384.0f / float(std::max(s.width, s.height));
    scale.x = scale.y = std::min(scale.x, limit);
  }
  RenderTarget& rt = g_render_targets[key];
  if (rt.texture && rt.width == s.width && rt.height >= s.height && rt.scale == scale) return &rt;
  dl::TextureDesc desc;
  desc.Name = s.depth ? "EdramDepth" : "EdramColor";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  uint32_t height = std::max(s.height, rt.height);
  desc.Width = scale.HostX(s.width);
  desc.Height = scale.HostY(height);
  desc.MipLevels = 1;
  desc.Format = s.depth ? DepthFormat(s.format) : ColorFormat(s.format);
  desc.BindFlags =
      (s.depth ? dl::BIND_DEPTH_STENCIL : dl::BIND_RENDER_TARGET) | dl::BIND_SHADER_RESOURCE;
  desc.Usage = dl::USAGE_DEFAULT;
  rt.texture.Release();
  g_device->RenderDevice()->CreateTexture(desc, nullptr, &rt.texture);
  if (!rt.texture) {
    REXLOG_ERROR("DanteDevice: failed to create host RT base={} pitch={} fmt={} depth={}",
                 s.base_tile, s.pitch, s.format, s.depth);
    g_render_targets.erase(key);
    return nullptr;
  }
  ClearNewTexture(rt.texture, s.depth);
  ++g_swap_new_targets;
  rt.width = s.width;
  rt.height = height;
  rt.format = desc.Format;
  rt.scale = scale;
  rt.guest_float_depth = s.depth && s.format == 1;
  REXLOG_INFO("NATIVE-RT new {} RT: edram base={} pitch={} fmt={} -> {}x{} (host {}x{})",
              s.depth ? "depth" : "color", s.base_tile, s.pitch, s.format, rt.width, rt.height,
              desc.Width, desc.Height);
  return &rt;
}

// `scale` is the scale of the render target being resolved.
ResolveTarget* GetResolveTarget(uint32_t address, uint32_t width, uint32_t height,
                                uint32_t guest_format, uint32_t slice, Scale scale) {
  ResolveTargetKey key{address, width, height, guest_format, slice};
  ResolveTarget& t = g_resolve_targets[key];
  if (t.texture && t.scale == scale) return &t;
  dl::TextureDesc desc;
  desc.Name = "ResolveTarget";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  desc.Width = scale.HostX(width);
  desc.Height = scale.HostY(height);
  desc.MipLevels = 1;
  desc.Format = ResolveTargetFormat(guest_format);
  desc.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_RENDER_TARGET;
  desc.Usage = dl::USAGE_DEFAULT;
  t.texture.Release();
  t.depth_bits.Release();
  g_device->RenderDevice()->CreateTexture(desc, nullptr, &t.texture);
  if (!t.texture) {
    REXLOG_ERROR("DanteDevice: failed to create resolve target {:08X} {}x{} fmt={}", address, width,
                 height, guest_format);
    g_resolve_targets.erase(key);
    return nullptr;
  }
  ClearNewTexture(t.texture, false);
  ++g_swap_new_targets;
  t.width = width;
  t.height = height;
  t.guest_format = guest_format;
  t.format = desc.Format;
  t.scale = scale;
  REXLOG_INFO("NATIVE-RT new resolve target {:08X} {}x{} guest fmt={}", address, width, height,
              guest_format);
  return &t;
}

// Full-screen passes that read a depth target texel by texel into a colour
// texture, offset by g_SourceOffset (source texel - destination pixel).
struct DepthPass {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  dl::RefCntAutoPtr<dl::IBuffer> params;
  bool failed = false;
};

bool EnsureDepthPass(DepthPass& d, const char* name, const char* pixel_source,
                     dl::TEXTURE_FORMAT format) {
  if (d.pso || d.failed) return d.pso;
  d.failed = true;
  d.pso.Attach(g_device->CreateFullscreenPipeline(name, pixel_source, format, true));
  if (!d.pso) return false;
  d.pso->CreateShaderResourceBinding(&d.srb, true);
  dl::BufferDesc params;
  params.Name = name;
  params.Size = 16;
  params.BindFlags = dl::BIND_UNIFORM_BUFFER;
  params.Usage = dl::USAGE_DYNAMIC;
  params.CPUAccessFlags = dl::CPU_ACCESS_WRITE;
  g_device->RenderDevice()->CreateBuffer(params, nullptr, &d.params);
  dl::SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = dl::FILTER_TYPE_POINT;
  dl::RefCntAutoPtr<dl::ISampler> sampler;
  g_device->RenderDevice()->CreateSampler(point, &sampler);
  if (!d.params || !sampler) return false;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Point")->Set(sampler);
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "CopyParams")->Set(d.params);
  d.failed = false;
  return true;
}

// Runs the pass on depth texels [x1, x2) x [y1, y2), written to (dst_x, dst_y)
// of `dest`. The caller has bound the depth inputs.
void RunDepthPass(DepthPass& d, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
                  uint32_t y2, uint32_t dst_x, uint32_t dst_y, int32_t param = 0,
                  int32_t param2 = 0) {
  auto* context = g_device->Context();
  {
    dl::MapHelper<int32_t> map(context, d.params, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    map[0] = int32_t(x1) - int32_t(dst_x);
    map[1] = int32_t(y1) - int32_t(dst_y);
    map[2] = param;
    map[3] = param2;
  }
  dl::ITextureView* rtv = dest->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &rtv, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const auto& dest_desc = dest->GetDesc();
  dl::Viewport vp;
  vp.Width = float(dest_desc.Width);
  vp.Height = float(dest_desc.Height);
  context->SetViewports(1, &vp, dest_desc.Width, dest_desc.Height);
  dl::Rect rect{int32_t(dst_x), int32_t(dst_y), int32_t(dst_x + (x2 - x1)),
                int32_t(dst_y + (y2 - y1))};
  context->SetScissorRects(1, &rect, dest_desc.Width, dest_desc.Height);
  context->SetPipelineState(d.pso);
  context->CommitShaderResources(d.srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

// Resolve of a depth surface into a sampled depth texture (k_24_8 /
// k_24_8_FLOAT -> R32_FLOAT, depth in red).
DepthPass g_depth_copy;

constexpr char kDepthCopyPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Depth;
layout(binding = 1) uniform sampler g_Point;
layout(std140, binding = 2) uniform CopyParams { ivec4 g_SourceOffset; };
layout(location = 0) out float out_depth;
void main() {
  ivec2 texel = ivec2(gl_FragCoord.xy) + g_SourceOffset.xy;
  out_depth = texelFetch(sampler2D(g_Depth, g_Point), texel, 0).r;
}
)";

void CopyDepth(RenderTarget& depth, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
               uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  DepthPass& d = g_depth_copy;
  if (!EnsureDepthPass(d, "DepthCopy", kDepthCopyPS, dl::TEX_FORMAT_R32_FLOAT)) return;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Depth")
      ->Set(depth.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  RunDepthPass(d, dest, x1, y1, x2, y2, dst_x, dst_y);
}

// Colour resolve with RB_COPY_DEST_INFO.copy_dest_exp_bias: the copy is scaled
// by 2^bias (Tron resolves its 7e3 HDR scene with bias -3). The values are
// first limited to the guest render target format's range (w: 1 = 7e3,
// 0..31.875; 2 = 16-bit fixed, -32..32): the host keeps those targets in
// 16-bit float, where stacked additive glows grew far past what the 360 can
// store and the bloom spread them as a bright haze.
constexpr char kColorScalePS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Color;
layout(binding = 1) uniform sampler g_Point;
layout(std140, binding = 2) uniform CopyParams { ivec4 g_SourceOffset; };
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 texel = ivec2(gl_FragCoord.xy) + g_SourceOffset.xy;
  vec4 c = texelFetch(sampler2D(g_Color, g_Point), texel, 0);
  if (g_SourceOffset.w == 1) c = clamp(c, vec4(0.0), vec4(31.875, 31.875, 31.875, 1.0));
  if (g_SourceOffset.w == 2) c = clamp(c, vec4(-32.0), vec4(32.0));
  out_color = c * exp2(float(g_SourceOffset.z));
}
)";

std::unordered_map<uint32_t, DepthPass> g_color_scale_passes;

bool CopyColorScaled(RenderTarget& rt, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
                     uint32_t y2, uint32_t dst_x, uint32_t dst_y, int32_t exp_bias,
                     int32_t clamp_mode = 0) {
  dl::TEXTURE_FORMAT format = dest->GetDesc().Format;
  DepthPass& d = g_color_scale_passes[uint32_t(format)];
  if (!EnsureDepthPass(d, "ColorScale", kColorScalePS, format)) return false;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Color")
      ->Set(rt.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  RunDepthPass(d, dest, x1, y1, x2, y2, dst_x, dst_y, exp_bias, clamp_mode);
  return true;
}

// Raw D24S8 words of a depth surface as RGBA8 (see ResolveTarget::depth_bits).
DepthPass g_depth_bits;

constexpr char kDepthBitsPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Depth;
layout(binding = 1) uniform utexture2D g_Stencil;
layout(binding = 2) uniform sampler g_Point;
layout(std140, binding = 3) uniform CopyParams { ivec4 g_SourceOffset; };
layout(location = 0) out vec4 out_bits;
void main() {
  ivec2 texel = ivec2(gl_FragCoord.xy) + g_SourceOffset.xy;
  float depth = texelFetch(sampler2D(g_Depth, g_Point), texel, 0).r;
  uint stencil = texelFetch(usampler2D(g_Stencil, g_Point), texel, 0).r;
  uint bits = uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5);
  out_bits = vec4(uvec4(stencil & 0xFFu, bits & 0xFFu, (bits >> 8) & 0xFFu, bits >> 16)) / 255.0;
}
)";

void CopyDepthBits(RenderTarget& depth, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
                   uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  DepthPass& d = g_depth_bits;
  if (!EnsureDepthPass(d, "DepthBits", kDepthBitsPS, dl::TEX_FORMAT_RGBA8_UNORM)) return;
  if (!depth.stencil_view) {
    dl::TextureViewDesc view;
    view.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
    view.Format = depth.format == dl::TEX_FORMAT_D24_UNORM_S8_UINT
                      ? dl::TEX_FORMAT_X24_TYPELESS_G8_UINT
                      : dl::TEX_FORMAT_X32_TYPELESS_G8X24_UINT;
    depth.texture->CreateView(view, &depth.stencil_view);
    if (!depth.stencil_view) return;
  }
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Depth")
      ->Set(depth.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Stencil")->Set(depth.stencil_view);
  RunDepthPass(d, dest, x1, y1, x2, y2, dst_x, dst_y);
}

void ClearColor(RenderTarget& rt, const float color[4]) {
  auto* rtv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  g_device->Context()->ClearRenderTarget(rtv, color, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void ClearDepth(RenderTarget& rt, bool depth, bool stencil, float z, uint8_t stencil_value) {
  dl::CLEAR_DEPTH_STENCIL_FLAGS flags = dl::CLEAR_DEPTH_FLAG_NONE;
  if (depth) flags |= dl::CLEAR_DEPTH_FLAG;
  if (stencil) flags |= dl::CLEAR_STENCIL_FLAG;
  auto* dsv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
  g_device->Context()->ClearDepthStencil(dsv, flags, z, stencil_value,
                                         dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

// Draw counts are logged when the next clear/resolve/swap or RT change ends a run.
void FlushDrawLog() {
  if (!g_log_this_frame || !g_logged_run_draws) return;
  REXLOG_INFO("NATIVE-RT f{} draws={} on RT0 base={} pitch={} fmt={}", g_frame, g_logged_run_draws,
              g_log_draw_target.base_tile, g_log_draw_target.pitch, g_log_draw_target.format);
  g_logged_run_draws = 0;
}

// Vertex shader microcode last loaded by a program flush (guest physical, dwords).
struct LoadedShader {
  uint32_t address = 0, dwords = 0;
};
LoadedShader g_loaded_vs;
// Pixel shader microcode of the last draw with a D3D pixel shader set.
GuestShaderCode g_last_ps;
// Index buffer of the last submitted draw (checked by the state verifier).
uint32_t g_last_index_base = 0, g_last_index_count = 0, g_last_index_format = 0;
bool g_in_program_flush = false;
// Draws and resolves since the last swap (frame recorder log).
uint32_t g_swap_draws = 0, g_swap_resolves = 0, g_swap_dest_info_missing = 0;
uint32_t g_flush_start = 0;

// Small resolves and resolves into 3D textures are written back to guest
// memory, one frame late: the game reads some on the CPU (64x8 k_32_FLOAT scene
// luminance -> exposure) and samples 3D ones (colour grading LUTs rendered
// slice by slice), which the texture cache loads from guest memory.
constexpr uint32_t kReadbackMaxTexels = 4096;
dl::RefCntAutoPtr<dl::IFence> g_readback_fence;
uint64_t g_readback_value = 0;

void QueueReadback(ResolveTarget& t, const uint8_t* base, uint32_t dest) {
  using rex::graphics::xenos::TextureFormat;
  if ((t.guest_format != uint32_t(TextureFormat::k_32_FLOAT) &&
       t.guest_format != uint32_t(TextureFormat::k_8_8_8_8)) ||
      (!t.volume_depth && t.width * t.height > kReadbackMaxTexels) || t.readback_fence) {
    return;
  }
  auto* device = g_device->RenderDevice();
  if (!g_readback_fence) {
    dl::FenceDesc desc;
    desc.Name = "ResolveReadback";
    device->CreateFence(desc, &g_readback_fence);
    if (!g_readback_fence) return;
  }
  if (!t.staging) {
    dl::TextureDesc desc;
    desc.Name = "ResolveReadback";
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = t.scale.HostX(t.width);
    desc.Height = t.scale.HostY(t.height);
    desc.Format = t.format;
    desc.Usage = dl::USAGE_STAGING;
    desc.CPUAccessFlags = dl::CPU_ACCESS_READ;
    device->CreateTexture(desc, nullptr, &t.staging);
    if (!t.staging) return;
  }
  auto* context = g_device->Context();
  dl::CopyTextureAttribs copy(t.texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, t.staging,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyTexture(copy);
  context->EnqueueSignal(g_readback_fence, ++g_readback_value);
  t.readback_fence = g_readback_value;
  for (uint32_t i = 0; i < 6; ++i) t.readback_fetch[i] = Load32(base, dest + 0x1C + i * 4);
}

// Copies finished readbacks into guest memory (tiling and endianness of the
// destination fetch constant).
void FinishReadbacks(const uint8_t* base) {
  if (!g_readback_fence) return;
  uint64_t completed = g_readback_fence->GetCompletedValue();
  auto* context = g_device->Context();
  uint8_t* memory = const_cast<uint8_t*>(PhysicalPtr(base, 0));
  for (auto& [key, t] : g_resolve_targets) {
    if (!t.readback_fence || t.readback_fence > completed) continue;
    t.readback_fence = 0;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, t.readback_fetch, sizeof(fetch));
    uint32_t pitch = std::max(uint32_t(fetch.pitch) << 5, t.width);
    dl::MappedTextureSubresource mapped;
    context->MapTextureSubresource(t.staging, 0, 0, dl::MAP_READ, dl::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                   mapped);
    if (!mapped.pData) continue;
    // Scaled targets: the centre texel of each guest texel.
    for (uint32_t y = 0; y < t.height; ++y) {
      const auto* row = reinterpret_cast<const uint32_t*>(
          static_cast<const uint8_t*>(mapped.pData) +
          size_t(std::min(uint32_t((y + 0.5f) * t.scale.y), t.scale.HostY(t.height) - 1)) *
              mapped.Stride);
      for (uint32_t x = 0; x < t.width; ++x) {
        uint32_t offset;
        if (t.volume_depth) {
          offset = fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset3D(
                                     int32_t(x), int32_t(y), int32_t(t.slice), pitch, t.height, 2))
                               : ((t.slice * ((t.height + 31) & ~31u) + y) * pitch + x) * 4;
        } else {
          offset = fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                                     int32_t(x), int32_t(y), pitch, 2))
                               : (y * pitch + x) * 4;
        }
        uint32_t texel =
            row[std::min(uint32_t((x + 0.5f) * t.scale.x), t.scale.HostX(t.width) - 1)];
        if (t.rb_swap)
          texel = (texel & 0xFF00FF00u) | ((texel >> 16) & 0xFF) | ((texel & 0xFF) << 16);
        uint32_t value = rex::graphics::xenos::GpuSwap(texel, fetch.endianness);
        std::memcpy(memory + key.address + offset, &value, 4);
      }
    }
    context->UnmapTextureSubresource(t.staging, 0, 0);
    if (!t.written_back) {
      t.written_back = true;
      REXLOG_INFO(
          "NATIVE-RT resolve {:08X} {}x{} written back to guest memory (tiled={} endian={})",
          key.address, t.width, t.height, bool(fetch.tiled), uint32_t(fetch.endianness));
    }
  }
}

// Parses the PM4 packets written in (start, end] (end = last dword written).
void ScanProgramFlush(const uint8_t* base, uint32_t start, uint32_t end) {
  if (!IsPlausibleAddress(start) || !IsPlausibleAddress(end) || end <= start ||
      end - start > 0x40000)
    return;
  uint32_t p = start + 4;
  LoadedShader found;
  while (p <= end) {
    uint32_t header = Load32(base, p);
    uint32_t type = header >> 30;
    uint32_t count = type == 2 ? 0 : type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
    if (p + count * 4 > end) return;
    if (type == 3) {
      uint32_t opcode = (header >> 8) & 0x7F;
      if (opcode == 0x27 && count >= 2) {  // IM_LOAD: [addr | type] [start << 16 | size]
        uint32_t d1 = Load32(base, p + 4), d2 = Load32(base, p + 8);
        if ((d1 & 3) == 0) found = {d1 & ~3u, d2 & 0xFFFF};
      } else if (opcode == 0x2B && count >= 2) {  // IM_LOAD_IMMEDIATE: [type] [size] ucode
        uint32_t d1 = Load32(base, p + 4), d2 = Load32(base, p + 8);
        if ((d1 & 3) == 0) found = {GpuAddress(p + 12), d2 & 0xFFFF};
      }
    }
    p += 4 + count * 4;
  }
  if (found.dwords) g_loaded_vs = found;
}

}  // namespace

uint32_t CommandBufferWritePtr(uint32_t dev, const uint8_t* base) {
  return IsPlausibleAddress(dev) ? Load32(base, dev + kDevCommandBufferWritePtr) : 0;
}

void BeforeProgramFlush(uint32_t dev, const uint8_t* base) {
  if (!g_device) return;
  g_in_program_flush = true;
  g_flush_start = Load32(base, dev + kDevCommandBufferWritePtr);
}

namespace {
struct SegmentSwitch {
  uint32_t old_ptr = 0, new_ptr = 0;
};
constexpr uint32_t kSegmentSwitchHistory = 64;
SegmentSwitch g_segment_switches[kSegmentSwitchHistory];
uint32_t g_segment_switch_count = 0;
}  // namespace

void RecordSegmentSwitch(uint32_t old_ptr, uint32_t new_ptr) {
  g_segment_switches[g_segment_switch_count % kSegmentSwitchHistory] = {old_ptr, new_ptr};
  ++g_segment_switch_count;
}

uint32_t SegmentSwitchCount() { return g_segment_switch_count; }

void AfterCommandBufferReserve(uint32_t write_ptr) {
  if (g_in_program_flush) g_flush_start = write_ptr;
}

void AfterProgramFlush(uint32_t dev, const uint8_t* base) {
  if (!g_in_program_flush) return;
  g_in_program_flush = false;
  ScanProgramFlush(base, g_flush_start, Load32(base, dev + kDevCommandBufferWritePtr));
}

namespace {

// The VS microcode the GPU runs is the one the last program flush loaded (D3D
// patches the fetches per declaration, see docs/NATIVE_D3D_MAP.md). Before the
// first flush: variant v bound to the declaration whose id (decl+48) is in
// VS[416*v + 40], ucode VS[+0x20] + blk[+872] with blk = VS + VS[+896 + 8*v].
GuestShaderCode VertexShaderCode(const uint8_t* base, uint32_t dev) {
  if (g_loaded_vs.dwords && g_loaded_vs.dwords <= 0x4000) {
    return {reinterpret_cast<const uint32_t*>(PhysicalPtr(base, g_loaded_vs.address)),
            g_loaded_vs.dwords};
  }
  uint32_t vs = Load32(base, dev + kDevVertexShader);
  uint32_t decl = Load32(base, dev + kDevVertexDecl);
  if (!IsPlausibleAddress(vs)) return {};
  uint32_t decl_id = IsPlausibleAddress(decl) ? Load32(base, decl + 48) : 0;
  uint32_t variant = 0;
  for (uint32_t v = 0; v < 2; ++v) {
    if (Load32(base, vs + 896 + 8 * v) && Load32(base, vs + 416 * v + 40) == decl_id) {
      variant = v;
      break;
    }
  }
  uint32_t blk = vs + Load32(base, vs + 896 + 8 * variant);
  uint32_t ucode = Load32(base, vs + 0x20) + Load32(base, blk + 872);
  uint32_t size = Load32(base, blk + 876);
  if (!IsPlausibleAddress(ucode) || !size || size > 0x10000) return {};
  return {reinterpret_cast<const uint32_t*>(guest::Ptr(base, ucode)), size / 4};
}

// Pixel shader: ucode = PS[+0x18] + blk[+40], size blk[+44], blk = PS + PS[+0x40].
// With a null D3D pixel shader nothing is loaded, so in color + depth mode the
// GPU still runs the previous one (the SDK backends use the active shader).
GuestShaderCode PixelShaderCode(const uint8_t* base, uint32_t dev) {
  if (g_capture_regs && g_capture_regs->Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode !=
                             rex::graphics::xenos::EdramMode::kColorDepth) {
    return {};
  }
  uint32_t ps = Load32(base, dev + kDevPixelShader);
  if (!IsPlausibleAddress(ps)) return g_last_ps;
  uint32_t blk = ps + Load32(base, ps + 0x40);
  uint32_t ucode = Load32(base, ps + 0x18) + Load32(base, blk + 40);
  uint32_t size = Load32(base, blk + 44);
  if (!IsPlausibleAddress(ucode) || !size || size > 0x10000) return {};
  g_last_ps = {reinterpret_cast<const uint32_t*>(guest::Ptr(base, ucode)), size / 4};
  return g_last_ps;
}

// The draw packet ends what the D3D call wrote to the command buffer (dev+48
// is its last dword). PM4 type-3 DRAW_INDX (0x22): [hdr][viz][initiator][dma
// base][dma size] when indexed, [hdr][viz][initiator] otherwise; DRAW_INDX_2
// (0x36): [hdr][initiator].
bool ParseDrawPacket(const uint8_t* base, uint32_t dev, uint32_t& initiator, uint32_t& dma_base,
                     uint32_t& dma_size) {
  uint32_t last = Load32(base, dev + kDevCommandBufferWritePtr);
  if (!IsPlausibleAddress(last)) return false;
  auto opcode = [&](uint32_t address) {
    uint32_t header = Load32(base, address);
    return (header >> 30) == 3 ? (header >> 8) & 0x7F : 0xFFu;
  };
  if (opcode(last - 16) == 0x22 && ((Load32(base, last - 8) >> 6) & 3) == 0) {
    initiator = Load32(base, last - 8);
    dma_base = Load32(base, last - 4);
    dma_size = Load32(base, last);
    return true;
  }
  if (opcode(last - 8) == 0x22 || opcode(last - 4) == 0x36) {
    initiator = Load32(base, last);
    dma_base = dma_size = 0;
    return true;
  }
  return false;
}

// Host render targets for the bound surfaces (RT0..3 up to the first gap, depth).
DrawTargets BoundTargets(const uint8_t* base, uint32_t dev) {
  DrawTargets t;
  t.scale_x = g_scale_x;
  t.scale_y = g_scale_y;
  for (uint32_t i = 0; i < 4; ++i) {
    SurfaceDesc s;
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, s)) break;
    RenderTarget* rt = GetRenderTarget(s);
    if (!rt) break;
    t.color[i] = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
    t.color_format[i] = rt->format;
    t.guest_color_format[i] = s.format;
    t.color_count = i + 1;
    if (!t.width) {
      t.width = rt->width;
      t.height = rt->height;
    }
  }
  SurfaceDesc ds;
  if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, ds)) {
    RenderTargetKey key{ds.base_tile, ds.pitch, ds.format, true};
    t.shadow_map = !t.color_count && ds.width == ds.height;
    if (t.shadow_map && g_shadow_map_factor != 1.0f && g_shadow_maps.insert(key).second) {
      REXLOG_INFO("NATIVE-RT shadow map: depth base={} pitch={} {}x{}", ds.base_tile, ds.pitch,
                  ds.width, ds.height);
    }
    if (RenderTarget* rt = GetRenderTarget(ds)) {
      if (!t.color_count) {
        t.scale_x = rt->scale.x;
        t.scale_y = rt->scale.y;
      } else if (!(rt->scale == Scale{g_scale_x, g_scale_y})) {
        static bool reported = false;
        if (!reported) {
          reported = true;
          REXLOG_WARN("NATIVE-RT shadow map base={} used with color targets", ds.base_tile);
        }
      }
      t.depth = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
      t.depth_format = rt->format;
      t.guest_depth_format = ds.format;
      if (!t.width) {
        t.width = rt->width;
        t.height = rt->height;
      }
    }
  }
  return t;
}

// Draws natively what the D3D call just submitted.
void WaitForPrecompile() {
  std::unique_lock lock(g_precompile.mutex);
  g_precompile.finished_cv.wait(lock, [] { return g_precompile.finished; });
}

// ---------------------------------------------------------------------------
// Render worker. The game's render thread only captures what can change under
// it (register state, shader code, surface objects, call arguments) and queues
// a job; this thread does the uploads and all Vulkan work. Guest memory the
// worker reads (vertex, index and texture data, the command buffer) is
// protected the way the Xbox 360 GPU's reads are: the command processor waits
// for the worker before it writes a fence (g_cp_memory_write_hook), so the
// game cannot reuse that memory before the worker has read it.

struct DrawJob {
  GuestRegisters state;  // float constants: see `constants`
  ConstantDelta constants;
  uint32_t initiator = 0, dma_base = 0, dma_size = 0;
  bool has_inline = false;
  InlineDraw inline_draw;
  TranslatedShaders shaders;
  SurfaceDesc rts[4];
  uint32_t rt_count = 0;
  SurfaceDesc ds;
  bool has_ds = false;
};

struct Job {
  bool is_draw = false;
  uint32_t next_fence = 0;  // device's next fence value when queued
  std::function<void()> fn;
  DrawJob draw;
};

constexpr uint64_t kJobRing = 4096;  // ~2 frames of draws in busy scenes
std::unique_ptr<Job[]> g_jobs;
uint64_t g_job_head = 0, g_job_tail = 0;
std::mutex g_job_mutex;
std::condition_variable g_job_cv, g_job_space_cv, g_job_idle_cv;
bool g_worker_stop = false;
std::thread g_worker;
// Waiters (guarded by g_job_mutex): wake them only when they can proceed.
bool g_cp_waiting = false;
uint32_t g_cp_wait_value = 0;
bool g_producer_waiting = false;
const uint8_t* g_worker_base = nullptr;
uint32_t g_job_device = 0;  // D3D device (render thread)
constexpr uint32_t kDevNextFence = 10908;

void ExecuteDraw(const DrawJob& d, const uint8_t* base);

// Where a slow frame's time went (PERF slow frame lines). The timestamp
// counter, as steady_clock::now() is a system call on the PS5.
double TicksPerMs() {
  static const double ticks_per_ms = [] {
    auto c0 = std::chrono::steady_clock::now();
    uint64_t t0 = __builtin_ia32_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t t1 = __builtin_ia32_rdtsc();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    return double(t1 - t0) / ms;
  }();
  return ticks_per_ms;
}
inline uint64_t Ticks() { return __builtin_ia32_rdtsc(); }
struct FrameCosts {
  std::atomic<uint64_t> worker_busy{0};   // worker executing jobs
  std::atomic<uint64_t> draws{0};         // of which draws
  std::atomic<uint64_t> present{0};       // of which presenting (incl. waiting for the GPU)
  std::atomic<uint64_t> cp_wait{0};       // command processor waiting for the worker
  std::atomic<uint64_t> ring_full{0};     // render thread waiting for ring space
  std::atomic<uint64_t> guest_frame{0};   // render thread: last guest frame (swap to swap)
  std::atomic<uint64_t> guest_frame_max{0};
};
FrameCosts g_costs;
}  // namespace
extern std::atomic<uint64_t> g_tex_load_ticks, g_tex_store_ticks;
namespace {
constexpr uint32_t kDrawsPerFlush = 384;

void WorkerMain() {
  for (;;) {
    Job* job;
    {
      std::unique_lock lock(g_job_mutex);
      g_job_cv.wait(lock, [] { return g_worker_stop || g_job_head != g_job_tail; });
      if (g_job_head == g_job_tail) return;
      job = &g_jobs[g_job_tail % kJobRing];
    }
    {
      const uint64_t t0 = Ticks();
      std::lock_guard lock(g_mutex);
      if (job->is_draw) {
        ExecuteDraw(job->draw, g_worker_base);
        // Hand recorded work to the GPU as the frame goes, so it runs while
        // the rest is recorded instead of all after the swap.
        static uint32_t since_flush = 0;
        if (++since_flush >= kDrawsPerFlush && g_device) {
          since_flush = 0;
          g_device->Context()->Flush();
          // A flush drops the committed resource bindings.
          if (g_draw_renderer) g_draw_renderer->InvalidateBindings();
        }
        g_costs.draws.fetch_add(Ticks() - t0, std::memory_order_relaxed);
      } else if (job->fn) {
        job->fn();
      }
      g_costs.worker_busy.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    }
    job->fn = nullptr;
    bool wake_cp = false, wake_producer = false;
    {
      std::lock_guard lock(g_job_mutex);
      ++g_job_tail;
      if (g_cp_waiting) {
        wake_cp = g_job_head == g_job_tail ||
                  int32_t(g_jobs[g_job_tail % kJobRing].next_fence - g_cp_wait_value) > 0;
      }
      wake_producer = g_producer_waiting;
    }
    if (wake_producer) g_job_space_cv.notify_one();
    if (wake_cp) g_job_idle_cv.notify_all();
  }
}

// Command processor, before it writes fence `value`: waits for the jobs queued
// before D3D inserted that fence (their next-fence value is not past it).
void WaitWorkerFence(uint32_t value) {
  if (!g_jobs) return;
  std::unique_lock lock(g_job_mutex);
  auto done = [value] {
    if (g_job_head == g_job_tail || g_worker_stop) return true;
    return int32_t(g_jobs[g_job_tail % kJobRing].next_fence - value) > 0;
  };
  if (done()) return;
  g_cp_waiting = true;
  g_cp_wait_value = value;
  const uint64_t t0 = Ticks();
  g_job_idle_cv.wait(lock, done);
  g_costs.cp_wait.fetch_add(Ticks() - t0, std::memory_order_relaxed);
  g_cp_waiting = false;
}

Job& AcquireJob() {
  std::unique_lock lock(g_job_mutex);
  if (g_job_head - g_job_tail >= kJobRing) {
    g_producer_waiting = true;
    const uint64_t t0 = Ticks();
    g_job_space_cv.wait(lock, [] { return g_job_head - g_job_tail < kJobRing; });
    g_costs.ring_full.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    g_producer_waiting = false;
  }
  return g_jobs[g_job_head % kJobRing];
}

void PublishJob() {
  if (g_job_device && g_worker_base) {
    g_jobs[g_job_head % kJobRing].next_fence = Load32(g_worker_base, g_job_device + kDevNextFence);
  }
  {
    std::lock_guard lock(g_job_mutex);
    ++g_job_head;
  }
  g_job_cv.notify_one();
}

void EnqueueFn(std::function<void()> fn) {
  Job& job = AcquireJob();
  job.is_draw = false;
  job.fn = std::move(fn);
  PublishJob();
}

void StartWorker() {
  if (g_jobs) return;
  g_jobs.reset(new Job[kJobRing]);
  g_worker_stop = false;
  g_worker = std::thread(WorkerMain);
  rex::graphics::g_cp_fence_hook = [](uint32_t value) { WaitWorkerFence(value); };
}

void StopWorker() {
  if (!g_jobs) return;
  rex::graphics::g_cp_fence_hook = nullptr;
  {
    std::lock_guard lock(g_job_mutex);
    g_worker_stop = true;
  }
  g_job_cv.notify_all();
  g_job_idle_cv.notify_all();
  if (g_worker.joinable()) g_worker.join();
}

// Host render targets for captured surfaces (RT0..3 up to the first gap, depth).
DrawTargets TargetsFromSurfaces(const DrawJob& d) {
  DrawTargets t;
  t.scale_x = g_scale_x;
  t.scale_y = g_scale_y;
  for (uint32_t i = 0; i < d.rt_count; ++i) {
    const SurfaceDesc& s = d.rts[i];
    RenderTarget* rt = GetRenderTarget(s);
    if (!rt) break;
    t.color[i] = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
    t.color_format[i] = rt->format;
    t.guest_color_format[i] = s.format;
    t.color_count = i + 1;
    if (!t.width) {
      t.width = rt->width;
      t.height = rt->height;
    }
  }
  if (d.has_ds) {
    const SurfaceDesc& ds = d.ds;
    RenderTargetKey key{ds.base_tile, ds.pitch, ds.format, true};
    t.shadow_map = !t.color_count && ds.width == ds.height;
    if (t.shadow_map && g_shadow_map_factor != 1.0f && g_shadow_maps.insert(key).second) {
      REXLOG_INFO("NATIVE-RT shadow map: depth base={} pitch={} {}x{}", ds.base_tile, ds.pitch,
                  ds.width, ds.height);
    }
    if (RenderTarget* rt = GetRenderTarget(ds)) {
      if (!t.color_count) {
        t.scale_x = rt->scale.x;
        t.scale_y = rt->scale.y;
      }
      t.depth = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
      t.depth_format = rt->format;
      t.guest_depth_format = ds.format;
      if (!t.width) {
        t.width = rt->width;
        t.height = rt->height;
      }
    }
  }
  return t;
}

void FillRegisters(rex::graphics::RegisterFile& regs, const GuestRegisters& state,
                   uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                   const InlineDraw* inline_draw, bool float_constants = true) {
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    // Shader selection (capture side) does not read the float constants.
    bool constants = range.reg >= 0x4000 && range.reg < 0x4800;
    if (float_constants || !constants) {
      std::memcpy(&regs.values[range.reg], &state[index], range.count * 4);
    }
    index += range.count;
  }
  regs.values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator;
  regs.values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] = dma_base;
  regs.values[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE] = dma_size;
  if (inline_draw) {
    // Stream 0 = vertex fetch constant 95, written to the ring, not shadowed.
    uint32_t words = (inline_draw->count * inline_draw->stride + 3) / 4;
    uint32_t fetch = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2;
    regs.values[fetch] = GpuAddress(inline_draw->data) | 3;
    regs.values[fetch + 1] = (words << 2) | 2;  // k8in32
  }
}

// Worker: the draw itself.
void ExecuteDraw(const DrawJob& d, const uint8_t* base) {
  if (!g_device || !g_draw_renderer) return;
  ++g_swap_draws;
  FillRegisters(*g_register_file, d.state, d.initiator, d.dma_base, d.dma_size,
                d.has_inline ? &d.inline_draw : nullptr, false);
  for (uint32_t i = 0; i < d.constants.count; ++i) {
    std::memcpy(&g_register_file->values[0x4000 + d.constants.index[i] * 4],
                d.constants.values[i], 16);
  }
  {
    rex::graphics::reg::VGT_DRAW_INITIATOR init;
    init.value = d.initiator;
    bool indexed = init.source_select == rex::graphics::xenos::SourceSelect::kDMA;
    uint32_t index_bytes = init.index_size == rex::graphics::xenos::IndexFormat::kInt32 ? 4 : 2;
    g_last_index_base = indexed ? d.dma_base & ~(index_bytes - 1) : 0;
    g_last_index_count = indexed ? init.num_indices : 0;
    g_last_index_format = uint32_t(init.index_size);
  }
  g_draw_renderer->Draw(*g_register_file, d.shaders, TargetsFromSurfaces(d), base,
                        g_log_this_frame, d.has_inline ? 95u : ~0u);
}

// Render thread: capture a draw and queue it.
void SubmitDraw(uint32_t dev, uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                const InlineDraw* inline_draw, const uint8_t* base) {
  if (!g_device || !g_shader_cache || !g_draw_renderer) return;
  WaitForPrecompile();
  g_worker_base = base;
  g_job_device = dev;
  Job& job = AcquireJob();
  DrawJob& d = job.draw;
  job.is_draw = true;
  job.fn = nullptr;
  CaptureDrawStateDelta(dev, base, d.state);
  d.initiator = initiator;
  d.dma_base = dma_base;
  d.dma_size = dma_size;
  d.has_inline = inline_draw != nullptr;
  if (inline_draw) d.inline_draw = *inline_draw;
  FillRegisters(*g_capture_regs, d.state, initiator, dma_base, dma_size, inline_draw, false);
  if (!g_shader_cache->Prepare(*g_capture_regs, VertexShaderCode(base, dev),
                               PixelShaderCode(base, dev), d.shaders)) {
    ++g_draws_without_shaders;
    return;  // the slot is not published
  }
  d.rt_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, d.rts[i])) break;
    d.rt_count = i + 1;
  }
  d.has_ds = ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, d.ds);
  TakeConstantDelta(d.constants);
  PublishJob();
}

// Instant replay: the last presented frames, downscaled, kept on the GPU; Home
// writes them with per-frame notes to frame_dump/replay_<frame>/.
constexpr uint32_t kReplayWidth = 640, kReplayHeight = 360;
constexpr char kReplayPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Source;
layout(binding = 1) uniform sampler g_Linear;
layout(location = 0) out vec4 out_color;
void main() {
  out_color = texture(sampler2D(g_Source, g_Linear), gl_FragCoord.xy / vec2(640.0, 360.0));
}
)";

struct Replay {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  std::vector<dl::RefCntAutoPtr<dl::ITexture>> frames;
  std::vector<std::string> notes;
  std::vector<bool> has_image;
  uint32_t next = 0, count = 0;
  bool failed = false;
};
Replay g_replay;

bool EnsureReplay(uint32_t capacity) {
  Replay& r = g_replay;
  if (r.failed) return false;
  if (!r.pso) {
    r.failed = true;
    r.pso.Attach(
        g_device->CreateFullscreenPipeline("Replay", kReplayPS, dl::TEX_FORMAT_RGBA8_UNORM));
    if (!r.pso) return false;
    r.pso->CreateShaderResourceBinding(&r.srb, true);
    dl::SamplerDesc linear;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = dl::FILTER_TYPE_LINEAR;
    dl::RefCntAutoPtr<dl::ISampler> sampler;
    g_device->RenderDevice()->CreateSampler(linear, &sampler);
    if (!sampler) return false;
    r.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Linear")->Set(sampler);
    r.failed = false;
  }
  if (r.frames.size() != capacity) {
    r.frames.assign(capacity, {});
    r.notes.assign(capacity, {});
    r.has_image.assign(capacity, false);
    r.next = r.count = 0;
  }
  return true;
}

// Keeps the presented image (view: what the swap chain shows) and its notes.
void ReplayCapture(dl::ITextureView* view, std::string note) {
  int32_t capacity = REXCVAR_GET(dante_replay_frames);
  if (capacity <= 0 || !EnsureReplay(uint32_t(capacity))) return;
  Replay& r = g_replay;
  uint32_t slot = r.next;
  r.next = (r.next + 1) % uint32_t(capacity);
  r.count = std::min(r.count + 1, uint32_t(capacity));
  r.notes[slot] = std::move(note);
  r.has_image[slot] = view != nullptr;
  if (!view) return;
  auto& texture = r.frames[slot];
  if (!texture) {
    dl::TextureDesc desc;
    desc.Name = "ReplayFrame";
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = kReplayWidth;
    desc.Height = kReplayHeight;
    desc.Format = dl::TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = dl::BIND_RENDER_TARGET | dl::BIND_SHADER_RESOURCE;
    g_device->RenderDevice()->CreateTexture(desc, nullptr, &texture);
    if (!texture) {
      r.has_image[slot] = false;
      return;
    }
  }
  auto* context = g_device->Context();
  dl::ITextureView* rtv = texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &rtv, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::Viewport vp;
  vp.Width = float(kReplayWidth);
  vp.Height = float(kReplayHeight);
  context->SetViewports(1, &vp, kReplayWidth, kReplayHeight);
  r.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Source")->Set(view);
  context->SetPipelineState(r.pso);
  context->CommitShaderResources(r.srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_NONE);
}

void ReplayDump() {
  Replay& r = g_replay;
  if (!r.count) {
    REXLOG_WARN("NATIVE-REPLAY: nothing recorded (dante_replay_frames=0?)");
    return;
  }
  auto dir = FrameDumpDirectory() / fmt::format("replay_f{}", g_frame);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string index;
  uint32_t capacity = uint32_t(r.frames.size());
  uint32_t first = (r.next + capacity - r.count) % capacity;
  for (uint32_t i = 0; i < r.count; ++i) {
    uint32_t slot = (first + i) % capacity;
    index += fmt::format("{:03} {}\n", i, r.notes[slot]);
    if (r.has_image[slot] && r.frames[slot]) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), r.frames[slot],
                     dir / fmt::format("{:03}.png", i));
    }
  }
  if (std::FILE* f = path_util::Open(dir / "index.txt", "wb")) {
    std::fwrite(index.data(), 1, index.size(), f);
    std::fclose(f);
  }
  REXLOG_INFO("NATIVE-REPLAY: {} frames written to {}", r.count, path_util::Utf8(dir));
}

// Performance log: a summary every 30 s and a line per slow frame, with the
// shaders and pipelines compiled in it (stutter vs GPU load).
constexpr double kPerfWindowSeconds = 30.0;
constexpr float kSlowFrameMs = 25.0f;
constexpr uint32_t kSlowFramesLoggedPerWindow = 60;

struct PerfWindow {
  std::chrono::steady_clock::time_point start{}, last_swap{};
  std::vector<float> frame_ms;
  uint32_t slow_frames = 0;
  RenderStats at_start{}, at_last_swap{};
};
PerfWindow g_perf;

void PerfOnSwap(uint32_t draws, uint32_t resolves) {
  using Clock = std::chrono::steady_clock;
  PerfWindow& p = g_perf;
  const RenderStats& stats = g_render_stats;
  Clock::time_point now = Clock::now();
  if (p.start == Clock::time_point{}) {
    p.start = p.last_swap = now;
    p.at_start = p.at_last_swap = stats;
    return;
  }
  float ms = std::chrono::duration<float, std::milli>(now - p.last_swap).count();
  p.last_swap = now;
  p.frame_ms.push_back(ms);
  uint64_t shaders = stats.shaders_compiled - p.at_last_swap.shaders_compiled;
  uint64_t pipelines = stats.pipelines_created - p.at_last_swap.pipelines_created;
  p.at_last_swap = stats;
  {
    const double tpm = TicksPerMs();
    const double busy = double(g_costs.worker_busy.exchange(0)) / tpm;
    const double draw_ms = double(g_costs.draws.exchange(0)) / tpm;
    const double present_ms = double(g_costs.present.exchange(0)) / tpm;
    const double tex_load_ms = double(g_tex_load_ticks.exchange(0)) / tpm;
    const double tex_store_ms = double(g_tex_store_ticks.exchange(0)) / tpm;
    const double cp = double(g_costs.cp_wait.exchange(0)) / tpm;
    const double ring = double(g_costs.ring_full.exchange(0)) / tpm;
    const double guest = double(g_costs.guest_frame_max.exchange(0)) / tpm;
    const uint32_t uploads = g_texture_cache ? g_texture_cache->UploadCount() : 0;
    static uint32_t last_uploads = 0;
    const uint32_t new_uploads = uploads - last_uploads;
    last_uploads = uploads;
    if (ms > kSlowFrameMs && ++p.slow_frames <= kSlowFramesLoggedPerWindow) {
      REXLOG_INFO(
          "PERF slow frame f{}: {:.1f} ms, {} shaders and {} pipelines compiled, {} draws, "
          "{} resolves; longest guest frame {:.1f} ms, worker busy {:.1f} ms, GPU-sync wait "
          "{:.1f} ms, ring full {:.1f} ms, {} texture uploads; worker draws {:.1f} ms, present {:.1f} ms; "
          "texture load {:.1f} ms, store {:.1f} ms",
          g_frame, ms, shaders, pipelines, draws, resolves, guest, busy, cp, ring, new_uploads,
          draw_ms, present_ms, tex_load_ms, tex_store_ms);
    }
  }
  double seconds = std::chrono::duration<double>(now - p.start).count();
  if (seconds < kPerfWindowSeconds) return;
  std::vector<float> sorted = p.frame_ms;
  std::sort(sorted.begin(), sorted.end(), std::greater<float>());
  // 1% low: the average frame rate of the slowest 1% of frames.
  size_t slowest = std::max<size_t>(1, sorted.size() / 100);
  double slowest_ms = 0.0;
  for (size_t i = 0; i < slowest; ++i) slowest_ms += sorted[i];
  slowest_ms /= double(slowest);
  REXLOG_INFO(
      "PERF last {:.0f} s: {} frames, avg {:.1f} fps, 1% low {:.1f} fps, worst {:.1f} ms, "
      "{} frames over {:.0f} ms; compiled {} shaders, {} pipelines (total {}, {})",
      seconds, sorted.size(), sorted.size() / seconds, 1000.0 / slowest_ms, sorted.front(),
      p.slow_frames, kSlowFrameMs, stats.shaders_compiled - p.at_start.shaders_compiled,
      stats.pipelines_created - p.at_start.pipelines_created, stats.shaders_compiled,
      stats.pipelines_created);
  std::string threads = BusiestThreads(seconds);
  if (!threads.empty()) REXLOG_INFO("PERF threads: {}", threads);
  std::string hot = HotSpots();
  if (!hot.empty()) REXLOG_INFO("PERF hot:{}", hot);
  p.start = now;
  p.frame_ms.clear();
  p.slow_frames = 0;
  p.at_start = stats;
}

// The game's gamma ramp (the 360's display LUT), as the Xenos presenter applies
// it: the 256-entry table for an 8-bit front buffer, the piecewise-linear ramp
// for a 10-bit one. Rebuilt when the game changes it; off while it is linear.
uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000, exp = (x >> 23) & 0xFF, man = x & 0x7FFFFF;
  if (exp < 113) return uint16_t(sign);  // tiny values to zero
  if (exp > 142) return uint16_t(sign | 0x7BFF);
  return uint16_t(sign | ((exp - 112) << 10) | (man >> 13));
}

// Presentation gamma on top of the game's ramp (as in the Oogie's Revenge PS5
// port): the picture looked brighter and washed out next to the Xbox 360.
// tron_present_gamma_x100 is the launcher's brightness setting.
float PresentGamma() { return float(REXCVAR_GET(tron_present_gamma_x100)) / 100.0f; }

// The picture was a little bluer than the Xbox 360's: blue gets this much more
// presentation gamma (tuned on the console), which takes blue out of the
// shadows and mid-tones while saturated cyan stays bright.
constexpr float kBlueExtraGamma = 0.20f;

void UpdateGammaRamp(uint32_t front_format) {
  auto* cp = rex::graphics::g_command_processor;
  if (!cp || !g_device) return;
  const float present_gamma = PresentGamma();
  const bool pwl = front_format == uint32_t(rex::graphics::xenos::TextureFormat::k_2_10_10_10) ||
                   front_format ==
                       uint32_t(rex::graphics::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  const auto* table = cp->gamma_table_256();
  const auto* ramp = cp->gamma_ramp_pwl();
  uint64_t key = pwl ? XXH3_64bits(ramp, sizeof(uint32_t) * 128 * 3)
                     : XXH3_64bits(table, sizeof(uint32_t) * 256);
  key ^= pwl ? 0x5057ull : 0;
  uint32_t gamma_bits;
  std::memcpy(&gamma_bits, &present_gamma, 4);
  key ^= uint64_t(gamma_bits) << 20;
  static uint64_t last_key = 0;
  if (key == last_key) return;
  last_key = key;
  std::vector<float> lut(1024 * 3);
  float deviation = 0, game_deviation = 0;
  for (uint32_t v = 0; v < 1024; ++v) {
    for (uint32_t c = 0; c < 3; ++c) {
      float out;
      if (pwl) {
        const auto& e = ramp[(v >> 3) * 3 + c];
        out = (float(e.base) + float((v & 7) * e.delta) * 0.125f) * 1.52737048e-05f;
      } else {
        const auto& e = table[(v * 255 + 511) / 1023];
        uint32_t level = c == 0 ? e.color_10_red : c == 1 ? e.color_10_green : e.color_10_blue;
        out = float(level) / 1023.0f;
      }
      out = std::clamp(out, 0.0f, 1.0f);
      lut[v * 3 + c] = out;
      game_deviation = std::max(game_deviation, std::abs(out - float(v) / 1023.0f));
      out = std::pow(out, present_gamma + (c == 2 ? kBlueExtraGamma : 0.0f));
      lut[v * 3 + c] = out;
      deviation = std::max(deviation, std::abs(out - float(v) / 1023.0f));
    }
  }
  auto at = [&](uint32_t v, uint32_t c) { return lut[v * 3 + c]; };
  REXLOG_INFO("NATIVE-GAMMA {} ramp (game deviation {:.3f}, presentation gamma {:.2f}): "
              "max deviation from linear {:.3f}; r/g/b at 25% "
              "{:.3f}/{:.3f}/{:.3f}, 50% {:.3f}/{:.3f}/{:.3f}, 75% {:.3f}/{:.3f}/{:.3f}{}",
              pwl ? "PWL" : "256-entry", game_deviation, present_gamma, deviation, at(256, 0), at(256, 1), at(256, 2), at(512, 0),
              at(512, 1), at(512, 2), at(768, 0), at(768, 1), at(768, 2),
              deviation < 1.0f / 255 ? " (linear: not applied)" : "");
  if (deviation < 1.0f / 255) {
    g_device->SetGammaRamp(nullptr);
    return;
  }
  std::vector<uint16_t> half(1024 * 4);
  for (uint32_t v = 0; v < 1024; ++v) {
    for (uint32_t c = 0; c < 3; ++c) half[v * 4 + c] = FloatToHalf(lut[v * 3 + c]);
    half[v * 4 + 3] = FloatToHalf(1.0f);
  }
  g_device->SetGammaRamp(half.data());
}

// Views of front buffer textures with the front buffer's swizzle.
std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ITextureView>> g_front_views;

dl::ITextureView* FrontBufferView(dl::ITexture* texture, uint32_t swizzle) {
  uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(texture)) ^ (uint64_t(swizzle) << 52);
  auto it = g_front_views.find(key);
  if (it != g_front_views.end()) return it->second;
  dl::TextureViewDesc desc;
  desc.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
  desc.TextureDim = dl::RESOURCE_DIM_TEX_2D;
  desc.Swizzle = ComponentMapping(swizzle);
  dl::RefCntAutoPtr<dl::ITextureView> view;
  texture->CreateView(desc, &view);
  g_front_views.emplace(key, view);
  return view ? view.RawPtr() : texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);
}

void SavePsoCache() {
  if (!g_pso_cache || g_pso_cache_path.empty()) return;
  dl::RefCntAutoPtr<dl::IDataBlob> blob;
  g_pso_cache->GetData(&blob);
  if (!blob || !blob->GetSize()) return;
  auto temp = g_pso_cache_path;
  temp += ".tmp";
  if (std::FILE* f = path_util::Open(temp, "wb")) {
    bool ok = std::fwrite(blob->GetConstDataPtr(), 1, blob->GetSize(), f) == blob->GetSize();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(temp, g_pso_cache_path, ec);
  }
  g_pso_cache_saved_pipelines = g_render_stats.pipelines_created;
  g_pso_cache_saved_time = std::chrono::steady_clock::now();
}

void CreatePsoCache(const std::filesystem::path& path) {
  g_pso_cache_path = path;
  std::vector<uint8_t> data;
  if (std::FILE* f = path_util::Open(path, "rb")) {
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      data.resize(std::fread(data.data(), 1, data.size(), f));
    }
    std::fclose(f);
  }
  dl::PipelineStateCacheCreateInfo ci;
  ci.Desc.Name = "DantePipelineCache";
  ci.pCacheData = data.empty() ? nullptr : data.data();
  ci.CacheDataSize = uint32_t(data.size());
  g_device->RenderDevice()->CreatePipelineStateCache(ci, &g_pso_cache);
  if (!g_pso_cache && !data.empty()) {
    // Another driver or GPU: start empty.
    ci.pCacheData = nullptr;
    ci.CacheDataSize = 0;
    g_device->RenderDevice()->CreatePipelineStateCache(ci, &g_pso_cache);
  }
  g_pso_cache_saved_time = std::chrono::steady_clock::now();
}

// Black frames with the overlay (the progress card) while the game waits.
void PresentPrecompileFrame() {
  {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock || !g_device || !g_device->UpdateWindowSize()) return;
    g_device->BeginFrame();
    g_device->Clear(0.0f, 0.0f, 0.0f, 1.0f);
    g_device->Present(0);
  }
  RequestOverlayFrame();
}

void StartPrecompile() {
  if (!g_pipeline_storage || g_pipeline_storage->pipelines().empty()) return;
  ShaderVariants variants;
  std::set<std::pair<uint64_t, uint64_t>> seen;
  for (const StoredDraw& draw : g_pipeline_storage->pipelines()) {
    const StoredPipeline& p = draw.pipeline;
    if (seen.emplace(p.vertex_hash, p.vertex_modification).second) {
      variants[p.vertex_hash].push_back(p.vertex_modification);
    }
    if (p.pixel_hash && seen.emplace(p.pixel_hash, p.pixel_modification).second) {
      variants[p.pixel_hash].push_back(p.pixel_modification);
    }
  }
  Precompile& c = g_precompile;
  c.shaders_total = uint32_t(seen.size());
  c.pipelines_total = uint32_t(g_pipeline_storage->pipelines().size());
  c.start = std::chrono::steady_clock::now();
  c.finished = false;
  PipelineTargets targets;
  targets.frame_scale = std::max(g_scale_x, g_scale_y);
  targets.shadow_scale = ShadowMapScale().x;
  targets.depth_format = [](uint32_t guest) { return uint32_t(DepthFormat(guest)); };
  c.thread = std::thread([variants = std::move(variants), targets] {
    Precompile& c = g_precompile;
    int32_t forced = REXCVAR_GET(dante_precompile_threads);
    uint32_t threads = forced > 0  ? uint32_t(forced)
                       : g_safe_start ? 1u
                                      : std::max(1u, std::thread::hardware_concurrency() - 1);
    std::atomic<bool> working{true};
    std::thread work([&] {
      g_shader_cache->Precompile(g_pipeline_storage->shaders(), variants, threads, c.cancel,
                                 c.shaders_done);
      c.pipelines = true;
      g_draw_renderer->Precompile(g_pipeline_storage->pipelines(), *g_shader_cache, targets,
                                  threads, c.cancel, c.pipelines_done);
      working = false;
    });
    while (working) {
      PresentPrecompileFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    work.join();
    float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - c.start).count();
    REXLOG_INFO("PRECOMPILE: {} shader variants, {} pipelines in {:.2f} s on {} threads{}",
                c.shaders_total, c.pipelines_total, seconds, threads,
                c.cancel ? " (cancelled)" : "");
    if (!c.cancel) SavePsoCache();
    std::function<void()> on_finished;
    {
      std::lock_guard lock(c.mutex);
      c.finished = true;
      on_finished = std::move(c.on_finished);
    }
    c.finished_cv.notify_all();
    if (on_finished) on_finished();
  });
}

void PresentTestPattern() {
  float phase = float(g_frame % 240) / 240.0f * 6.2831853f;
  g_device->BeginFrame();
  g_device->Clear(0.3f + 0.3f * std::sin(phase), 0.3f + 0.3f * std::sin(phase + 2.0943951f),
                  0.3f + 0.3f * std::sin(phase + 4.1887902f), 1.0f);
  g_device->Present(0);
}

}  // namespace

void AdoptDevice(std::unique_ptr<VulkanDevice> device) {
  std::lock_guard lock(g_mutex);
  g_adopted_device = std::move(device);
}

bool Start(rex::ui::Window* window, const std::filesystem::path& cache_dir) {
  std::lock_guard lock(g_mutex);
  if (g_device) return true;
  void* hwnd = window ? window->GetNativeWindowHandle() : nullptr;
#if !REX_PLATFORM_PS5
  if (!hwnd) {
    REXLOG_ERROR("DanteDevice: no game window");
    return false;
  }
#endif
  if (window) window->SetPresenter(nullptr);
  std::unique_ptr<VulkanDevice> device = std::move(g_adopted_device);
  if (device) {
    REXLOG_INFO("DanteDevice: presenting on the launcher's swap chain");
  } else {
    device = std::make_unique<VulkanDevice>();
    if (!device->Initialize(hwnd, 1280, 720)) {
      REXLOG_ERROR("DanteDevice: native device initialization failed");
      return false;
    }
  }
  const dl::GraphicsAdapterInfo& adapter = device->RenderDevice()->GetAdapterInfo();
  const dl::RenderDeviceInfo& device_info = device->RenderDevice()->GetDeviceInfo();
  REXLOG_INFO("DanteDevice: GPU {} (vendor {:04X} device {:04X}, {} MB VRAM), Vulkan {}.{}",
              adapter.Description, adapter.VendorId, adapter.DeviceId,
              // CPU-visible VRAM (Resizable BAR) is reported as unified memory.
              (adapter.Memory.LocalMemory + adapter.Memory.UnifiedMemory) >> 20,
              device_info.APIVersion.Major, device_info.APIVersion.Minor);
  RenderScale scale = ComputeRenderScale(window);
  device->SetDisplayAspect(scale.display_aspect, true);
  g_device = std::move(device);
  g_scale_x = scale.x;
  g_scale_y = scale.y;
  g_shadow_map_factor = float(std::clamp(REXCVAR_GET(dante_shadow_scale), 1, 4));
  constexpr dl::BIND_FLAGS kDepthUse = dl::BIND_DEPTH_STENCIL | dl::BIND_SHADER_RESOURCE;
  const auto& d24 =
      g_device->RenderDevice()->GetTextureFormatInfoExt(dl::TEX_FORMAT_D24_UNORM_S8_UINT);
  g_use_d24_depth = (d24.BindFlags & kDepthUse) == kDepthUse && !REXCVAR_GET(dante_depth_float);
  REXLOG_INFO("DanteDevice: depth buffers {}",
              g_use_d24_depth ? "D24S8" : "D32S8 (no 24-bit depth on this GPU or forced)");
  Scale target_scale{g_scale_x, g_scale_y};
  REXLOG_INFO("DanteDevice: resolution scale {:.3f} x {:.3f} ({}x{}, display aspect {:.4f})",
              g_scale_x, g_scale_y, target_scale.HostX(1280), target_scale.HostY(720),
              scale.display_aspect);
  g_shader_cache = std::make_unique<ShaderCache>(g_device->RenderDevice(), g_scale_x, g_scale_y);
  g_texture_cache = std::make_unique<TextureCache>(
      g_device->RenderDevice(), g_device->Context(),
      [](uint32_t address, uint32_t width, uint32_t height, uint32_t format,
         bool& rb_swap) -> dl::ITexture* {
        auto it = g_resolve_targets.find(ResolveTargetKey{address, width, height, format});
        if (it != g_resolve_targets.end()) {
          rb_swap = it->second.rb_swap;
          return it->second.texture.RawPtr();
        }
        // k_8_8_8_8 over a depth resolve (k_24_8): its raw words, from the
        // next resolve on.
        if (format != uint32_t(rex::graphics::xenos::TextureFormat::k_8_8_8_8)) return nullptr;
        it = g_resolve_targets.find(ResolveTargetKey{
            address, width, height, uint32_t(rex::graphics::xenos::TextureFormat::k_24_8)});
        if (it == g_resolve_targets.end()) return nullptr;
        it->second.depth_bits_wanted = true;
        rb_swap = false;
        return it->second.depth_bits.RawPtr();
      });
  g_draw_renderer = std::make_unique<DrawRenderer>(g_device->RenderDevice(), g_device->Context(),
                                                   g_texture_cache.get());
  g_smaa = std::make_unique<SmaaPass>(g_device.get());
  g_overlay = std::make_unique<OverlayRenderer>(g_device.get());
  g_device->SetBeforePresent(
      [](dl::ITextureView* back_buffer) { g_overlay->Draw(g_device->Context(), back_buffer); });
  g_button_glyphs = std::make_unique<ButtonGlyphs>(g_device->RenderDevice());
  g_mod_loader = std::make_unique<ModLoader>(g_device->RenderDevice(),
                                             rex::filesystem::GetExecutableFolder() / "mods");
  if (!g_mod_loader->Active()) g_mod_loader.reset();
  g_texture_cache->SetReplacer([](uint64_t hash, const TextureCache::LoadedTexture* loaded) {
    dl::ITexture* glyph = g_button_glyphs ? g_button_glyphs->Replace(hash, loaded) : nullptr;
    if (glyph) return glyph;
    return g_mod_loader ? g_mod_loader->Replace(hash, loaded) : nullptr;
  });
  if (!cache_dir.empty()) {
    std::error_code ec;
    g_startup_marker = cache_dir / "dante" / "startup.lock";
    std::filesystem::create_directories(g_startup_marker.parent_path(), ec);
    g_safe_start = std::filesystem::exists(g_startup_marker, ec);
    if (g_safe_start) {
      REXLOG_WARN(
          "DanteDevice: the previous start did not reach gameplay; starting without the Vulkan "
          "pipeline cache and precompiling on one thread");
    }
    if (std::FILE* f = path_util::Open(g_startup_marker, "wb")) std::fclose(f);
    // The list installed with the game, then what this player's runs added.
    g_pipeline_storage =
        std::make_unique<PipelineStorage>(cache_dir / "dante" / "pipelines.bin",
                                          rex::filesystem::GetExecutableFolder() / kShippedList);
    g_shader_cache->SetStorage(g_pipeline_storage.get());
    g_draw_renderer->SetStorage(g_pipeline_storage.get());
    if (g_safe_start) {
      std::filesystem::rename(cache_dir / "dante" / "vulkan_pipeline_cache.bin",
                              cache_dir / "dante" / "vulkan_pipeline_cache.bin.old", ec);
    }
    CreatePsoCache(cache_dir / "dante" / "vulkan_pipeline_cache.bin");
    g_draw_renderer->SetPipelineCache(g_pso_cache);
  }
  g_register_file = std::make_unique<rex::graphics::RegisterFile>();
  g_capture_regs = std::make_unique<rex::graphics::RegisterFile>();
  StartWorker();
  g_frame = 0;
  REXLOG_INFO("DanteDevice: presenting on the game window (SDK presenter detached)");
  if (auto logger = rex::GetLogger()) logger->flush();
  StartPrecompile();
  return true;
}

PrecompileStatus GetPrecompileStatus() {
  const Precompile& c = g_precompile;
  PrecompileStatus status;
  {
    std::lock_guard lock(g_precompile.mutex);
    status.active = !c.finished;
  }
  status.pipelines = c.pipelines;
  status.shaders_done = c.shaders_done;
  status.shaders_total = c.shaders_total;
  status.pipelines_done = c.pipelines_done;
  status.pipelines_total = c.pipelines_total;
  status.start = c.start;
  return status;
}

bool RunWhenPrecompiled(std::function<void()> done) {
  std::lock_guard lock(g_precompile.mutex);
  if (g_precompile.finished) return false;
  g_precompile.on_finished = std::move(done);
  return true;
}

void Stop() {
  g_precompile.cancel = true;
  if (g_precompile.thread.joinable()) g_precompile.thread.join();
  StopWorker();
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  if (!g_startup_marker.empty()) {
    std::error_code ec;
    std::filesystem::remove(g_startup_marker, ec);
  }
  SavePsoCache();
  g_pso_cache.Release();
  g_render_targets.clear();
  g_shadow_maps.clear();
  g_resolve_targets.clear();
  g_readback_fence.Release();
  g_readback_value = 0;
  g_replay = {};
  g_depth_copy = {};
  g_depth_bits = {};
  g_front_views.clear();
  g_loaded_vs = {};
  g_last_ps = {};
  g_smaa.reset();
  g_device->SetBeforePresent(nullptr);
  g_overlay.reset();
  g_draw_renderer.reset();
  g_button_glyphs.reset();
  g_mod_loader.reset();
  g_texture_cache.reset();
  g_shader_cache.reset();
  g_pipeline_storage.reset();
  g_device->Shutdown();
  g_device.reset();
  REXLOG_INFO("DanteDevice: stopped after {} frames", g_frame);
}

void LastIndexBuffer(uint32_t& base, uint32_t& count, uint32_t& format) {
  base = g_last_index_base;
  count = g_last_index_count;
  format = g_last_index_format;
}

void BoundShaderHashes(uint32_t dev, const uint8_t* base, uint64_t& vs, uint64_t& ps) {
  GuestShaderCode v = VertexShaderCode(base, dev), p = PixelShaderCode(base, dev);
  vs = v.ucode ? XXH3_64bits(v.ucode, v.dwords * 4) : 0;
  ps = p.ucode ? XXH3_64bits(p.ucode, p.dwords * 4) : 0;
}

// Clear(count, pRects, flags, color, Z, stencil): flags TARGET0..3 = bits 0-3,
// ZBUFFER = 0x10, STENCIL = 0x20; color is D3DCOLOR (ARGB).
void AfterClear(const CallArgs& a, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  uint32_t dev = a.r[0];
  uint32_t flags = a.r[3];
  uint32_t c = a.r[4];
  const float color[4] = {float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
                          float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    SurfaceDesc s;
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, s)) continue;
    if (RenderTarget* rt = GetRenderTarget(s)) ClearColor(*rt, color);
    if (g_log_this_frame) {
      REXLOG_INFO("NATIVE-RT f{} clear RT{} base={} pitch={} fmt={} color={:08X}", g_frame, i,
                  s.base_tile, s.pitch, s.format, c);
    }
  }
  if (flags & 0x30) {
    SurfaceDesc s;
    if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, s)) {
      if (RenderTarget* rt = GetRenderTarget(s)) {
        ClearDepth(*rt, flags & 0x10, flags & 0x20, float(a.f1), uint8_t(a.r[5]));
      }
      if (g_log_this_frame) {
        REXLOG_INFO("NATIVE-RT f{} clear depth base={} pitch={} z={:.3f} stencil={}", g_frame,
                    s.base_tile, s.pitch, a.f1, a.r[5] & 0xFF);
      }
    }
  }
}

// Tron calls the XDK's inner ClearF(dev, flags, pRect, pColor, Z=f1, stencil)
// directly: pColor points to four big-endian floats (r, g, b, a).
void AfterClearF(const CallArgs& a, const uint8_t* base) {
  if (!g_device) return;
  uint32_t dev = a.r[0];
  uint32_t flags = a.r[1];
  std::array<float, 4> color = {0, 0, 0, 0};
  if (IsPlausibleAddress(a.r[3])) {
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t bits = Load32(base, a.r[3] + i * 4);
      std::memcpy(&color[i], &bits, 4);
    }
  }
  std::array<SurfaceDesc, 4> rts;
  uint32_t rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    if (ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, rts[i])) {
      rt_mask |= 1u << i;
    }
  }
  SurfaceDesc ds;
  bool has_ds = (flags & 0x30) && ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, ds);
  float z = float(a.f1);
  uint8_t stencil = uint8_t(a.r[5]);
  EnqueueFn([=] {
    if (!g_device) return;
    if (g_draw_renderer) g_draw_renderer->InvalidateBindings();
    FlushDrawLog();
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(rt_mask & (1u << i))) continue;
      if (RenderTarget* rt = GetRenderTarget(rts[i])) ClearColor(*rt, color.data());
      if (g_log_this_frame) {
        REXLOG_INFO("NATIVE-RT f{} clearF RT{} base={} pitch={} fmt={} color={:.3f},{:.3f},{:.3f},{:.3f}",
                    g_frame, i, rts[i].base_tile, rts[i].pitch, rts[i].format, color[0], color[1],
                    color[2], color[3]);
      }
    }
    if (has_ds) {
      if (RenderTarget* rt = GetRenderTarget(ds)) {
        ClearDepth(*rt, flags & 0x10, flags & 0x20, z, stencil);
      }
    }
  });
}

void AfterBeginIndexedVertices(const CallArgs& a, const uint8_t* base) {
  uint32_t dev = a.r[0];
  // dev+13652: last dword the call wrote; +13656 vertex data; +13660 index data.
  uint32_t last = Load32(base, dev + 13652);
  uint32_t vertex_data = Load32(base, dev + 13656);
  if (!IsPlausibleAddress(last) || !IsPlausibleAddress(vertex_data)) {
    ++g_indexed_inline_unparsed;
    return;
  }
  for (uint32_t back = 0; back < 32; ++back) {
    uint32_t header = last - back * 4;
    if (Load32(base, header) != 0xC0032201u) continue;
    IndexedInlineDraw& d = g_pending_indexed_inline;
    d.pending = true;
    d.dev = dev;
    d.initiator = Load32(base, header + 8);
    d.dma_base = Load32(base, header + 12);
    d.dma_size = Load32(base, header + 16);
    d.vertices = {};
    d.vertices.args = a;
    d.vertices.prim = a.r[1];
    d.vertices.count = a.r[3];   // NumVertices
    d.vertices.stride = a.r[6];  // VertexStreamZeroStride
    d.vertices.data = vertex_data;
    return;
  }
  ++g_indexed_inline_unparsed;
}

// Tron: EndVertices is inlined into the engine, so a pending BeginVertices
// draw is issued when the next D3D function is entered.
void FlushPendingInline(const uint8_t* base) {
  if (g_pending_inline_draw.pending) AfterEndVertices(base);
  if (g_pending_indexed_inline.pending) {
    IndexedInlineDraw& d = g_pending_indexed_inline;
    d.pending = false;
    if (!g_device || !g_draw_renderer) return;
    SubmitDraw(d.dev, d.initiator, d.dma_base, d.dma_size, &d.vertices, base);
  }
}

// Resolve(flags, pSourceRect, pDestTexture, pDestPoint, level, slice,
// pClearColor, ClearZ, ...): flags & 7 = source (RT0-3, 4 = depth),
// 0x100 / 0x200 = clear color / depth-stencil afterwards.
struct ResolveCapture {
  uint32_t flags = 0;
  bool s_ok = false;
  SurfaceDesc s;
  uint32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0, dst_x = 0, dst_y = 0;
  uint32_t dest = 0;
  bool dest_ok = false;
  uint32_t dest_dwords[6] = {};
  uint32_t slice_arg = 0;
  uint32_t dest_info = ~0u;
  bool has_clear_color = false;
  float clear_color[4] = {};
  bool ds_ok = false;
  SurfaceDesc ds;
  float z = 0;
};
void ExecuteResolve(const ResolveCapture& c, const uint8_t* base);

void AfterResolve(const CallArgs& a, const uint8_t* base) {
  if (!g_device) return;
  ResolveCapture c;
  uint32_t dev = a.r[0];
  c.flags = a.r[1];
  uint32_t source = c.flags & 7;
  bool from_depth = source == 4;
  c.s_ok = ReadSurface(base, Load32(base, dev + (from_depth ? kDevDepthStencil
                                                             : kDevRenderTargets + source * 4)),
                       from_depth, c.s);
  c.x2 = c.s.width;
  c.y2 = c.s.height;
  if (IsPlausibleAddress(a.r[2])) {
    c.x1 = Load32(base, a.r[2]);
    c.y1 = Load32(base, a.r[2] + 4);
    c.x2 = Load32(base, a.r[2] + 8);
    c.y2 = Load32(base, a.r[2] + 12);
  }
  if (IsPlausibleAddress(a.r[4])) {
    c.dst_x = Load32(base, a.r[4]);
    c.dst_y = Load32(base, a.r[4] + 4);
  }
  c.dest = a.r[3];
  c.dest_ok = IsPlausibleAddress(c.dest);
  if (c.dest_ok) {
    for (uint32_t i = 0; i < 6; ++i) c.dest_dwords[i] = Load32(base, c.dest + 0x1C + i * 4);
    // RB_COPY_DEST_INFO from the Resolve's packets, which may span several
    // command buffer segments.
    uint32_t span_start = a.write_ptr_before;
    uint32_t switches = g_segment_switch_count - a.segment_switches_before;
    if (switches <= kSegmentSwitchHistory) {
      for (uint32_t i = a.segment_switches_before; i != g_segment_switch_count; ++i) {
        const SegmentSwitch& sw = g_segment_switches[i % kSegmentSwitchHistory];
        uint32_t v = FindRegisterWrite(base, span_start, sw.old_ptr, 0x231B);
        if (v != ~0u) c.dest_info = v;
        span_start = sw.new_ptr;
      }
      uint32_t v = FindRegisterWrite(base, span_start,
                                     Load32(base, dev + kDevCommandBufferWritePtr), 0x231B);
      if (v != ~0u) c.dest_info = v;
    }
  }
  c.slice_arg = a.r[6];
  if ((c.flags & 0x100) && !from_depth && IsPlausibleAddress(a.r[7])) {
    c.has_clear_color = true;
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t bits = Load32(base, a.r[7] + i * 4);
      std::memcpy(&c.clear_color[i], &bits, 4);
    }
  }
  if (c.flags & 0x200) {
    c.ds_ok = ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, c.ds);
  }
  c.z = float(a.f1);
  EnqueueFn([c, base] { ExecuteResolve(c, base); });
}

void ExecuteResolve(const ResolveCapture& c, const uint8_t* base) {
  if (!g_device) return;
  if (g_texture_cache) g_texture_cache->InvalidateFrameViews();
  if (g_draw_renderer) g_draw_renderer->InvalidateBindings();
  FlushDrawLog();
  ++g_swap_resolves;
  uint32_t flags = c.flags;
  bool from_depth = (flags & 7) == 4;
  if (!c.s_ok) return;
  SurfaceDesc s = c.s;
  RenderTarget* rt = GetRenderTarget(s);
  if (!rt) return;

  uint32_t x1 = c.x1, y1 = c.y1, x2 = c.x2, y2 = c.y2;
  uint32_t dst_x = c.dst_x, dst_y = c.dst_y;

  uint32_t dest = c.dest;
  if (c.dest_ok) {
    // Fetch constant at +0x1C: dword1 = base | format, dword2 = size.
    uint32_t dword1 = c.dest_dwords[1];
    uint32_t dword2 = c.dest_dwords[2];
    // Texture objects hold the guest virtual address; fetch constants (what
    // sampling sees) hold the physical one.
    uint32_t address = GpuAddress(dword1 & 0xFFFFF000u);
    uint32_t guest_format = dword1 & 0x3F;
    uint32_t width = (dword2 & 0x1FFF) + 1;
    uint32_t height = ((dword2 >> 13) & 0x1FFF) + 1;
    uint32_t volume_depth = 0, slice = 0;
    rex::graphics::xenos::xe_gpu_texture_fetch_t dest_fetch;
    uint32_t dest_dwords[6];
    for (uint32_t i = 0; i < 6; ++i) dest_dwords[i] = c.dest_dwords[i];
    std::memcpy(&dest_fetch, dest_dwords, sizeof(dest_fetch));
    if (dest_fetch.dimension == rex::graphics::xenos::DataDimension::k3D) {
      width = dest_fetch.size_3d.width + 1;
      height = dest_fetch.size_3d.height + 1;
      volume_depth = dest_fetch.size_3d.depth + 1;
      slice = std::min(c.slice_arg, volume_depth - 1);
    }
    ResolveTarget* t = GetResolveTarget(address, width, height, guest_format,
                                        volume_depth ? slice + 1 : 0, rt->scale);
    if (t) {
      t->volume_depth = volume_depth;
      t->slice = slice;
    }
    // RB_COPY_DEST_INFO from the Resolve's packets, which may span several
    // command buffer segments.
    uint32_t dest_info = c.dest_info;
    // Not found (more switches than the history keeps): keep the target's
    // previous setting; the game resolves each target the same way.
    bool rb_swap = dest_info != ~0u ? bool((dest_info >> 24) & 1) : t && t->rb_swap;
    if (dest_info == ~0u) ++g_swap_dest_info_missing;
    const char* result = "skipped";
    if (t && !from_depth && t->format == rt->format) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        dl::Box box(rt->scale.HostX(x1), rt->scale.HostX(x2), rt->scale.HostY(y1),
                    rt->scale.HostY(y2));
        dl::CopyTextureAttribs copy(rt->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                    t->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        copy.pSrcBox = &box;
        copy.DstX = t->scale.HostX(dst_x);
        copy.DstY = t->scale.HostY(dst_y);
        // The source is usually still bound as the render target.
        g_device->Context()->SetRenderTargets(0, nullptr, nullptr,
                                              dl::RESOURCE_STATE_TRANSITION_MODE_NONE);
        // copy_dest_exp_bias: signed 6 bits at 16.
        int32_t exp_bias = dest_info != ~0u ? int32_t(dest_info << 10) >> 26 : 0;
        // The guest format's range (see kColorScalePS).
        const int32_t clamp_mode = s.format == 3 || s.format == 12 ? 1
                             : s.format == 4 || s.format == 5 ? 2
                                                              : 0;
        if ((exp_bias || clamp_mode) &&
            CopyColorScaled(*rt, t->texture, rt->scale.HostX(x1), rt->scale.HostY(y1),
                            rt->scale.HostX(x2), rt->scale.HostY(y2), t->scale.HostX(dst_x),
                            t->scale.HostY(dst_y), exp_bias, clamp_mode)) {
          result = "copied(scaled)";
        } else {
          g_device->Context()->CopyTexture(copy);
          result = rb_swap ? "copied(rb swap)" : "copied";
        }
        t->rb_swap = rb_swap;
        QueueReadback(*t, base, dest);
      }
    } else if (t && from_depth && t->format == dl::TEX_FORMAT_R32_FLOAT) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        CopyDepth(*rt, t->texture, rt->scale.HostX(x1), rt->scale.HostY(y1), rt->scale.HostX(x2),
                  rt->scale.HostY(y2), t->scale.HostX(dst_x), t->scale.HostY(dst_y));
        result = "depth-copied";
        if (t->depth_bits_wanted && !rt->guest_float_depth) {
          const auto& desc = t->texture->GetDesc();
          if (!t->depth_bits) {
            dl::TextureDesc bits;
            bits.Name = "ResolveDepthBits";
            bits.Type = dl::RESOURCE_DIM_TEX_2D;
            bits.Width = desc.Width;
            bits.Height = desc.Height;
            bits.Format = dl::TEX_FORMAT_RGBA8_UNORM;
            bits.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_RENDER_TARGET;
            g_device->RenderDevice()->CreateTexture(bits, nullptr, &t->depth_bits);
            if (t->depth_bits) ClearNewTexture(t->depth_bits, false);
          }
          if (t->depth_bits) {
            CopyDepthBits(*rt, t->depth_bits, rt->scale.HostX(x1), rt->scale.HostY(y1),
                          rt->scale.HostX(x2), rt->scale.HostY(y2), t->scale.HostX(dst_x),
                          t->scale.HostY(dst_y));
          }
        }
      }
    } else if (t) {
      // Other format conversions need a shader copy.
      uint64_t pair = (uint64_t(from_depth) << 40) | (uint64_t(s.format) << 32) | guest_format;
      if (g_reported_conversions.insert(pair).second) {
        REXLOG_INFO("NATIVE-RT resolve needs conversion: {} fmt={} -> guest fmt={}",
                    from_depth ? "depth" : "color", s.format, guest_format);
      }
    }
    if (g_dump_this_frame && t) {
      DumpTexturePng(
          g_device->RenderDevice(), g_device->Context(), t->texture,
          FrameDumpDirectory() / fmt::format("f{}_{:02}_{}_{:08X}_{}x{}_g{}.png", g_frame,
                                             ++g_dump_seq, from_depth ? "depth" : "color", address,
                                             width, height, guest_format));
    }
    if (g_log_this_frame) {
      REXLOG_INFO(
          "NATIVE-RT f{} resolve {} base={} pitch={} fmt={} rect=({},{})-({},{}) -> {:08X} "
          "{}x{} fmt={} at ({},{}) {} dest_info={:08X}",
          g_frame, from_depth ? "depth" : "color", s.base_tile, s.pitch, s.format, x1, y1, x2, y2,
          address, width, height, guest_format, dst_x, dst_y, result, dest_info);
    }
  }

  if ((flags & 0x100) && !from_depth && c.has_clear_color) ClearColor(*rt, c.clear_color);
  if ((flags & 0x200) && c.ds_ok) {
    if (RenderTarget* depth_rt = GetRenderTarget(c.ds)) ClearDepth(*depth_rt, true, true, c.z, 0);
  }
}

void BeforeDraw(uint32_t device, const uint8_t* base) {
  return;  // per-run draw logs are written by the worker (FlushDrawLog)
  if (!g_log_this_frame) return;
  std::lock_guard lock(g_mutex);
  SurfaceDesc s;
  ReadSurface(base, Load32(base, device + kDevRenderTargets), false, s);
  RenderTargetKey key{s.base_tile, s.pitch, s.format, false};
  if (!(key == g_log_draw_target)) FlushDrawLog();
  g_log_draw_target = key;
  ++g_logged_run_draws;
}

void AfterDraw(const CallArgs& a, DrawKind kind, const uint8_t* base) {
  if (!g_device || !g_shader_cache || !g_draw_renderer) return;
  if (kind == DrawKind::kInline) return;  // drawn at EndVertices
  uint32_t dev = a.r[0];
  if (kind == DrawKind::kInlineUP) {
    // DrawVerticesUP(prim, count, pData, stride): its nested BeginVertices
    // provided the ring copy of the data.
    if (!g_pending_inline_draw.pending) return;
    g_pending_inline_draw.pending = false;
    SubmitDraw(dev, InlineDrawInitiator(g_pending_inline_draw), 0, 0, &g_pending_inline_draw, base);
    return;
  }
  uint32_t initiator, dma_base, dma_size;
  if (!ParseDrawPacket(base, dev, initiator, dma_base, dma_size)) {
    ++g_draws_unparsed;
    return;
  }
  SubmitDraw(dev, initiator, dma_base, dma_size, nullptr, base);
}

void AfterBeginVertices(const CallArgs& a, uint32_t data) {
  g_pending_inline_draw.pending = true;
  g_pending_inline_draw.args = a;
  g_pending_inline_draw.prim = a.r[1];
  g_pending_inline_draw.count = a.r[2];
  g_pending_inline_draw.stride = a.r[3];
  g_pending_inline_draw.data = data;
}

void AfterEndVertices(const uint8_t* base) {
  if (!g_device || !g_draw_renderer || !g_pending_inline_draw.pending) return;
  g_pending_inline_draw.pending = false;
  SubmitDraw(g_pending_inline_draw.args.r[0], InlineDrawInitiator(g_pending_inline_draw), 0, 0,
             &g_pending_inline_draw, base);
}

void SetButtonGlyphStyleSource(std::function<GlyphStyle()> source) {
  std::lock_guard lock(g_mutex);
  g_glyph_style_source = std::move(source);
}

void SetButtonGlyphBindingsSource(std::function<KeyBindings()> source) {
  std::lock_guard lock(g_mutex);
  g_glyph_bindings_source = std::move(source);
}

// Swap(pFrontBuffer, ...): present the native texture resolved to the front buffer.
struct SwapCapture {
  bool front_ok = false;
  uint32_t dword1 = 0, dword2 = 0, dword3 = 0;
};
void ExecuteSwap(const SwapCapture& c, const uint8_t* base, bool log_next_frame);

void AfterSwap(const CallArgs& a, const uint8_t* base, bool log_next_frame) {
  {
    static uint64_t last = 0;
    const uint64_t now = Ticks();
    if (last) {
      g_costs.guest_frame.store(now - last, std::memory_order_relaxed);
      uint64_t prev = g_costs.guest_frame_max.load(std::memory_order_relaxed);
      while (now - last > prev &&
             !g_costs.guest_frame_max.compare_exchange_weak(prev, now - last)) {
      }
    }
    last = now;
  }
  if (!g_device) return;
  SwapCapture c;
  uint32_t front = a.r[1];
  c.front_ok = IsPlausibleAddress(front);
  if (c.front_ok) {
    c.dword1 = Load32(base, front + 0x20);
    c.dword2 = Load32(base, front + 0x24);
    c.dword3 = Load32(base, front + 0x28);
  }
  EnqueueFn([c, base, log_next_frame] { ExecuteSwap(c, base, log_next_frame); });
}

void ExecuteSwap(const SwapCapture& c, const uint8_t* base, bool log_next_frame) {
  if (!g_device) return;
  if (g_draw_renderer) g_draw_renderer->InvalidateBindings();
  FlushDrawLog();
  FinishReadbacks(base);
  g_logged_run_draws = 0;

  uint32_t address = 0;
  ResolveTargetKey key{};
  if (c.front_ok) {
    uint32_t dword1 = c.dword1;
    uint32_t dword2 = c.dword2;
    address = GpuAddress(dword1 & 0xFFFFF000u);
    key = {address, (dword2 & 0x1FFF) + 1, ((dword2 >> 13) & 0x1FFF) + 1, dword1 & 0x3F};
  }
  auto it = g_resolve_targets.find(key);
  bool found = it != g_resolve_targets.end() && it->second.texture;

  // Debug view: present a color host RT instead of the front buffer.
  dl::ITextureView* debug_view = nullptr;
  if (int32_t view = REXCVAR_GET(dante_debug_view); view > 0) {
    std::vector<std::pair<RenderTargetKey, RenderTarget*>> colors;
    for (auto& [rt_key, rt] : g_render_targets) {
      if (!rt_key.depth && rt.texture) colors.push_back({rt_key, &rt});
    }
    std::sort(colors.begin(), colors.end(), [](const auto& a, const auto& b) {
      return std::tie(a.first.base_tile, a.first.pitch, a.first.format) <
             std::tie(b.first.base_tile, b.first.pitch, b.first.format);
    });
    if (!colors.empty()) {
      auto& [rt_key, rt] = colors[(view - 1) % colors.size()];
      debug_view = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);
      static int32_t logged_view = -1;
      if (logged_view != view) {
        logged_view = view;
        REXLOG_INFO("NATIVE-RT debug view {}: color RT base={} pitch={} fmt={} {}x{}", view,
                    rt_key.base_tile, rt_key.pitch, rt_key.format, rt->width, rt->height);
      }
    }
  }
  if (g_log_this_frame) {
    REXLOG_INFO("NATIVE-RT f{} swap front={:08X} {}", g_frame, address,
                found ? "presented" : "missing (test pattern)");
  }
  if (int32_t record = REXCVAR_GET(dante_record_frames); record > 0) {
    if (found) {
      auto dir = FrameDumpDirectory() / "record";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                     dir / fmt::format("f{:06}.png", g_frame));
    }
    REXLOG_INFO(
        "NATIVE-REC f{} front={:08X} {}x{} fmt={} rb_swap={} draws={} resolves={} "
        "dest_info_missing={}{}",
        g_frame, address, key.width, key.height, key.guest_format, found && it->second.rb_swap,
        g_swap_draws, g_swap_resolves, g_swap_dest_info_missing,
        found ? "" : " (front buffer missing)");
    rex::cvar::SetFlagByName("dante_record_frames", std::to_string(record - 1));
  }
  // Anti-alias the finished frame (3D and UI) before it is presented.
  if (found && g_smaa && REXCVAR_GET(dante_smaa)) {
    if (g_dump_this_frame) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                     FrameDumpDirectory() / fmt::format("f{}_98_front_before_smaa.png", g_frame));
    }
    g_smaa->Apply(it->second.texture);
    if (g_dump_this_frame && g_smaa->Edges()) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), g_smaa->Edges(),
                     FrameDumpDirectory() / fmt::format("f{}_98_smaa_edges.png", g_frame));
    }
  }
  uint32_t front_swizzle = 0;
  dl::ITextureView* front_view = nullptr;
  if (found) {
    // The front buffer's fetch swizzle (dword 3 bits 1-12) maps the stored
    // channels to RGB, e.g. ZYX1 for this game's BGR-ordered front buffer.
    front_swizzle = (c.dword3 >> 1) & 0xFFF;
    if (it->second.rb_swap) front_swizzle = SwapRedBlueSwizzle(front_swizzle);
    front_view = FrontBufferView(it->second.texture, front_swizzle);
  }
  ReplayCapture(debug_view ? debug_view : front_view,
                fmt::format("f{} front={:08X} {}x{} fmt={} rb_swap={} swizzle={:03X} draws={} "
                            "resolves={} dest_info_missing={} new_targets={}{}",
                            g_frame, address, key.width, key.height, key.guest_format,
                            found && it->second.rb_swap, front_swizzle, g_swap_draws,
                            g_swap_resolves, g_swap_dest_info_missing, g_swap_new_targets,
                            found ? "" : " (front buffer missing)"));
  if (REXCVAR_GET(dante_replay_dump)) {
    rex::cvar::SetFlagByName("dante_replay_dump", "false");
    ReplayDump();
  }
  PerfOnSwap(g_swap_draws, g_swap_resolves);
  if (g_button_glyphs && g_glyph_style_source) {
    GlyphStyle style = g_glyph_style_source();
    if (style == GlyphStyle::kKeyboard && g_glyph_bindings_source) {
      g_button_glyphs->SetBindings(g_glyph_bindings_source());
    }
    g_button_glyphs->SetStyle(style);
  }
  g_swap_draws = g_swap_resolves = g_swap_dest_info_missing = g_swap_new_targets = 0;
  if (g_dump_this_frame && found) {
    DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                   FrameDumpDirectory() / fmt::format("f{}_99_front.png", g_frame));
  }
  ++g_frame;
  if (g_frame == 300 && !g_startup_marker.empty()) {
    std::error_code ec;
    std::filesystem::remove(g_startup_marker, ec);
  }
  g_dump_this_frame = log_next_frame && !g_log_this_frame;
  g_dump_seq = 0;
  g_log_this_frame = log_next_frame;
  if (g_draw_renderer) g_draw_renderer->SetFrame(g_frame);
  if (g_shader_cache && g_draw_renderer && g_frame % 300 == 0) {
    g_shader_cache->LogStats();
    g_draw_renderer->LogStats();
    g_texture_cache->LogStats();
    REXLOG_INFO("NATIVE-DRAW draws_without_shaders={} unparsed={} indexed_inline_unparsed={}",
                g_draws_without_shaders, g_draws_unparsed, g_indexed_inline_unparsed);
  }
  if (!g_device->UpdateWindowSize()) {  // minimized
    g_device->Context()->Flush();
    g_device->Context()->FinishFrame();
    return;
  }
  if (debug_view) {
    g_device->PresentTexture(debug_view, 0);
  } else if (front_view) {
    UpdateGammaRamp(key.guest_format);
    const uint64_t present_start = Ticks();
    g_device->PresentTexture(front_view, 0);
    g_costs.present.fetch_add(Ticks() - present_start, std::memory_order_relaxed);
  } else {
    PresentTestPattern();
  }
  {
    std::lock_guard present_lock(g_present_mutex);
    ++g_present_count.frames;
    g_present_count.last = std::chrono::steady_clock::now();
  }
  // New pipelines reach the Vulkan cache file now and then, not only at exit.
  if (g_render_stats.pipelines_created != g_pso_cache_saved_pipelines &&
      std::chrono::steady_clock::now() - g_pso_cache_saved_time > std::chrono::minutes(2)) {
    SavePsoCache();
  }
  RequestOverlayFrame();
}

PresentCount PresentedFrames() {
  std::lock_guard lock(g_present_mutex);
  return g_present_count;
}

}  // namespace renderer
