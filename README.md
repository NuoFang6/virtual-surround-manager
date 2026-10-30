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

- Selectable **diffuse / δ / center LFE timbre**, fixed 120 Hz Butterworth-4 low-pass and per-ear HRIR/main phase alignment
- 20 ms crossfade for automatic stereo ↔ multichannel switching
- Optional **preset loudness matching** and **output limiter**
- 44.1/96 kHz HRIR support with q14 resampling and 48 kHz graph alignment
- Output gate with a 100 ms fade on track changes to prevent first-sample pops

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

The LFE channel is **not** convolved with the center HRIR by default. It uses a
runtime-generated, HRIR-normalized LFE timbre IR of the same length and format
as the selected HRIR (`convLFE_L`/`convLFE_R`): a diffuse-field average (the
default) or, selectable in Settings, a unit impulse or the center HRIR pair.
Only the unit impulse is strictly uncolored and equal L/R; the diffuse average
and the center pair deliberately apply HRIR-derived timbre and can differ
slightly between the ears. All variants keep the same convolver timing as the
other channels. The 120 Hz 4th-order Butterworth lowpass, per-ear alignment and
source-dependent gain are described below.

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
and the upmix core's FIFOs are kept primed.

Both paths are 128 ms delayed. Before switching to bypass, the detector waits
for the native content to traverse that delay line (plus the crossfade length),
then blends the two paths over 20 ms. This removes the old hard-switch click
and the 85 ms centre-channel hole; without the wait, the bypass path still
contains the previous (stereo) content when the switch starts. The 20 ms
crossfade also covers the return to upmixing after one second of pure stereo.

### LFE / bass management

The low-frequency handling is selectable in Settings (`lfe_mode` in the
configuration file). All modes leave the main channels full-range (`use_lfe=0`),
so their bass keeps the selected HRIR's spatial and timbral response, matching
HeSuVi's stereo matrix (`SUB=0`) and the Dolby/Apple binaural renderers. Native
5.1/7.1 LFE enters the LFE path directly and its 2x (+6 dB) plus the
HRIR-normalized LFE timbre IR (diffuse-field by default) is calibrated against
the Dolby binaural convention of +5.5 dB (10 dB speaker makeup minus 4.5 dB for
two-point-source headphone rendering). With the BW4 low-pass it measures
**+6.0 dB at 60 Hz**, 0.5 dB above that reference; the earlier LR4 produced
+5.5 dB. That runtime IR is scaled to the selected HRIR's center-channel
40-80 Hz magnitude, so the average 40-80 Hz LFE level is comparable across
profiles; individual presets can still deviate at a single frequency (for
example the synthetic AtmosRenderer profiles at 60 Hz).

- **Balanced (default)**: while upmixing stereo, the plugin writes a mono copy
  of the seven main outputs, scaled to −6 dB, into its LFE output; the graph's
  120 Hz 4th-order Butterworth stage low-passes it. The bypass branch used
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
- **Automatic send safety**: a send is only added when the post-alignment
  average absolute phase residual is at most 30°. The 0 dB impact level
  additionally requires at most 15°. For the default diffuse timbre the LFE IR
  is the average of the same seven HRIRs as the main sum, so the HRIR phase
  cancels exactly and only the BW4 low-pass phase remains (~5.5° RMS after the
  all-pass); the gate therefore passes for every preset. The preset-specific
  failures (for example `AtmosRenderer-Far`, 249°, and `dtshx`, 143°) apply to
  the δ/center timbres, where the LFE phase is not sourced from the same HRIR
  average. Without the gate the −6 dB send cancelled the main bass by up to
  4.6 dB at 40 Hz and 3.5 dB at 100 Hz. The analysis line and the journal state
  which case applies.

The send and the LFE are aligned to the main HRIR bass per preset: the 120 Hz
Butterworth-4 low-pass has several milliseconds of group delay at 40-120 Hz,
while each HRIR has its own low-frequency delay. A per-ear 20-120 Hz fit
(4.29 ms for `27_95_142` with the default diffuse IR; 3.98 ms in the earlier
LR4/common-ear setup) removes the old −95…−171° phase difference above 90 Hz:
the main path gets a common non-negative delay, and each LFE ear gets the
remainder plus its own all-pass. Without it a louder send cancels instead of
reinforcing. With the default diffuse timbre the L/R fits are identical,
because the timbre is the same HRIR average as the main sum: the per-ear stage
reduces to a common 4.29 ms main delay, zero LFE delays and one all-pass per
ear. The continuous-grid residual is 12.15° RMS before and 5.55° RMS after the
all-pass (max 10.6°). The per-ear/preset-dependent behavior below applies to the
δ/center timbres: those whose phase is not a clean delay (for example
`AtmosRenderer-Far`) fail the 30° send gate and get no send; impact
additionally falls back to −6 dB when the residual exceeds 15°.

