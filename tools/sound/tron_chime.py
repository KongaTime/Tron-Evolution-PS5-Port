# Original synthesized "achievement unlocked" cue in a Tron-like style:
# a rising filtered sweep, a detuned saw chord, then an echoing square arpeggio.
import numpy as np, wave
SR = 48000
def t(sec): return np.arange(int(sec * SR)) / SR
def saw(f, tt): return 2 * ((f * tt) % 1.0) - 1
def sq(f, tt): return np.sign(np.sin(2 * np.pi * f * tt))
def lowpass(x, cutoff):  # one-pole, cutoff array or scalar (Hz)
    c = np.broadcast_to(np.asarray(cutoff, float), x.shape)
    a = 1 - np.exp(-2 * np.pi * c / SR)
    y = np.zeros_like(x); s = 0.0
    for i in range(len(x)):
        s += a[i] * (x[i] - s); y[i] = s
    return y
total = 1.9
out = np.zeros((int(total * SR), 2))
def add(sig, start, pan=0.0, gain=1.0):
    i = int(start * SR); n = min(len(sig), len(out) - i)
    out[i:i + n, 0] += sig[:n] * gain * (1 - pan) ; out[i:i + n, 1] += sig[:n] * gain * (1 + pan)
# 1. rising sweep (noise + saw glide), 0.22 s
tt = t(0.22)
glide = 220 * (8 ** (tt / 0.22))
ph = np.cumsum(glide) / SR
sweep = 0.5 * (2 * (ph % 1) - 1) + 0.35 * np.random.default_rng(3).standard_normal(len(tt))
sweep = lowpass(sweep, 300 + 6000 * (tt / 0.22) ** 2) * np.sin(np.pi * tt / 0.22) ** 0.7
add(sweep, 0.0, gain=0.55)
# 2. detuned saw chord stab (D minor add9: D3 A3 D4 F4 E5), filter opening then closing
tt = t(1.3)
env = np.minimum(tt / 0.01, 1) * np.exp(-tt * 2.6)
chord = np.zeros_like(tt)
for f in (146.83, 220.0, 293.66, 349.23, 659.25):
    for d in (-0.35, 0.0, 0.35):
        chord += saw(f * 2 ** (d / 100 * 3), tt)
chord /= 15
cut = 500 + 4500 * np.exp(-tt * 3.5)
chord = lowpass(chord, cut) * env
add(chord, 0.2, gain=0.9)
# 3. square arpeggio with echo: D5 F5 A5 D6 E6
notes = [587.33, 698.46, 880.0, 1174.66, 1318.51]
for k, f in enumerate(notes):
    tt = t(0.11)
    blip = sq(f, tt) * 0.5 + saw(f * 2.003, tt) * 0.3
    blip = lowpass(blip, 3500) * np.exp(-tt * 28) * np.minimum(tt / 0.003, 1)
    for e, (dl, g) in enumerate([(0, 1), (0.16, 0.45), (0.32, 0.2)]):
        add(blip, 0.28 + k * 0.065 + dl, pan=(-0.4 if e % 2 else 0.4) if e else 0, gain=0.32 * g)
# master: soft clip, -3 dBFS peak
out = np.tanh(out * 1.3)
out *= 0.7 / np.max(np.abs(out))
fade = np.minimum(1, (len(out) - np.arange(len(out))) / (0.05 * SR))[:, None]
out *= fade
pcm = (out * 32767).astype('<i2')
with wave.open('tron_achievement.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print('ok', len(out) / SR, 's')
