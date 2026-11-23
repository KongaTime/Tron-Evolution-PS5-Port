#include "guest_gpu_state.h"

#include <rex/ppc/context.h>

#include <cstring>

namespace renderer {

using namespace guest;

namespace {

// Per vec4 (0-511): buffer address loaded by 0x8291D778, 0 = shadow.
std::array<uint32_t, 512> g_constant_source{};

// Literal constants written to the GPU by earlier program flushes. D3D loads
// them without updating its shadow, so they stay in the GPU registers (and
// later shaders may read them) until a dirty-group flush or buffer load
// overwrites that vec4.
struct StaleLiteral {
  bool valid = false;
  uint32_t value[4] = {};
};
std::array<StaleLiteral, 512> g_stale_literals{};
// Bitmaps of the entries above that are set, so capture skips the empty ones.
uint64_t g_stale_bits[8] = {};
// Constant register model (threaded capture): values as the GPU holds them.
std::array<uint32_t, 2048> g_const_view{};
bool g_const_view_init = false;
uint64_t g_flush_pending[2] = {};   // dirty groups (VS, PS) seen before draw flushes
uint64_t g_view_changed[8] = {};    // vec4s changed since the last job
uint64_t g_source_bits[8] = {};
inline void SetBit(uint64_t* bits, uint32_t i, bool on) {
  if (on) {
    bits[i >> 6] |= uint64_t(1) << (i & 63);
  } else {
    bits[i >> 6] &= ~(uint64_t(1) << (i & 63));
  }
}

// Literal constants loaded by the program flush (0x827EA218): records
// {u16 vec4, u16 dwords, u32 offset} at blk + [blk+20] + 20, data at base + offset.
uint32_t ApplyShaderLiterals(const uint8_t* base, uint32_t blk, uint32_t ucode_base,
                             GuestRegisters& state) {
  if (!IsPlausibleAddress(blk) || !IsPlausibleAddress(ucode_base)) return 0;
  uint32_t table_offset = Load32(base, blk + 20);
  if (!table_offset) return 0;
  uint32_t table = blk + table_offset;
  uint32_t rec = table + 20;
  uint32_t end = rec + Load32(base, table + 16);
  uint32_t applied = 0;
  for (uint32_t n = 0; rec < end && n < 256; ++n) {
    uint32_t vec4_index = Load16(base, rec);
    uint32_t dword_count = Load16(base, rec + 2);
    if (!dword_count || dword_count > 1024) break;
    uint32_t data = ucode_base + Load32(base, rec + 4);
    rec += 8;
    if (!IsPlausibleAddress(data)) break;
    for (uint32_t d = 0; d < dword_count; ++d) {
      uint32_t value = Load32(base, data + d * 4);
      uint32_t vec4 = vec4_index + d / 4;
      if (vec4 < 512) {
        g_stale_literals[vec4].valid = true;
        g_stale_literals[vec4].value[d & 3] = value;
        SetBit(g_stale_bits, vec4, true);
      }
      int index = FloatConstantIndex(vec4_index * 4 + d);
      if (index < 0) continue;
      state[index] = value;
      ++applied;
    }
  }
  return applied;
}

}  // namespace

void TrackDrawFlush(uint32_t device, const uint8_t* base) {
  for (uint32_t type = 0; type < 2; ++type) {
    uint32_t mask_addr = device + type * 8;
    uint64_t dirty = (uint64_t(Load32(base, mask_addr)) << 32) | Load32(base, mask_addr + 4);
    if (!dirty) continue;
    g_flush_pending[type] |= dirty;
    for (uint32_t group = 0; group < 64; ++group) {
      if (!((dirty >> (63 - group)) & 1)) continue;
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t v = type * 256 + group * 4 + i;
        g_constant_source[v] = 0;
        g_stale_literals[v].valid = false;
        SetBit(g_stale_bits, v, false);
        SetBit(g_source_bits, v, false);
      }
    }
  }
}

