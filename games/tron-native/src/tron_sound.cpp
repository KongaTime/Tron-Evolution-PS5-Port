// The achievement cue (see tron_sound.h). Same recipe as tools/sound/tron_chime.py (which writes a WAV to listen to).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tron_sound.h"

#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <numbers>
#include <random>
#include <vector>

namespace tron {
namespace {

constexpr int kRate = 48000;
constexpr float kPi = std::numbers::pi_v<float>;

std::vector<int16_t> g_cue;          // interleaved stereo
std::atomic<bool> g_restart{false};  // set by PlayAchievementSound
size_t g_position = ~size_t(0);       // audio thread only: frame playing, or idle
std::once_flag g_prepared;
bool g_output = false;

float Saw(float f, float t) { return 2 * std::fmod(f * t, 1.0f) - 1; }
float Square(float f, float t) { return std::sin(2 * kPi * f * t) >= 0 ? 1.0f : -1.0f; }

// One-pole low-pass with a per-sample cutoff.
struct LowPass {
  float state = 0;
  float operator()(float x, float cutoff) {
    state += (1 - std::exp(-2 * kPi * cutoff / kRate)) * (x - state);
    return state;
  }
};

std::vector<int16_t> Synthesize() {
  const size_t frames = size_t(1.9 * kRate);
  std::vector<float> left(frames), right(frames);
  auto add = [&](const std::vector<float>& sig, float start, float pan, float gain) {
    const size_t at = size_t(start * kRate);
    for (size_t i = 0; i < sig.size() && at + i < frames; ++i) {
      left[at + i] += sig[i] * gain * (1 - pan);
      right[at + i] += sig[i] * gain * (1 + pan);
    }
  };
  // 1. Rising sweep: a gliding saw and noise, opening low-pass.
  {
    const size_t n = size_t(0.22f * kRate);
    std::vector<float> sig(n);
    std::mt19937 rng(3);
    std::normal_distribution<float> noise;
    LowPass lp;
    float phase = 0;
    for (size_t i = 0; i < n; ++i) {
      const float t = float(i) / kRate, k = t / 0.22f;
      phase += 220 * std::pow(8.0f, k) / kRate;
      const float x = 0.5f * (2 * std::fmod(phase, 1.0f) - 1) + 0.35f * noise(rng);
      sig[i] = lp(x, 300 + 6000 * k * k) * std::pow(std::sin(kPi * k), 0.7f);
    }
    add(sig, 0, 0, 0.55f);
  }
  // 2. Detuned saw chord (D minor add9) with a closing filter.
  {
    const size_t n = size_t(1.3f * kRate);
    std::vector<float> sig(n);
    const float notes[] = {146.83f, 220.0f, 293.66f, 349.23f, 659.25f};
    LowPass lp;
    for (size_t i = 0; i < n; ++i) {
      const float t = float(i) / kRate;
      float x = 0;
      for (float f : notes) {
        for (float d : {-0.35f, 0.0f, 0.35f}) x += Saw(f * std::pow(2.0f, d * 3 / 100), t);
      }
      x /= 15;
      const float env = std::min(t / 0.01f, 1.0f) * std::exp(-t * 2.6f);
      sig[i] = lp(x, 500 + 4500 * std::exp(-t * 3.5f)) * env;
    }
    add(sig, 0.2f, 0, 0.9f);
  }
  // 3. Square arpeggio with a ping-pong echo.
  {
    const float notes[] = {587.33f, 698.46f, 880.0f, 1174.66f, 1318.51f};
    const size_t n = size_t(0.11f * kRate);
    for (int k = 0; k < 5; ++k) {
      std::vector<float> sig(n);
      LowPass lp;
      for (size_t i = 0; i < n; ++i) {
        const float t = float(i) / kRate, f = notes[k];
        const float x = Square(f, t) * 0.5f + Saw(f * 2.003f, t) * 0.3f;
        sig[i] = lp(x, 3500) * std::exp(-t * 28) * std::min(t / 0.003f, 1.0f);
      }
      const float echoes[3][3] = {{0, 1, 0}, {0.16f, 0.45f, -0.4f}, {0.32f, 0.2f, 0.4f}};
      for (const auto& e : echoes) add(sig, 0.28f + k * 0.065f + e[0], e[2], 0.32f * e[1]);
    }
  }
  // Master: soft clip, normalize to 0.7, fade the tail.
  float peak = 0;
  for (size_t i = 0; i < frames; ++i) {
    left[i] = std::tanh(left[i] * 1.3f);
    right[i] = std::tanh(right[i] * 1.3f);
    peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
  }
  std::vector<int16_t> out(frames * 2);
  const float scale = peak > 0 ? 0.7f / peak : 0;
  for (size_t i = 0; i < frames; ++i) {
    const float fade = std::min(1.0f, float(frames - i) / (0.05f * kRate));
    out[i * 2] = int16_t(std::clamp(left[i] * scale * fade, -1.0f, 1.0f) * 32767);
    out[i * 2 + 1] = int16_t(std::clamp(right[i] * scale * fade, -1.0f, 1.0f) * 32767);
  }
  return out;
}

void Fill(int16_t* frames, int count, void*) {
  if (g_restart.exchange(false, std::memory_order_acq_rel)) g_position = 0;
  const size_t total = g_cue.size() / 2;
  for (int i = 0; i < count; ++i) {
    if (g_position < total) {
      frames[i * 2] = g_cue[g_position * 2];
      frames[i * 2 + 1] = g_cue[g_position * 2 + 1];
      ++g_position;
    } else {
      frames[i * 2] = frames[i * 2 + 1] = 0;
    }
  }
}

}  // namespace

void PrepareAchievementSound() {
  std::call_once(g_prepared, [] {
    g_cue = Synthesize();
    g_output = audio_start(Fill, nullptr);
  });
}

void PlayAchievementSound() {
  PrepareAchievementSound();
  if (g_output) g_restart.store(true, std::memory_order_release);
}

}  // namespace tron
