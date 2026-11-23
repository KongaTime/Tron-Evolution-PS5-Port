# Tron: Evolution — native renderer (PS5)

`games/tron-native` builds Tron with the native renderer from the Dante's
Inferno PS5 port (KongaTime/Dantes-Inferno-PS5-Port, `src/renderer`) instead of
the Xenos GPU emulation. The Xbox 360 D3D library's draws, clears, resolves and
swaps are rendered with DiligentCore on RADV; the Xenos command processor only
runs for sync (fences, interrupts, pacing). This removed the white flashes of
the emulated path and runs the busiest scenes at ~50 fps, the rest at 60.

## Tron-specific changes to the Dante renderer

- **Addresses** (`src/renderer/d3d_hooks.cpp`): Tron's XDK D3D library matched
  Dante's by instruction fingerprint (`tools/analysis/funcmatch.py`). Fields
  of the device at or above +13700 sit 4 bytes later than Dante's.
- **EndVertices is inlined** in Tron: a BeginVertices draw is issued at the
  next D3D call (`FlushPendingInline`). **BeginIndexedVertices** (UE3's
  post-process and composite) is an indexed draw with an inline vertex stream.
- **ClearF**: UE3 calls the inner float-color clear directly.
- **Surfaces**: height from the EDRAM byte size at +0x2C.
- **Formats**: resolve formats 54 (10:10:10:2) and EXPAND (float16); float
  formats ignore the fetch sign; `copy_dest_exp_bias` honoured (ColorScale pass;
  the HDR scene resolves with bias -3).
- **Textures**: whole-content hashing for small 8-bit textures and per-use
  checks for changing ones (UI glyph cache); per-frame view cache.

## Threading (`src/renderer/renderer.cpp`)

The game's render thread only captures each call (register state, constant
deltas from a model of the GPU constant registers fed by D3D's dirty bits,
shaders, surfaces, arguments) into a 4096-slot job ring; a worker thread does
the uploads and all Vulkan work. Guest memory the worker reads is protected as
on the 360: the command processor waits (`g_cp_fence_hook`) until the jobs
queued before a fence are done before writing that fence.

## Launcher (`games/tron-native/src/launcher`)

The title opens on a start screen (from the Dante port's launcher, with a
settings page like the Oogie's Revenge port's): PLAY, SETTINGS and EXIT. PLAY
starts the game from the disc files extracted in `/app0/gamedata` if present,
else from the player's Xbox 360 disc image in `/app0/dump/` (Disney Tron:
Evolution, USA/Europe; its `default.xex` is checked by XXH64), which the
runtime mounts in place. Nothing is extracted or copied. The screen stays up
while the game loads and is then handed to the renderer (same swap chain).

SETTINGS (saved in `/app0/userdata/launcher.txt`, applied at PLAY):
resolution 1440p / 4K (next start), frame rate 60 / 30, brightness 60-150%
(presentation gamma 1.19 at 100%, plus 0.20 more on blue, as tuned on the console; the game's own
gamma ramp is applied too), anti-aliasing (SMAA), shadow quality, and
PlayStation or Xbox button icons.

The screen art (`launcher-bg.png`, `-bar.png`, `-arrow.png`) and the font atlas
(`launcher-font.png` / `.txt`) go in the untracked
`games/tron-native/title-art/`; the build scripts copy them into the package.

## Achievement popups

Unlocks are recorded and saved by the runtime (`userdata/achievements/`). The
popup is Tron-styled (`games/tron-native/src/tron_toast.cpp`): an angled panel
with a glowing edge, the icon in a spinning ring, the launcher's lettering,
and an original synthesized cue (`tron_sound.cpp`; `tools/sound/tron_chime.py`
writes the same sound as a WAV). It is drawn through the renderer's overlay:
the runtime now sets up its ImGui layer for a sync-only graphics system too.

## Button icons

Tron draws each prompt from its own 32x32 texture (`button_glyphs.cpp`,
`kTronIcons`): the letter is replaced with the PlayStation symbol in the
icon's own style. Icons not in the table are logged ("not a known button
icon") with their content hash.

## Occlusion queries

The native renderer does not count samples for the game's occlusion queries;
the runtime answers them with a fixed count. Its default (1000 visible) made
the lens flares and light glare that UE3 hides behind walls draw through them
as a blue haze whenever the camera faced the light, so Tron reports 1 (hidden).
Real queries (counting the samples on the GPU and writing them to the query's
memory) would bring back flares where the light is really in view.

## Xbox 360 render target ranges

7e3 (`k_2_10_10_10_FLOAT`) targets are kept in 16-bit float; their output is
limited to 0..31.875 per draw (translator flag `kSysFlag_HostClampColor0To7e3`)
and again when resolved, and 16-bit fixed targets to -32..32, as the 360
stores them.

## Runtime

Built from a copy of Dante's patched ReXGlue (base f47ccdd) plus
`patches/rexglue-tron-native.patch`'s Tron additions: input-system lock,
event-driven WaitMultiple, the CP fence hook, and a TSC-based clock on the PS5.

## Building and testing

- `tools/native/build-runtime.sh` — runtime libraries (nat/rex -> nat/rex/out).
- `tools/native/build-native.sh` — the title.
- `tools/native/build-pgo.sh gen|use` — profile-guided builds; the instrumented
  title writes `/app0/pgo/tron.profraw` every 30 s; merge with
  `llvm-profdata-21 merge -o pgo/tron.profdata`.
- `tools/native/run-console.sh <name> <seconds>` — kill, upload, launch, fetch.
- Advanced settings in `/app0/tron-flags.txt` (`name=value`, any cvar), applied
  before the launcher's choices.
- The perf log lists slow frames (over 25 ms) with where the time went: the
  game's frame, the render worker (draws, present), GPU sync and texture loads.