The old 70-90 Hz FreeSurround redirect and its `VSM_BASS_MODE` /
`VSM_BASS_LEGACY` / `VSM_LFE_SEND_DB` environment variables were removed.
Important bass stays in the main channels; the LFE is an effects channel and
is discarded in a stereo downmix.

### LFE timbre selection (A/B)

Settings → “LFE 音色”. The LFE low-pass is fixed to a **120 Hz 4th-order
Butterworth** (−3 dB at 120 Hz, 24 dB/oct) and **per-ear LFE alignment is
always on** (separate L/R delay/all-pass, one common main delay). The earlier
LR4/BW8 low-pass and common/per-ear switches have been removed.

Only the LFE IR/timbre is selectable, in this order:

1. **Diffuse-field HRIR (default)** — average of the seven left/right HRIRs,
   normalized to the center pair’s 40–80 Hz magnitude. With the BW4 low-pass
   the native LFE lands at +6.0 dB at 60 Hz, 0.5 dB above the Dolby binaural
   +5.5 dB reference (the old LR4 produced +5.5). It is a timbre/externalization
   choice, not a strictly neutral LFE: some presets deviate from the center IR
   and show a small L/R difference. Because it is the same seven-HRIR average
   used by the main sum, its HRIR phase cancels exactly in the LFE/main
   comparison; the alignment reduces to the BW4 low-pass and is identical for
   L/R and for every preset (the send gate passes in this mode). This exact
   cancellation refers to the unweighted seven-HRIR main sum: FreeSurround's
   per-channel output gains depend on the content, so a fixed LFE filter aligns
   the mono/equal-weight reference exactly but only approximates arbitrary
   upmix distributions (the 5.5° figure is that reference, not a guarantee for
   every mix).
2. **δ impulse (uncolored, second)** — the normalized unit impulse; equal L/R,
   no HRIR coloring. This is the strictly neutral LFE option.
3. **Center HRIR (third)** — the center HRIR pair; a colored, center-like LFE.

Changing the LFE timbre rebuilds the effects graph behind the stable bridge. The analysis
line shows the active LFE timbre, the per-ear delays/all-pass and the send
state. Measured with 27_95 at 60 Hz the output level is 0.02755 for the
default (diffuse/BW4/per-ear); the 120 Hz BW4 level is 0.02469 vs 0.02292 with
the old LR4. The differences are deliberately subtle; use bass-heavy content
and A/B.

### Relationship to speaker/AVR bass management

The app is a **headphone binaural renderer**, not an AVR with a subwoofer, so
the usual speaker bass-management settings do not transfer directly:

- **Do not add an 80 Hz speaker crossover / redirect to the main channels.**
  The HRIR already contains the headphone's full-range response; a high-pass
  and a mono LFE copy would remove the spatial low-frequency cues. This is why
  the main channels stay full-range (`use_lfe=0`).
- **LFE low-pass = 120 Hz** is the one setting that transfers directly (the
  Dolby LFE band limit; DTS LFE content ends at 80 Hz and passes unchanged).
  The implementation is a 120 Hz 4th-order Butterworth, which is −3 dB at
  120 Hz and 24 dB/oct (chosen by listening on 2026-10-30 over the previous
  LR4). Because LR4 was −6 dB at 120 Hz, BW4 makes the LFE about 3 dB hotter
  at 100–120 Hz; that is the intended impact, not a level error. Changing it
  would alter the 80–120 Hz transition and require re-fitting the
  delay/all-pass.
- **LFE is not a subwoofer**, and it is not +10 dB here. The speaker/sub
  convention is +10 dB SPL; the Dolby binaural convention is +5.5 dB (10 dB
  speaker makeup minus 4.5 dB for a two-point-source headphone render). The
  native LFE gets its +6 dB (2x) and the HRIR-normalized LFE timbre IR targets
  that reference; with BW4 it measures +6.0 dB at 60 Hz (0.5 dB hot). Do not
  apply +10 dB to the headphone path.
- **Reference is the LFE-only equivalent.** Native 5.1/7.1 never receives a
  duplicated main bass (no “LFE+Main” for real multichannel). Balanced/impact
  add a mono send **only while upmixing stereo**, with a 120 Hz low-pass and
  phase alignment; they are taste options, not a bass-management standard.
- **LFE LPF 80 Hz is not used**; it would cut the 80–120 Hz LFE content that
  carries impact. The AVR advice to set the *sub's own* low-pass to maximum
  does not apply because there is no sub in this chain.
- **Level match and protection**: the optional preset loudness matching is the
  analog of AVR speaker/sub level calibration, and the optional output limiter
  is the analog of the AVR/pro LFE/output limiter. Both are off by default.

