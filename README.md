<h2 align="center">
  <img src="src/assets/icons/de.berny23.virtual_surround_manager.svg" alt="Virtual Surround icon" width="160" height="160"/>
  <br>
  Virtual Surround
</h2>

<p align="center">
  <strong>3D sound for headphones on Linux</strong>
</p>

<p align="center">
<a href="https://www.buymeacoffee.com/Berny23" title="Donate to this project using Buy Me A Coffee"><img src="https://img.shields.io/badge/buy%20me%20a%20coffee-donate-yellow.svg" alt="Buy Me A Coffee donate button" /></a>
</p>

## Features

- Enable or disable seemlessly, without changing your default device
- Drop in your own **HRIR WAV files** as virtualization presets, same as **HeSuVi** on Windows
- Optional **stereo-to-7.1 upmixing** (FreeSurround/Real3D, bundled with the app): stereo audio is upmixed with Passive Surround Decoding, while real multichannel audio is detected and passed through unchanged
- Can be run in the background with a **system tray icon** (check the app's settings)
- Compatible with **EasyEffects** (and most similar apps)
- Written entirely using the **PipeWire C API**, so no config files or annoying service restarts are required
- Modern user interface, built with **KDE's Kirigami 6 framework**
- The user interface is Simplified Chinese (as requested for this fork)
- This project is **not using** vibe coding, AI agents or similar tools for generating unmaintainable slop

## Screenshot

<img width="552" height="580" alt="Screenshot of this app" src="dist/flatpak/screenshots/main-window-dark.webp" />

## Installation

### Flatpak (Recommended)

Compatible with any distribution.

<a href='https://flathub.org/en/apps/de.berny23.virtual_surround_manager'><img width='240' alt='Get Virtual Surround on Flathub' src='https://flathub.org/api/badge?svg&locale=en'/></a>

### AppImage

Compatible with any distribution.
Just [Download](https://github.com/Berny23/virtual-surround-manager/releases/latest) and run. Use the "x86_64" file if you're unsure.

### AUR

For Arch Linux and derivatives. Install with your AUR manager, like: `yay -S virtual-surround-manager`

### Third-party packages

These packages are maintained and provided by community members.

- [Fedora](https://software.opensuse.org/download/package?package=virtual-surround-manager&project=home%3AAndnoVember%3AFedora) (by AndnoVember)
- [Ubuntu/Debian](https://software.opensuse.org/download.html?package=virtual-surround-manager&project=home%3AAndnoVember%3ADebian) (by AndnoVember)
- [OpenSUSE](https://software.opensuse.org/download/package?package=virtual-surround-manager&project=home%3AAndnoVember%3ALXQt%3AQt6) (by AndnoVember)
- [Arch Linux](https://software.opensuse.org/download/package?package=virtual-surround-manager&project=home%3AAndnoVember%3AArch) (by AndnoVember)

## Usage

Just open the settings of your favorite game or media player and select real 7.1 or 5.1 audio output. Do not use any additional in-game virtualization setting (like a “headphone” or "3D audio" profile).

Please note: This app requires the PipeWire audio system. Recent Linux distributions use this by default, so don't worry about it.

### Usage with EasyEffects and similar apps

**TL;DR:**
<br>
In EasyEffects:
- Tick "Enable" for Virtual Surround Manager
- Tick "Exclude" for the actual game/app

Detailed explanation:

Both EasyEffects and Virtual Surround do essentially the same thing, they re-route audio. Meaning, **you must exclude the actual 7.1/5.1 game from routing in EasyEffects**, so my app can process it first!

**How routing works with EasyEffects for example:**
Game "Overwatch" (7.1/5.1 channels) → Virtual Surround Sink (7.1/5.1 channels) → Virtual Surround Source (2 channels) → Easy Effects Sink (2 channels) → All your EasyEffects plugins → Headphones (2 channels)

Note: The spatial virtualization and conversion to stereo happens between "Virtual Surround Sink" and "Virtual Surround Source".

**Apply all your effects on Virtual Surround in EasyEffects, not the game itself!**
If you want to exclude one app from Virtual Surround (like a music player or Discord), just tick "Enabled" for the specific app to route it directly through EasyEffects and not Virtual Surround.

### Check internal connections

To check if everything is working correctly, this is how audio routing should look like in [coppwr](https://flathub.org/de/apps/io.github.dimtpap.coppwr). If you don't use EasyEffects, the "Virtual Surround Source" should be directly connected to your headphones.
<br>
<img width="3401" height="849" alt="Bildschirmfoto_20260712_182628" src="https://github.com/user-attachments/assets/1bdb0cec-f318-4f50-9469-1ef502a2bc19" />

## Building

### Native

#### Dependencies for building the native package or AppImage

Arch Linux: `sudo pacman -S git ninja libpipewire base-devel extra-cmake-modules cmake kirigami ki18n kcoreaddons breeze kiconthemes qt6-base qt6-declarative qqc2-desktop-style`

For other distributions, just look up how the packages are called in your distro: https://pkgs.org

#### Native (for users)

1. Clone repository: `git clone https://github.com/Berny23/virtual-surround-manager.git`
2. Change directory: `cd virtual-surround-manager`
3. Prepare Build directory: `cmake -B build -G Ninja`
4. Build the project: `cmake --build build --config Release`
5. Install the project: `sudo cmake --install build --config Release`

#### Native (for developers)

1. Clone repository: `git clone https://github.com/Berny23/virtual-surround-manager.git`
2. Change directory: `cd virtual-surround-manager`
3. Prepare Build directory and set local install path: `cmake -B build -G Ninja --install-prefix ~/.local`
4. Build the project: `cmake --build build --config Debug`
5. Install the project: `cmake --install build --config Debug`

The build also compiles the bundled **FreeSurround (Real3D) LADSPA upmix plugin** from `third_party/freesurround_ladspa` and installs it next to the app into the system LADSPA directory (e.g. `/usr/lib/ladspa` or `/usr/lib64/ladspa`). It is only needed while stereo upmixing is enabled; without upmixing the plugin is not used at all. See `third_party/freesurround_ladspa/README.md` for details.

### Flatpak

1. Clone repository: `git clone https://github.com/Berny23/virtual-surround-manager.git`
2. Change directory: `cd virtual-surround-manager`
3. Build the flatpak: `flatpak-builder --force-clean --user --install-deps-from=flathub --repo=repo --install flatpak_build ./dist/flatpak/de.berny23.virtual_surround_manager.Devel.json`
4. Run the program: `flatpak run de.berny23.virtual_surround_manager`

### AppImage

1. Clone repository: `git clone https://github.com/Berny23/virtual-surround-manager.git`
2. Change directory: `cd virtual-surround-manager`
3. Build the AppImage: `./dist/appimage/build_virtual_surround_manager.sh` (x86-64-v3 on x86_64)
4. Make executable: `chmod +x ./build_appimage/virtual-surround-manager-*-x86_64-v3.AppImage`
5. Run the program: `./build_appimage/virtual-surround-manager-*-x86_64-v3.AppImage`

The AppImage bundles Qt/Kirigami, the PipeWire filter-chain module and SPA
plugins, the FreeSurround LADSPA plug-in and the HRIR presets. It still uses the
host's PipeWire/WirePlumber session and GPU driver; in a minimal session, start
PipeWire/WirePlumber first (or use `--appimage-extract-and-run` when FUSE is not
available).

## Development notes

### HRIR channel mapping

The channel indices of the HeSuVi 14-channel HRIR WAV files used in the filter
graph (see `create_virtual_surround_module` in `src/pipewire_manager.cpp`)
follow **HeSuVi's own convolution channel order**. Stock HeSuVi 2.0.0.1
(`hesuvi.txt`) feeds its convolvers in this order:

```
Channel: L0 R1 SL0 SR1 RL0 RR1 C0 R0 L1 SR0 SL1 RR0 RL1 C1
Copy:    L0=LVI R1=LVI SL0=SLVI SR1=SLVI RL0=RLVI RR1=RLVI C0=CVI
         R0=RVI L1=RVI SR0=SRVI SL1=SRVI RR0=RRVI RL1=RRVI C1=CVI
```

The naming is `[family prefix][ear][slot]`, so the resulting WAV channel map is:

| ch | speaker | ear | graph node |
|----|---------|-----|------------|
| 0  | FL | L | convFL_L |
| 1  | FL | R | convFL_R |
| 2  | SL | L | convSL_L |
| 3  | SL | R | convSL_R |
| 4  | RL | L | convRL_L |
| 5  | RL | R | convRL_R |
| 6  | C  | L | convFC_L |
| 7  | FR | R | convFR_R |
| 8  | FR | L | convFR_L |
| 9  | SR | R | convSR_R |
| 10 | SR | L | convSR_L |
| 11 | RR | R | convRR_R |
| 12 | RR | L | convRR_L |
| 13 | C  | R | convFC_R |

Channels 7-12 look "shuffled" only because HeSuVi lists the right-ear IRs of
the right-side speakers in that block.

The LFE channel is **not** convolved with the center HRIR anymore. It uses a
runtime-generated, HRIR-normalized unit-impulse IR of the same length and
format as the selected HRIR (`convLFE_L`/`convLFE_R`), so the LFE bypasses the HRIR coloring, is mixed
equally into both ears, and keeps the same convolver timing as the other
channels. Its 120 Hz LR4 lowpass and source-dependent gain are described below.

This was verified **from the IR files themselves** (no recordings involved):
for every real-measurement preset, the near-ear channel leads with the
physically expected ITD (front ~0.2 ms, rear ~0.3 ms, side ~0.5-0.7 ms) and is
louder by 6-12 dB; the center pair shows zero ITD and equal level. Synthesized
presets (Apple/Atmos) carry only the level asymmetry and zero ITD. To re-check
any preset, cross-correlate the channel pairs listed above instead of trusting
recording-side channel maps - those are frequently wrong.

Note: the mapping requires a 14-channel HRIR. Files with fewer channels (e.g.
the bundled `oal_dflt.wav`, which has 7) are rejected with an error and the
previous profile stays active; otherwise PipeWire would tolerate the
out-of-range channel indices and produce a wrong spatial image. Use 14-channel
HeSuVi WAVs.

### Stereo upmixing

The stereo-to-7.1 upmixer is the bundled FreeSurround LADSPA plugin
(`freesurround71_auto` label). It upmixes only while all non-L/R input
channels are silent; as soon as any other channel carries signal, real
multichannel content is assumed and the signal is passed through unchanged
(the native-LFE makeup described below still applies). The bypass delay ring
and the upmix core's FIFOs are kept primed, so switching between stereo and
multichannel content is gap-free instead of flushing a silent buffer.

### LFE / bass management

The low-frequency handling is selectable in Settings (`lfe_mode` in the
configuration file). All modes leave the main channels full-range (`use_lfe=0`),
so their bass keeps the selected HRIR's spatial and timbral response, matching
HeSuVi's stereo matrix (`SUB=0`) and the Dolby/Apple binaural renderers. Native
5.1/7.1 LFE enters the LFE path directly and its 2x (+6 dB) plus the
HRIR-normalized delta matches the Dolby binaural convention of +5.5 dB (10 dB
speaker makeup minus 4.5 dB for two-point-source headphone rendering). The
runtime delta IR is scaled to the selected HRIR's center-channel 40-80 Hz
magnitude, so the LFE level does not change with the profile.

- **Balanced (default)**: while upmixing stereo, the plugin writes a mono copy
  of the seven main outputs, scaled to −6 dB, into its LFE output; the graph's
  120 Hz 4th-order Linkwitz-Riley stage low-passes it. The bypass branch used
  for native 5.1/7.1 does not add this send, so real multichannel content never
  gets a duplicate bass. Measures +3.4/+3.0/+2.5/+1.9 dB at
  40/80/100/120 Hz with AppleSpatial and +3.6/+3.1/+2.6/+2.0 dB with
  `27_95_142`.
- **Reference**: native LFE only, no send; the most faithful and least
  fatiguing option.
- **Impact**: the same send at 0 dB instead of −6 dB (6 dB above balanced,
  2x amplitude). Measures +5.8/+5.3/+4.5/+3.5 dB with AppleSpatial and
  +6.1/+5.5/+4.7/+3.6 dB with `27_95_142`. It uses no static EQ; the previous
  +4 dB/100 Hz shelf was removed because it sounded muddy and did not improve
  punch.

The send and the LFE are aligned to the main HRIR bass per preset: the 120 Hz
LR4 has about 4.4 ms of group delay at 40-120 Hz, while each HRIR has its own
low-frequency delay. A 20-120 Hz fit (2.98 ms for AppleSpatial, 3.98 ms for
`27_95_142`, on the main or LFE path as appropriate) removes the old
−95…−171° phase difference above 90 Hz. Without it a louder send cancels
instead of reinforcing; presets whose phase is not a clean delay (for example
AtmosRenderer) are left unchanged and impact falls back to the balanced send
level.

The old 70-90 Hz FreeSurround redirect and its `VSM_BASS_MODE` /
`VSM_BASS_LEGACY` / `VSM_LFE_SEND_DB` environment variables were removed.
Important bass stays in the main channels; the LFE is an effects channel and
is discarded in a stereo downmix.

### Input detection display

While the main window is visible, it polls the upmixer every 250 ms and shows
whether the current stream is being upmixed as stereo, passed through as
multichannel, or skipped because the output is a built-in speaker. The detector
state (`vsm_auto_upmix_state`) and a processed-block counter
(`vsm_auto_blocks`, used as an "audio is really flowing" heartbeat) are exported
by the LADSPA plugin and read by the application; the text is not inferred from
the UI switches.

### Routing safeguards

- **Built-in speakers**: if the physical default output reports
  `device.form-factor=internal`, streams are released to that device and new
  streams following the default are not virtualized. Switching back to a USB
  DAC or headphones re-enables routing automatically. On laptops the internal
  analog sink often serves both speakers and the headphone jack, so plugging
  headphones into that jack is also skipped; USB/Bluetooth headphones are
  unaffected.
- **Default device**: the virtual bridge and internal effects sinks have
  `priority.session=0`. If WirePlumber nevertheless selects one as the default
  (manual choice or restored history), the application writes the previous
  physical device back to `default.audio.sink` and
  `default.configured.audio.sink`, so "revert default" cannot fall back to the
  virtual device.
- **Internal effects sink**: it is labelled "虚拟环绕声（内部效果）" to
  distinguish it from the app-facing "虚拟环绕声". If a stream is manually
  assigned to it (or to an older generation), the link monitor moves it back to
  the stable bridge, so a later graph rebuild cannot drop the stream.

### HRIR analysis

The graph analyses the selected 14-channel WAV at every rebuild (not a hardcoded
table): it prints and displays the delta-IR normalization gain, the 20-120 Hz
alignment delay, the absolute average/worst-channel phase residual after that
delay, and the automatically fitted all-pass correction (unity magnitude, phase
only). A new file is handled automatically; a 2 ms longer copy of `27_95_142`
changed the fitted delay from about 4.0 ms to 2.0 ms as expected. If the phase is
not a clean delay (slope residual >15 degrees, e.g. AtmosRenderer), no delay or
all-pass is inserted and impact falls back to the balanced send level. The
result appears above the profile list in the main window and in the journal.

### Crackling / brief dropouts

Two known sources of short interruptions during playback are addressed:

- The virtual device nodes set `node.latency = "1024/48000"`, which keeps the
  PipeWire graph quantum at 1024 samples or more whenever the chain is active.
  Without it, a low-latency application can drag the whole graph down to tiny
  quanta where the convolver chain is prone to xruns, and every quantum
  renegotiation is audible.
- The upmix plugin applies hysteresis before re-engaging upmixing after a
  bypass period (see `third_party/freesurround_ladspa/README.md`): this keeps
  content hovering at the stereo detection threshold from toggling the
  detector. The delay ring and upmix FIFOs stay primed, so the actual
  content-type switch itself is gap-free.
- Toggling upmixing on rebuilds the effect graph and needs a fresh 128 ms
  delay line. A normal HeSuVi HRIR is about 1 s long and its tail covers that
  refill, so changing profiles stays seamless. A very short HRIR (<128 ms,
  e.g. `27_95_142.wav`) leaves a ~90 ms gap when upmixing is switched on;
  upmixing off is a short overlap of the old delay line. This is a property of
  the FreeSurround latency, not a routing problem.

## Acknowledgements

- The [EasyEffects](https://github.com/wwmm/easyeffects) project for research on the API, because the PipeWire docs are quite lacking for beginners
- The [HeSuVi](https://hesuvi.net) project for their awesome collection of HRIR WAV files
- The [Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project and Christian Kothe's original FreeSurround decoder for the stereo upmixing plugin
