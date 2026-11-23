// Bring-up diagnostics: where guest threads sleep.
//
// sub_82F688F8 is the game's only caller of KeDelayExecutionThread (its SleepEx).
// Every 2000th sleep on a thread logs that thread's guest backtrace: the LR and
// the saved LRs found by walking the PPC stack back-chain (saved LR at
// back_chain - 8, as the 360 compiler lays frames out).

#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>


REX_EXTERN(__imp__sub_82F688F8);

namespace {
inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  __builtin_memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
thread_local uint32_t t_sleeps = 0;
std::atomic<int> g_reports{0};

std::string GuestBacktrace(PPCContext& ctx, uint8_t* base) {
  std::string out = fmt::format("lr {:08X}", static_cast<uint32_t>(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 10 && sp >= 0x70000000 && sp < 0x80000000; ++i) {
    const uint32_t back = LoadU32(base, sp);
    if (back <= sp || back >= 0x80000000) break;
    out += fmt::format(" < {:08X}", LoadU32(base, back - 8));
    // The outermost frame's back chain is the stack's top: a page boundary with
    // nothing mapped above it, so stop before reading there.
    if ((back & 0xFFF) == 0) break;
    sp = back;
  }
  return out;
}
}  // namespace

REX_HOOK_RAW(sub_82F688F8) {
  if ((++t_sleeps % 2000) == 1 && g_reports.fetch_add(1) < 400) {
    REXLOG_INFO("TRON sleep#{} r3={} [{}]", t_sleeps, ctx.r3.s32, GuestBacktrace(ctx, base));
  }
  return __imp__sub_82F688F8(ctx, base);
}