### Additional output options

Both options rebuild the effects graph behind the stable bridge, so a running
stream is not moved:

- **Preset loudness matching** (`preset_level_match`, default off): computes a
  center-weighted A-weighted level for every readable 14-channel HRIR and moves
  the selected preset to the median of the current list, bounded to ±8 dB.
  Only the overall gain changes; channel balance and the LFE/main ratio stay
  untouched. The analysis line shows the applied trim.
- **Output safety limiter** (`output_limiter`, default off): stereo-linked
  lookahead peak limiter after `outGain`; 5 ms lookahead, −1.5 dBFS threshold,
  100 ms release. Transparent below threshold. Enabling it adds 5 ms latency.
  The normal chain is far below the threshold; the limiter only protects
  extreme native bass or impact material.

### Non-48 kHz HRIRs

The graph always runs at 48 kHz. Imported 44.1/96 kHz 14-channel WAVs are
accepted: PipeWire resamples the IR at the highest quality (`resample_quality`
14), and the LFE alignment/all-pass are designed for the 48 kHz graph while the
HRIR phase is measured on its own time axis. The analysis line shows the source
rate and “已重采样”. The LFE timbre IR (delta or diffuse) is generated at the
source rate and resampled by the same convolver, so the LFE/main level relation
is preserved.

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
table): it prints and displays the LFE-IR normalization gain, the per-ear
20-120 Hz alignment delays and all-pass corrections (unity magnitude, phase
only), the absolute average/worst-channel phase residual per ear, the LFE-send
state, the source sample rate and the preset-level trim. A new file is handled
automatically; a 2 ms longer copy of `27_95_142` shifts the fitted delay by
about 2 ms as expected. If the phase is not a clean delay (slope residual
>15 degrees, e.g. AtmosRenderer-Far), no delay or all-pass is inserted; a
post-alignment residual above 30° disables any LFE send and above 15° prevents
the 0 dB impact level. The result appears above the profile list in the main
window and in the journal. Whenever the graph is rebuilt or the profile list is
reloaded, the line is first replaced by an explicit “refreshing” sentinel; a
failed or unavailable analysis shows “not acquired”, and missing per-ear values
show “—”, so an old/default value cannot be mistaken for a fresh one. With the
default diffuse timbre the LFE L/R delays are a real 0.00 ms (the HRIR phase
cancels); the text says so explicitly instead of showing a bare zero.

### Crackling / brief dropouts

Known sources of short interruptions during playback are addressed:

- A newly created playback stream is routed behind a **150 ms output gate**
  while upmixing: the effects graph is muted for 150 ms (the FreeSurround
  delay), then unmuted at zero gain and faded in over 100 ms. Muting only the
  source stream was too late: the first samples were already inside the 128 ms
  delay line and reappeared at the output (measured first-sample burst
  0.354 → 0). The gate also removes the previous track's delay-line residue.
  A 30 ms source mute remains as a fallback when upmixing is off or the gate
  proxy is unavailable; the continuous-tone release step measured
  0.135 → 0.037.
- The virtual device nodes set `node.latency = "1024/48000"`, which keeps the
  PipeWire graph quantum at 1024 samples or more whenever the chain is active.
  Without it, a low-latency application can drag the whole graph down to tiny
  quanta where the convolver chain is prone to xruns, and every quantum
  renegotiation is audible.
- The upmix plugin applies one second of stereo hysteresis before re-engaging
  upmixing after a bypass period (see `third_party/freesurround_ladspa/README.md`):
  this keeps content hovering at the stereo detection threshold from toggling
  the detector. The delay ring and upmix FIFOs stay primed.
- The stereo/multichannel switch waits for the native content to traverse the
  128 ms bypass delay (plus 20 ms) and then crossfades over 20 ms. The old
  hard switch produced a click and an 85 ms centre hole with a short HRIR;
  the wait/crossfade keeps the centre and surround image continuous.
- Toggling upmixing on rebuilds the effect graph and needs a fresh 128 ms
  delay line. A normal HeSuVi HRIR is about 1 s long and its tail covers that
  refill, so changing profiles stays seamless. A very short HRIR (<128 ms,
  e.g. `27_95_142.wav`) leaves a ~90 ms gap when upmixing is switched on;
  upmixing off is a short overlap of the old delay line. This is a property of
  the FreeSurround latency, not a routing problem. The optional output limiter
  adds a further 5 ms when enabled.

## Acknowledgements

- The [EasyEffects](https://github.com/wwmm/easyeffects) project for research on the API, because the PipeWire docs are quite lacking for beginners
- The [HeSuVi](https://hesuvi.net) project for their awesome collection of HRIR WAV files
- The [Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project and Christian Kothe's original FreeSurround decoder for the stereo upmixing plugin
