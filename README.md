## This fork is peak vibe code—so much vibe that mere humans just can’t keep up with reviewing it! Not because we don’t want to, mind you—we’re simply too lazy.🫠   
But hey, I personally kept an eye on it—so it’s probably not just random edits.  
  
The upstream is here: [Berny23/virtual-surround-manager](https://github.com/Berny23/virtual-surround-manager).  
  
<h2 align="center">
  <img src="src/assets/icons/de.berny23.virtual_surround_manager.svg" alt="Virtual Surround icon" width="160" height="160"/>
  <br>
  Virtual Surround
</h2>

<p align="center">
  <strong>3D sound for headphones on Linux</strong>
</p>

<p align="center">
<a href="https://www.buymeacoffee.com/Berny23" title="Donate to this project using Buy Me A Coffee (not this fork)"><img src="https://img.shields.io/badge/buy%20me%20a%20coffee-donate-yellow.svg" alt="Buy Me A Coffee donate button  (not this fork)" /></a>
</p>

## Fork additions

- 4096-point FreeSurround FFT (128 ms upmix latency) with automatic stereo ↔ multichannel detection and 20 ms crossfades
- **Mono 7-channel LFE send** (−6 / 0 dB, stereo upmix only) through the diffuse LFE path; no shelf on the front HRIRs
- Native LFE: 120 Hz Butterworth-4, **coherent seven-pair diffuse** IR (normalized to the center HRIR), per-preset LFE/main delay and all-pass alignment
- FreeSurround decode kept at its upstream mapping; `center_image` remains a no-op (not implemented upstream)
- Optional **reference-level matching** and **8× reconstructed-peak limiter**
- 44.1/96 kHz HRIR support with q14 resampling and 48 kHz graph alignment
- Application-owned startup gate and 100 ms fade (an intentional interruption, not lossless processing)

## Development notes

### Regression tests and reliability

```sh
cmake -B build -G Ninja -DBUILD_TESTING=ON -DBUILD_LADSPA_TEST_HOST=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests cover malformed/EXTENSIBLE HRIRs, bundled HRIR analysis, arbitrary and
variable audio block sizes, short multichannel events, limiter transients and
the original plugin regressions. They do not change the user's audio routing.
The 2026-11 audit also exercised separate PipeWire/WirePlumber/D-Bus sessions:
server disconnect/re-enable, user mute preservation, preset reload and MPRIS
pause-on-removal (including a forced-exit positive control).

- All LFE timbres validate the same 14-channel WAV format. Truncated frames,
  NaN/Inf, HRIRs over ten seconds or audio payloads over 256 MiB are rejected.
- Generated delta/diffuse IRs use float32 without clipping individual taps;
  quiet reference gains are no longer artificially raised to 0.05.
- Track-start gating uses application-owned graph gains, not user mute/volume.
- Server loss disables the reported state and safely retires modules. After
  the service returns, enabling the effect reconnects without restarting the app.
- `center_image` remains the upstream stored-but-unused parameter (no-op); the
  later C/FL/FR redistribution and global power normalization were removed so
  the FreeSurround decode keeps its original mapping and level law.
- Flatpak portal completion handling and AppImage autostart escaping have been
  updated, but their packaged runtime paths have not been integration-tested in
  this session. No claim of universal bug-freedom is made.

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

The LFE channel is **not** convolved with the center HRIR by default. It uses a
runtime-generated, HRIR-normalized float32 stereo LFE timbre IR: the **coherent
average of the seven left/right HRIR pairs**, scaled so its 40-80 Hz mean
magnitude equals the center-HRIR reference gain. This is the historical
"diffuse-field" approximation (the legacy configuration name is `diffuse`); it
is a coherent sum, not a power average, so direction cancellations are possible.
The unit impulse (`delta`) or the center HRIR pair (`center`) remain selectable.
The unit impulse adds no HRIR timbre and is equal in the two generated IR
channels; the complete path still contains filters/delays and the physical
headphone response. Neither option is a universal neutral headphone calibration.
Equal IR lengths do not imply equal phase or physical arrival time.

The main-channel mapping agrees with PipeWire's installed HeSuVi example.
ITD/level asymmetry checks can corroborate it for appropriate measured HRIRs,
but synthetic presets and strong reflections need separate interpretation.

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
and the upmix core's FIFOs are kept primed.

Both paths are exactly 6144 samples delayed at 48 kHz (128 ms), independently
of fixed or changing host block sizes. Detector decisions travel with the
audio through that delay, and even brief non-L/R events are remembered. The
paths blend over 20 ms; upmixing resumes after one second without non-L/R
content. The transition ramp still attenuates the first part of a newly
arriving native event; it no longer discards short events entirely.

### Acoustic model and limitations

The equations, authoritative-source scope, numerical counterexamples and
reproduction commands are documented in [the acoustic review](docs/acoustics.md).
The implementation is an HRIR-based headphone renderer with heuristic upmixing,
not a certified Dolby/DTS renderer or a headphone/SPL calibration system.

### LFE and optional bass enhancement

Main channels remain full-range through their HRIRs. Native LFE uses a 120 Hz
Butterworth-4 low-pass and 2x digital makeup (+6.0206 dB), applied once whether
upmixing is enabled or not. This is the project's level convention, **not a
universal +5.5 dB binaural standard**. The old +5.5 claim came from a secondary
article describing a particular Dolby rendering behavior. Apple's LFE “Off”
setting only documents disabled distance modelling; it does not specify this
project's LFE gain or require a delta IR.

The three modes control a **mono main-sum LFE send**, not a shelf on the front
HRIRs:

- `reference`: no send; native LFE processing remains.
- `balanced` (default): send at −6 dB (0.501).
- `impact`: send at 0 dB (1.0).

The send is the sum of the seven upmix main outputs, written into the auto
plugin's LFE output only in the upmix branch. The native bypass branch carries
the native LFE at 2× and no send. It is low-passed at 120 Hz BW4, rendered
through the coherent seven-pair diffuse IR with per-ear delay and
all-pass alignment, and mixed into the final bus. The added layer is
mono/diffuse, so it does not inherit the front HRIR
image and does not apply to native multichannel content. This is a taste
effect, not recovered LFE information or a standard crossover.

Native LFE is rendered independently: plugin bypass ×2 -> 120 Hz BW4 ->
coherent diffuse timbre IR -> per-ear alignment delay -> final mix.
The IR is scaled so its 40-80 Hz
mean magnitude equals the center-HRIR reference gain, keeping the native-LFE
calibration stable across presets. The main path gets one common alignment delay
(so L/R ITD is untouched) and the LFE path gets per-ear delays plus an optional
unity-magnitude all-pass. The fit is against the equal-weight coherent main sum
over 20-120 Hz. This is a reference fit, not a guarantee for
arbitrary content.

The 30°/15° send gate applies again: the send is disabled entirely when the
post-alignment residual exceeds 30°, and impact's 0 dB requires 15° (otherwise
it falls back to −6 dB). The gate values are fixed at graph build time.
`center_image` remains the upstream no-op: the decoder stores it but never
reads it. The later C/FL/FR redistribution and global main-channel
normalization were removed so the mapping and level law are the original
FreeSurround ones.

### Reference-level matching and reconstructed-peak limiting

Both options are off by default:

- `preset_level_match` estimates A-weighted output power for **seven
  uncorrelated, equal-power pink-noise inputs**, integrating 240 logarithmic
  frequency bins below Nyquist and matching the current list median (±8 dB).
  It sums channel powers instead of an arbitrary coherent C/FL/FR response.
  This is a static reference level, **not LUFS or a promise of equal programme
  loudness**. Only the overall trim changes.
- `output_limiter` is a stereo-linked lookahead limiter with an **8×, 48-tap
  polyphase windowed-sinc reconstructed-peak detector**. The audio stays at the
  graph rate; only the detector is oversampled. 5 ms lookahead (240 samples),
  −1.5 dBFS, 100 ms release. The host test checks 12 kHz phase-shifted,
  23 kHz near-Nyquist and 200 Hz square waves with an independent 32×/128-tap
  reconstruction; both sample and reconstructed peaks stay below −1.5 dBFS
  (plus 0.02 tolerance). It is a safety limiter, not a hearing-protection
  device.

The 0.30 output gain is −10.46 dB of nominal digital attenuation, not proof
against overload for every multichannel signal. Headphone SPL depends on the
whole reproduction chain and listening volume; the optional `impact` LFE send
(0 dB) is the most likely source of hot low-frequency peaks.

### Sample rate and rendering latency

The streams request 48 kHz; imported HRIRs are resampled by PipeWire at q14.
The analysis uses the file's time axis and designs filters for 48 kHz. This is
an approximation to the resampled graph and should be verified for unusual
files, not assumed exact because the quality setting is high.

A zero-block-latency convolver can still reproduce an IR's onset delay and
frequency-dependent group delay. Equal IR lengths do not align those delays.
The upmixer's 6144-sample latency (128 ms) is separate from HRIR/filter/system
latency. The filter-graph reports the average of the main and LFE branch latencies;
the mono LFE send and native-LFE alignment add at most ~4 ms of
preset-dependent main-path delay on top of the 128 ms upmixer. The
application-facing reported number therefore varies with the preset and was
not re-measured after the FFT rollback; measure it with pw-dump when it
matters.

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

Each graph rebuild analyses the selected file. The display shows the reference
gain, the LFE/main alignment delays (common main delay and per-ear LFE delays),
the wrapped phase residual and whether an all-pass was fitted. Zero or invalid
transfer functions cannot be called “aligned”; phase at zero magnitude is
undefined. A phase of 359° is a −1° error, not 359°. All-pass search uses that
circular distance as well.

The reference phase is not the phase of every possible programme. It is an
equal-weight coherent sum; FreeSurround's actual channel weights depend on the
content. Diagnostics are estimates, not acoustic certification.

### User HRIR 27_95_142 parameters

The bundled `src/assets/hrir_wav/27_95_142.wav` was generated from the user's
April 2026 anthropometric inputs. The exact values, generator settings, source
screenshot and SHA-256 hashes are preserved in
[`docs/hrir/27_95_142/`](docs/hrir/27_95_142/README.md). Do not confuse them with
the earlier January `HRTF1` inputs, which are a different measurement.

### Startup and switching trade-offs

- A new stream triggers application-owned post-graph gains: 150 ms hold while
  upmixing, 30 ms otherwise, then a 100 ms fade. User mute/volume are not changed.
  This intentionally suppresses audio and can affect other streams sharing the
  graph; it is not lossless, and a source transient is not necessarily a defect.
- `node.latency = "1024/48000"` is a **suggestion**, not a minimum quantum.
  PipeWire may choose a smaller or forced larger quantum. The plugin supports
  arbitrary block partitions instead of relying on this property for correctness.
- Automatic content switching delays decisions alongside audio and uses a
  20 ms crossfade. The initial part of a native event may be attenuated during
  the transition. One second of stereo hysteresis reduces repeated switching.
- Toggling upmix on/off and LFE mode are runtime controls and do **not** rebuild
  the graph. Rebuilding for HRIR/timbre/level-match/limiter changes links the
  new graph first and re-targets the bridge; the old graph can still play its
  IR tail while the new one fills its 128 ms upmix delay. With very short HRIRs
  this leaves an approximately 64–90 ms gap; there is no sample-level crossfade
  between generations. This is a switching limitation, not a steady-state level
  error.

## Acknowledgements

- The [EasyEffects](https://github.com/wwmm/easyeffects) project for research on the API, because the PipeWire docs are quite lacking for beginners
- The [HeSuVi](https://hesuvi.net) project for their awesome collection of HRIR WAV files
- The [Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project and Christian Kothe's original FreeSurround decoder for the stereo upmixing plugin
