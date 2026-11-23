// Tron: Evolution frame rate.
//
// The XDK D3D swap callback (0x82BE1B88, run by the command processor once per
// Swap) queues the next flip `interval` vblanks after the last one, with the
// presentation interval in bits 8-11 of its argument. The game presents with
// interval 2 (30 fps at 60 Hz). tron_present_interval overrides it.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_INT32(tron_present_interval, 1, "Tron",
                     "Presentation interval for the game flips (default 1 = 60 fps): -1 = the game own (2 = "
                     "30 fps), 1 = every vblank (60 fps), 0 = immediately");

REX_EXTERN(__imp__sub_82BE1B88);
REX_HOOK_RAW(sub_82BE1B88) {
  static std::atomic<uint32_t> logged{0};
  uint32_t arg = ctx.r3.u32;
  uint32_t interval = (arg >> 8) & 0xF;
  int32_t wanted = REXCVAR_GET(tron_present_interval);
  if (logged.fetch_add(1) < 3) {
    REXLOG_INFO("TRON: swap callback arg {:08X} (presentation interval {}), override {}", arg,
                interval, wanted);
  }
  if (wanted >= 0 && wanted <= 15) {
    ctx.r3.u64 = (arg & ~0xF00u) | (uint32_t(wanted) << 8);
  }
  __imp__sub_82BE1B88(ctx, base);
}