void TrackConstantBufferLoad(const PPCContext& ctx, const uint8_t* base) {
  uint32_t start = (ctx.r4.u32 << 8) + ctx.r5.u32;
  uint32_t count = ctx.r6.u32;
  uint32_t buffer = ctx.r7.u32;
  if (!IsPlausibleAddress(buffer)) return;
  uint32_t source = Load32(base, buffer + 0x18) + ctx.r8.u32 * 16;
  for (uint32_t i = 0; i < count && start + i < 512; ++i) {
    g_constant_source[start + i] = source + i * 16;
    g_stale_literals[start + i].valid = false;
    SetBit(g_stale_bits, start + i, false);
    SetBit(g_source_bits, start + i, true);
  }
}

DrawStateStats CaptureDrawState(uint32_t device, const uint8_t* base, GuestRegisters& out) {
  DrawStateStats stats;
  uint32_t index = 0;
  // One pass: load, byte-swap, store (vectorized).
  for (const auto& range : kShadowRanges) {
    const uint8_t* src = Ptr(base, device + range.offset);
    uint32_t* dst = &out[index];
    for (uint32_t i = 0; i < range.count; ++i) {
      uint32_t v;
      std::memcpy(&v, src + i * 4, 4);
      dst[i] = __builtin_bswap32(v);
    }
    index += range.count;
  }
  for (uint32_t w = 0; w < 8; ++w) {
    for (uint64_t bits = g_stale_bits[w]; bits; bits &= bits - 1) {
      uint32_t vec4 = w * 64 + uint32_t(__builtin_ctzll(bits));
      std::memcpy(&out[kFloatConstantIndex + vec4 * 4], g_stale_literals[vec4].value,
                  sizeof(g_stale_literals[vec4].value));
    }
  }
  for (uint32_t w = 0; w < 8; ++w) for (uint64_t bits = g_source_bits[w]; bits; bits &= bits - 1) {
    uint32_t vec4 = w * 64 + uint32_t(__builtin_ctzll(bits));
    uint32_t source = g_constant_source[vec4];
    if (!source) continue;
    for (uint32_t c = 0; c < 4; ++c) {
      out[kFloatConstantIndex + vec4 * 4 + c] = Load32(base, source + c * 4);
    }
    stats.buffer_constant_dwords += 4;
  }
  uint32_t vs = Load32(base, device + kDevVertexShader);
  uint32_t ps = Load32(base, device + kDevPixelShader);
  if (IsPlausibleAddress(vs)) {
    stats.literal_dwords += ApplyShaderLiterals(base, vs + 872, Load32(base, vs + 0x20), out);
  }
  if (IsPlausibleAddress(ps)) {
    stats.literal_dwords += ApplyShaderLiterals(base, ps + 40, Load32(base, ps + 0x18), out);
  }
  return stats;
}

namespace {
inline void MarkVec4(uint32_t vec4) { g_view_changed[vec4 >> 6] |= uint64_t(1) << (vec4 & 63); }

// Literal constants into the model (as ApplyShaderLiterals into a state).
void ApplyShaderLiteralsToView(const uint8_t* base, uint32_t blk, uint32_t ucode_base) {
  if (!IsPlausibleAddress(blk) || !IsPlausibleAddress(ucode_base)) return;
  uint32_t table_offset = Load32(base, blk + 20);
  if (!table_offset) return;
  uint32_t table = blk + table_offset;
  uint32_t rec = table + 20;
  uint32_t end = rec + Load32(base, table + 16);
  for (uint32_t n = 0; rec < end && n < 256; ++n) {
    uint32_t vec4_index = Load16(base, rec);
    uint32_t dword_count = Load16(base, rec + 2);
    if (!dword_count || dword_count > 1024) break;
    uint32_t data = ucode_base + Load32(base, rec + 4);
    rec += 8;
    if (!IsPlausibleAddress(data)) break;
    for (uint32_t d = 0; d < dword_count; ++d) {
      uint32_t c = vec4_index * 4 + d;
      if (c >= 2048) continue;
      uint32_t value = Load32(base, data + d * 4);
      if (g_const_view[c] != value) {
        g_const_view[c] = value;
        MarkVec4(c >> 2);
      }
    }
  }
}
}  // namespace

