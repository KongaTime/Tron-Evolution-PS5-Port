// Hooks on the game's XDK D3D layer (docs/NATIVE_D3D_MAP.md). Every hook calls
// the original __imp__sub_X; the draw, clear, resolve and swap hooks also
// drive renderer=dante and the state verifier. --d3d_trace_frames=N (F8: 3
// frames) logs the calls as "D3D-TRACE" and the first sighting of each object
// as "D3D-OBJ".

#include "guest_gpu_state.h"
#include "renderdoc_capture.h"
#include "renderer.h"
#include "state_verifier.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <fmt/format.h>

REXCVAR_DEFINE_INT32(d3d_trace_frames, 0, "Diagnostics",
                     "Log hooked D3D API calls (args, caller, return) for the "
                     "next N frames, then a per-frame call-count summary")
    .range(0, 1000);

namespace {

struct TraceSlot {
  const char* label;
  uint32_t guest_address;
  uint32_t max_logged_per_frame;
  int skip_null_reg;  // 3..10, or 0 = none
  int dump_reg;       // 3..10, or 0 = none
  uint32_t dump_dwords;
  std::atomic<uint32_t> frame_calls{0};
  std::atomic<uint32_t> frame_logged{0};
};

constexpr uint32_t kMaxSlots = 64;
constexpr uint32_t kMaxDumpsPerSlot = 64;

std::atomic<bool> g_tracing{false};
std::atomic<uint32_t> g_frame_index{0};

TraceSlot* g_slots[kMaxSlots];
std::atomic<uint32_t> g_slot_count{0};

std::mutex g_dump_mutex;
// (slot address << 32) | object address of every object dumped.
std::unordered_set<uint64_t> g_dumped;

struct SlotRegistrar {
  explicit SlotRegistrar(TraceSlot* slot) {
    uint32_t index = g_slot_count.fetch_add(1);
    if (index < kMaxSlots) g_slots[index] = slot;
  }
};

struct SavedArgs {
  uint32_t r[8];  // r3..r10
  double f1, f2;
  uint32_t lr;
};

// PPCContext field order is r3, r0, r1, r2, r4..., so no array indexing.
inline uint32_t ArgReg(const PPCContext& ctx, int reg) {
  switch (reg) {
    case 3: return ctx.r3.u32;
    case 4: return ctx.r4.u32;
    case 5: return ctx.r5.u32;
    case 6: return ctx.r6.u32;
    case 7: return ctx.r7.u32;
    case 8: return ctx.r8.u32;
    case 9: return ctx.r9.u32;
    case 10: return ctx.r10.u32;
    default: return 0;
  }
}

void DumpObject(const TraceSlot& slot, const uint8_t* base, uint32_t obj) {
  if (!guest::IsPlausibleAddress(obj)) return;
  if (uint64_t(obj) + slot.dump_dwords * 4 + 0x1000 > 0x100000000ull) return;
  {
    std::lock_guard lock(g_dump_mutex);
    uint64_t prefix = uint64_t(slot.guest_address) << 32;
    uint32_t per_slot = 0;
    for (uint64_t key : g_dumped) {
      if ((key & 0xFFFFFFFF00000000ull) == prefix) ++per_slot;
    }
    if (per_slot >= kMaxDumpsPerSlot) return;
    if (!g_dumped.insert(prefix | obj).second) return;
  }
  for (uint32_t row = 0; row < slot.dump_dwords; row += 8) {
    std::string line;
    for (uint32_t i = row; i < row + 8 && i < slot.dump_dwords; ++i) {
      line += fmt::format(" {:08X}", guest::Load32(base, obj + i * 4));
    }
    REXLOG_INFO("D3D-OBJ f{} {} obj={:08X} +{:03X}:{}",
                g_frame_index.load(std::memory_order_relaxed), slot.label, obj, row * 4, line);
  }
}

inline bool TraceEnter(TraceSlot& slot, const PPCContext& ctx, const uint8_t* base,
                       SavedArgs& saved) {
  if (!g_tracing.load(std::memory_order_relaxed)) return false;
  slot.frame_calls.fetch_add(1, std::memory_order_relaxed);
  if (slot.dump_reg) DumpObject(slot, base, ArgReg(ctx, slot.dump_reg));
  if (slot.skip_null_reg && ArgReg(ctx, slot.skip_null_reg) == 0) return false;
  if (slot.frame_logged.fetch_add(1, std::memory_order_relaxed) >= slot.max_logged_per_frame) {
    return false;
  }
  for (int i = 0; i < 8; ++i) saved.r[i] = ArgReg(ctx, 3 + i);
  saved.f1 = ctx.f1.f64;
  saved.f2 = ctx.f2.f64;
  saved.lr = static_cast<uint32_t>(ctx.lr);
  return true;
}

inline void TraceExit(const TraceSlot& slot, const PPCContext& ctx, const SavedArgs& a) {
  REXLOG_INFO(
      "D3D-TRACE f{} {} [{:08X}] from {:08X}: r3={:08X} r4={:08X} r5={:08X} "
      "r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} f1={:.4f} f2={:.4f} "
      "-> r3={:08X}",
      g_frame_index.load(std::memory_order_relaxed), slot.label, slot.guest_address, a.lr, a.r[0],
      a.r[1], a.r[2], a.r[3], a.r[4], a.r[5], a.r[6], a.r[7], a.f1, a.f2, ctx.r3.u32);
}

void TraceFrameBoundary() {
  bool was_tracing = g_tracing.load(std::memory_order_relaxed);
  if (was_tracing) {
    uint32_t frame = g_frame_index.load(std::memory_order_relaxed);
    uint32_t count = std::min(g_slot_count.load(), kMaxSlots);
    for (uint32_t i = 0; i < count; ++i) {
      TraceSlot* slot = g_slots[i];
      uint32_t calls = slot->frame_calls.exchange(0);
      slot->frame_logged.store(0);
      if (calls) {
        REXLOG_INFO("D3D-TRACE f{} summary {} [{:08X}] calls={}", frame, slot->label,
                    slot->guest_address, calls);
      }
    }
    int32_t remaining = REXCVAR_GET(d3d_trace_frames) - 1;
    REXCVAR_SET(d3d_trace_frames, remaining < 0 ? 0 : remaining);
    g_frame_index.fetch_add(1);
  }
  bool now_tracing = REXCVAR_GET(d3d_trace_frames) > 0;
  if (now_tracing && !was_tracing) {
    std::lock_guard lock(g_dump_mutex);
    g_dumped.clear();
  }
  g_tracing.store(now_tracing, std::memory_order_relaxed);
}

// Logs the guest frame rate every 5 s (pacing check for renderer changes).
void FrameHeartbeat() {
  static const bool enabled = rex::cvar::Query<std::string>("renderer") == "dante";
  if (!enabled) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point window_start = Clock::now();
  static uint32_t frames = 0;
  ++frames;
  double seconds = std::chrono::duration<double>(Clock::now() - window_start).count();
  if (seconds < 5.0) return;
  REXLOG_INFO("FRAME-HB fps={:.1f} host_rendering={}", frames / seconds,
              rex::cvar::Query<bool>("gpu_host_rendering"));
  frames = 0;
  window_start = Clock::now();
}

// Draw calls nested in another hooked draw (BeginVertices inside DrawVerticesUP).
thread_local int g_draw_depth = 0;

inline renderer::CallArgs CaptureArgs(const PPCContext& ctx) {
  renderer::CallArgs args;
  for (int i = 0; i < 8; ++i) args.r[i] = ArgReg(ctx, 3 + i);
  args.f1 = ctx.f1.f64;
  return args;
}

inline void OnBeginVerticesReturned(const renderer::CallArgs& args, const PPCContext& ctx) {
  renderer::ObserveBeginVerticesReturned(ctx);
  renderer::AfterBeginVertices(args, ctx.r3.u32);
}

inline void OnEndVertices(const uint8_t* base) {
  renderer::ObserveEndVertices(base);
  renderer::AfterEndVertices(base);
}

}  // namespace

#define D3D_TRACE_HOOK_EX(addr, label, max_logged, skip_null_reg, dump_reg, dump_dwords) \
  REX_EXTERN(__imp__sub_##addr);                                                         \
  static TraceSlot g_slot_##addr{                                                        \
      label, 0x##addr, max_logged, skip_null_reg, dump_reg, dump_dwords};                \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                     \
  REX_HOOK_RAW(sub_##addr) {                                                             \
    renderer::FlushPendingInline(base);                                                  \
    SavedArgs saved;                                                                     \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                              \
    __imp__sub_##addr(ctx, base);                                                        \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                       \
  }

#define D3D_TRACE_HOOK(addr, label) D3D_TRACE_HOOK_EX(addr, label, 6, 0, 0, 0)

// Traced hook whose arguments are handed to a renderer=dante handler after the original.
#define D3D_NATIVE_HOOK(addr, label, max_logged, dump_reg, dump_dwords, handler)         \
  REX_EXTERN(__imp__sub_##addr);                                                         \
  static TraceSlot g_slot_##addr{label, 0x##addr, max_logged, 0, dump_reg, dump_dwords}; \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                     \
  REX_HOOK_RAW(sub_##addr) {                                                             \
    renderer::FlushPendingInline(base);                                                  \
    SavedArgs saved;                                                                     \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                              \
    renderer::CallArgs args = CaptureArgs(ctx);                                          \
    args.write_ptr_before = renderer::CommandBufferWritePtr(ctx.r3.u32, base);           \
    args.segment_switches_before = renderer::SegmentSwitchCount();                       \
    __imp__sub_##addr(ctx, base);                                                        \
    handler(args, base);                                                                 \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                       \
  }

// Draw hooks also feed the state verifier (state_verifier.cpp).
#define D3D_DRAW_HOOK_AFTER(addr, label, kind, after)                                  \
  REX_EXTERN(__imp__sub_##addr);                                                       \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0};                         \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                   \
  REX_HOOK_RAW(sub_##addr) {                                                           \
    renderer::FlushPendingInline(base);                                                \
    SavedArgs saved;                                                                   \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                            \
    renderer::TrackDrawFlush(ctx.r3.u32, base);                                        \
    if (g_draw_depth == 0) renderer::BeforeDraw(ctx.r3.u32, base);                     \
    uint32_t ticket = renderer::ObserveDrawBegin(renderer::DrawKind::kind, ctx, base); \
    renderer::CallArgs draw_args = CaptureArgs(ctx);                                   \
    ++g_draw_depth;                                                                    \
    __imp__sub_##addr(ctx, base);                                                      \
    --g_draw_depth;                                                                    \
    if (g_draw_depth == 0) {                                                           \
      renderer::AfterDraw(draw_args, renderer::DrawKind::kind, base);                  \
    }                                                                                  \
    renderer::ObserveDrawEnd(ticket, ctx, base);                                       \
    after;                                                                             \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                     \
  }
#define D3D_DRAW_HOOK(addr, label, kind) D3D_DRAW_HOOK_AFTER(addr, label, kind, (void)0)

#define D3D_TRACE_HOOK_AFTER(addr, label, after)               \
  REX_EXTERN(__imp__sub_##addr);                               \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0}; \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};           \
  REX_HOOK_RAW(sub_##addr) {                                   \
    renderer::FlushPendingInline(base);                        \
    SavedArgs saved;                                           \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);    \
    __imp__sub_##addr(ctx, base);                              \
    after;                                                     \
    if (log) TraceExit(g_slot_##addr, ctx, saved);             \
  }

#define D3D_TRACE_HOOK_BEFORE(addr, label, before)             \
  REX_EXTERN(__imp__sub_##addr);                               \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0}; \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};           \
  REX_HOOK_RAW(sub_##addr) {                                   \
    renderer::FlushPendingInline(base);                        \
    SavedArgs saved;                                           \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);    \
    before;                                                    \
    __imp__sub_##addr(ctx, base);                              \
    if (log) TraceExit(g_slot_##addr, ctx, saved);             \
  }

// Tron: Evolution (XDK D3D at 0x82BCxxxx-0x82BExxxx). Addresses matched against
// Dante's Inferno by instruction fingerprint (tools/funcmatch.py); see
// docs/TRON_D3D_MAP.md. EndVertices is inlined into the engine's code in Tron
// (nothing reads dev+13656 back), so a BeginVertices draw is issued at the next
// D3D call instead (FlushPendingInline at every hook's entry).
D3D_DRAW_HOOK(82BDF378, "DrawIndexedVertices", kIndexed)
D3D_DRAW_HOOK(82BDEF88, "DrawVertices", kAuto)
D3D_DRAW_HOOK_AFTER(82BDE590, "BeginVertices", kInline, OnBeginVerticesReturned(draw_args, ctx))
D3D_DRAW_HOOK(82BDEA28, "DrawVerticesUP", kInlineUP)
// BeginIndexedVertices flushes dirty constants: record the groups first.
REX_EXTERN(__imp__sub_82BDEA70);
REX_HOOK_RAW(sub_82BDEA70) {
  renderer::FlushPendingInline(base);
  renderer::TrackDrawFlush(ctx.r3.u32, base);
  renderer::CallArgs args = CaptureArgs(ctx);
  args.write_ptr_before = renderer::CommandBufferWritePtr(ctx.r3.u32, base);
  args.segment_switches_before = renderer::SegmentSwitchCount();
  __imp__sub_82BDEA70(ctx, base);
  renderer::AfterBeginIndexedVertices(args, base);
}

D3D_TRACE_HOOK(82BD8380, "SetVertexShaderConstantF")
D3D_TRACE_HOOK(82BD8458, "SetPixelShaderConstantF")

D3D_NATIVE_HOOK(82BE60C8, "ClearF", 6, 0, 0, renderer::AfterClearF)
D3D_NATIVE_HOOK(82BE67F8, "Resolve", 6, 6, 16, renderer::AfterResolve)  // r6 = dest texture
D3D_TRACE_HOOK(82BE7DA8, "EndTiling")
D3D_TRACE_HOOK(82BE7A50, "BeginTiling")
D3D_TRACE_HOOK(82BE78E0, "SetPredication")
D3D_TRACE_HOOK_EX(82BD54B0, "SetRenderTarget", 12, 0, 5, 24)  // (index, surface)
D3D_TRACE_HOOK_EX(82BD5800, "SetDepthStencilSurface", 12, 0, 4, 24)

D3D_TRACE_HOOK_EX(82BD8BC0, "SetVertexShader", 8, 4, 4, 32)
D3D_TRACE_HOOK_EX(82BD8818, "SetPixelShader", 8, 4, 4, 32)
D3D_TRACE_HOOK_EX(82BD5420, "SetIndices", 6, 4, 4, 12)
D3D_TRACE_HOOK_EX(82BD5300, "SetStreamSource", 12, 5, 5, 12)
D3D_TRACE_HOOK_EX(82BD7D70, "SetTexture", 16, 5, 5, 16)

D3D_TRACE_HOOK(82BD4758, "SetSampler_0EC8")
D3D_TRACE_HOOK(82BD4900, "SetSampler_1070")
D3D_TRACE_HOOK(82BD4BA0, "SetSampler_1310")
D3D_TRACE_HOOK(82BD4CC0, "SetSampler_LodBias")

D3D_TRACE_HOOK(82BCED58, "Release?")
D3D_TRACE_HOOK(82BCFCB8, "unk_5778(dev+60)")
D3D_TRACE_HOOK_EX(82BD0408, "Query_Issue", 6, 0, 3, 16)

D3D_TRACE_HOOK(82BE9060, "CreateDevice?")
D3D_TRACE_HOOK(82BE1D38, "unk_4AB0(pre-Swap)")

// Program flush: tracks the vertex shader microcode the GPU will run.
REX_EXTERN(__imp__sub_82BE44B0);
REX_HOOK_RAW(sub_82BE44B0) {
  renderer::FlushPendingInline(base);
  uint32_t dev = ctx.r3.u32;
  renderer::BeforeProgramFlush(dev, base);
  __imp__sub_82BE44B0(ctx, base);
  renderer::AfterProgramFlush(dev, base);
}

// Command buffer reservation (may start a new segment); returns the write pointer.
REX_EXTERN(__imp__sub_82BE10B0);
REX_HOOK_RAW(sub_82BE10B0) {
  uint32_t old_ptr = renderer::CommandBufferWritePtr(ctx.r3.u32, base);
  __imp__sub_82BE10B0(ctx, base);
  renderer::RecordSegmentSwitch(old_ptr, ctx.r3.u32);
  renderer::AfterCommandBufferReserve(ctx.r3.u32);
}
namespace {

void PauseFenceSpin(bool waiting) {
  thread_local uint32_t streak = 0;
  if (!waiting) {
    streak = 0;
    return;
  }
  if (++streak < 32) {
    std::this_thread::yield();
    return;
  }
#if defined(_WIN32)
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                             TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -2500;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(250));
}

}  // namespace

REX_EXTERN(__imp__sub_82BCF8C0);
REX_HOOK_RAW(sub_82BCF8C0) {
  __imp__sub_82BCF8C0(ctx, base);
  PauseFenceSpin(ctx.r3.u32 != 0);
}

REX_EXTERN(__imp__sub_82BDFC40);
REX_HOOK_RAW(sub_82BDFC40) {
  __imp__sub_82BDFC40(ctx, base);
  if (ctx.r3.u32) renderer::AfterCommandBufferReserve(ctx.r3.u32);
}

// Swap ends the frame for both the trace and the state verifier.
REX_EXTERN(__imp__sub_82BE2270);
static TraceSlot g_slot_82BE2270{"Swap", 0x82BE2270, 6, 0, 4, 16};
static SlotRegistrar g_reg_82BE2270{&g_slot_82BE2270};
REX_HOOK_RAW(sub_82BE2270) {
  renderer::FlushPendingInline(base);
  SavedArgs saved;
  bool log = TraceEnter(g_slot_82BE2270, ctx, base, saved);
  renderer::CallArgs args = CaptureArgs(ctx);
  __imp__sub_82BE2270(ctx, base);
  if (log) TraceExit(g_slot_82BE2270, ctx, saved);
  TraceFrameBoundary();
  renderer::ObserveFrameBoundary();
  renderer::AfterSwap(args, base, g_tracing.load(std::memory_order_relaxed));
  renderer::RenderDocOnGuestSwap();
  FrameHeartbeat();
}