void CaptureDrawStateDelta(uint32_t device, const uint8_t* base, GuestRegisters& out) {
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    bool constants = range.reg >= 0x4000 && range.reg < 0x4800;
    if (!constants) {
      const uint8_t* src = Ptr(base, device + range.offset);
      uint32_t* dst = &out[index];
      for (uint32_t i = 0; i < range.count; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        dst[i] = __builtin_bswap32(v);
      }
    }
    index += range.count;
  }
  // Flushed groups: the shadow's values reach the GPU registers.
  auto copy_vec4 = [&](uint32_t vec4) {
    uint32_t offset = vec4 < 256 ? 1920 + vec4 * 16 : 6016 + (vec4 - 256) * 16;
    const uint8_t* src = Ptr(base, device + offset);
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t v;
      std::memcpy(&v, src + c * 4, 4);
      v = __builtin_bswap32(v);
      if (g_const_view[vec4 * 4 + c] != v) {
        g_const_view[vec4 * 4 + c] = v;
        MarkVec4(vec4);
      }
    }
  };
  bool first_capture = !g_const_view_init;
  if (!g_const_view_init) {
    g_const_view_init = true;
    for (uint32_t vec4 = 0; vec4 < 512; ++vec4) copy_vec4(vec4);
    for (auto& w : g_view_changed) w = ~uint64_t(0);
  }
  bool flushed = (g_flush_pending[0] | g_flush_pending[1]) != 0;
  for (uint32_t type = 0; type < 2; ++type) {
    for (uint64_t dirty = g_flush_pending[type]; dirty; dirty &= dirty - 1) {
      uint32_t bit = uint32_t(__builtin_ctzll(dirty));
      uint32_t group = 63 - bit;  // bit 63-g = group g
      for (uint32_t i = 0; i < 4; ++i) copy_vec4(type * 256 + group * 4 + i);
    }
    g_flush_pending[type] = 0;
  }
  // Constants loaded from buffers (not used by Tron, kept for completeness).
  for (uint32_t w = 0; w < 8; ++w) for (uint64_t bits = g_source_bits[w]; bits; bits &= bits - 1) {
    uint32_t vec4 = w * 64 + uint32_t(__builtin_ctzll(bits));
    uint32_t source = g_constant_source[vec4];
    if (!source) continue;
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t v = Load32(base, source + c * 4);
      if (g_const_view[vec4 * 4 + c] != v) {
        g_const_view[vec4 * 4 + c] = v;
        MarkVec4(vec4);
      }
    }
  }
  // Literal constants only change the model when the shader changed or a
  // flush may have overwritten them (the program flush reloads them then too).
  static uint32_t last_vs = 0, last_ps = 0, last_vs_ucode = 0, last_ps_ucode = 0;
  uint32_t vs = Load32(base, device + kDevVertexShader);
  uint32_t ps = Load32(base, device + kDevPixelShader);
  uint32_t vs_ucode = IsPlausibleAddress(vs) ? Load32(base, vs + 0x20) : 0;
  uint32_t ps_ucode = IsPlausibleAddress(ps) ? Load32(base, ps + 0x18) : 0;
  bool reapply = flushed || first_capture || vs != last_vs || ps != last_ps || vs_ucode != last_vs_ucode ||
                 ps_ucode != last_ps_ucode;
  last_vs = vs;
  last_ps = ps;
  last_vs_ucode = vs_ucode;
  last_ps_ucode = ps_ucode;
  if (!reapply) return;
  if (IsPlausibleAddress(vs)) ApplyShaderLiteralsToView(base, vs + 872, vs_ucode);
  if (IsPlausibleAddress(ps)) ApplyShaderLiteralsToView(base, ps + 40, ps_ucode);
}

void TakeConstantDelta(ConstantDelta& delta) {
  delta.count = 0;
  for (uint32_t w = 0; w < 8; ++w) {
    for (uint64_t bits = g_view_changed[w]; bits; bits &= bits - 1) {
      uint32_t vec4 = w * 64 + uint32_t(__builtin_ctzll(bits));
      delta.index[delta.count] = uint16_t(vec4);
      std::memcpy(delta.values[delta.count], &g_const_view[vec4 * 4], 16);
      ++delta.count;
    }
    g_view_changed[w] = 0;
  }
}

}  // namespace renderer
