# FreeSurround LADSPA plugin (Real3D)

This directory vendors the **FreeSurround stereo-to-7.1 upmixer** from the
[Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project as a LADSPA plugin.

The directory provides three LADSPA labels: **`freesurround71`** (stereo to
7.1), **`freesurround71_auto`** (8 in / 8 out with stereo detection) and
**`output_limiter`** (optional stereo reconstructed-peak limiter).

The virtual surround filter graph uses **`freesurround71_auto`** when upmixing:
when the input is stereo — i.e. all non-L/R channels are silent — it upmixes to
7.1 with FreeSurround (Passive Surround Decoding); as soon as any non-L/R
channel carries signal, real multichannel content is detected and the input is
passed through after a crossfade with matching sample latency and native-LFE
makeup. This is heuristic spatial redistribution, not recovery of independently
recorded surround channels or certification of equivalence to another renderer.
The FFT is 4096 points; both paths are delayed by 6144 samples (128 ms at 48 kHz).

The Virtual Surround Manager's graph always runs the upmixer with
`use_lfe=0`: the main channels stay full-range through the HRIRs. The
`balanced`/`impact` modes use the auto plugin's **mono main-sum `lfe_send`**
(−6 dB / 0 dB) only in the upmix branch; the native bypass branch keeps native
LFE ×2 and no send. The send is low-passed at 120 Hz BW4, rendered through the
coherent seven-pair diffuse IR, and per-ear delay/all-pass aligned against the
main HRIR sum. A send is only applied when the post-alignment phase residual
stays within 30° (impact's 0 dB needs 15°, otherwise it falls back to −6 dB);
bad presets get no send at all. The added layer is a mono send through the
diffuse IR, not a shelf on the front HRIRs, and does not apply to native
multichannel content. The app also delays the main path (one common delay,
preserving L/R ITD); see
[the acoustic model](../../docs/acoustics.md).
The plugin's own bass-redirection DSP (`use_lfe=1`) remains available for other
hosts but is no longer used by this application.

The plugin is **required for stereo upmixing** and built and installed together
with the application (into the system LADSPA directory, e.g. `/usr/lib/ladspa`,
`/usr/lib64/ladspa` or `/app/lib/ladspa` inside the Flatpak). When upmixing is
disabled and the optional output limiter is off, the plugin is not used at all.

## Provenance

Copied from the Real3D-VST repository, commit `4f9567b` ("add LADSPA"), with the
JUCE/VST-specific parts of that repository left out and the directory layout
flattened (only the `#include "../dsp/..."` path in `freesurround_ladspa.cpp` was
adapted to match).

Local patches (not yet in Real3D-VST):

- `vsm_auto_upmix_state` is exported (`extern "C"` atomic int): 1 while the
  detector is upmixing, 0 while bypassing multichannel content, -1 before the
  first processed block. The application dlopen()s the plugin in its own
  process and reads it for the live input-detection display.
- `vsm_auto_blocks` is exported as a monotonic processed-block counter; the
  application uses it as an "audio is really flowing" heartbeat for the UI.
- **`center_image` is the upstream no-op**: the LADSPA port and the decoder
  setter store it, but `buffered_decode()` never reads it. No C/FL/FR phantom
  and no global renormalization are applied; the original FreeSurround mapping
  and level law are preserved.
- `freesurround71_auto` re-engages upmixing only after one second of pure-stereo
  input instead of on the first silent block. The hysteresis is counted in
  samples (`kStereoToleranceSeconds`), so it is independent of the host quantum.
- Both paths stay primed and have an explicitly scheduled 6144-sample latency
  (4096-point FFT), independent of the host's fixed or variable block size.
  Large blocks are streamed through bounded buffers rather than copied into
  fixed-size scratch arrays.
- Detector decisions are delayed alongside the audio. A short non-L/R event
  engages the existing one-second bypass hold rather than being discarded
  because it did not last for `kLatency + crossfade` samples.
- The path switch is a per-sample 20 ms crossfade (`kCrossfadeSeconds`).
  Both paths are kLatency-delayed, so they can be blended without an echo or a
  time jump; the crossfade removes the click and keeps the centre/surround
  image continuous.
- The optional `output_limiter` label is a stereo-linked lookahead limiter with
  an **8×, 48-tap polyphase windowed-sinc reconstructed-peak detector**.
  Default 5 ms lookahead (240 samples), −1.5 dBFS threshold, 100 ms release.
  Audio stays at the host rate; only the detector is oversampled. The host test
  checks 12 kHz phase-shifted, 23 kHz near-Nyquist and 200 Hz square waves with
  an independent 32×/128-tap reconstruction. Below threshold it is unity gain
  plus the lookahead delay; it is a safety limiter, not true-peak
  hearing protection. The latency is reported through the standard LADSPA
  `latency` output port.
- The stereo label's latency port is wired independently of the auto label's
  additional controls.
- The bypass branch applies the native-LFE makeup: the LFE output is 2x the
  input LFE (+6.0206 dB), a product convention rather than universal headphone
  calibration. The graph applies unity makeup while the auto plugin is present
  so native LFE is not doubled twice.
- The detector threshold is approximately −90.46 dBFS (`3e-5`) rather than
  `1e-6`. This is a digital classification heuristic, not a physical audibility
  threshold; quiet non-L/R content can be classified as stereo.

## Licenses

- `freesurround_ladspa.cpp` — SPDX-License-Identifier: GPL-2.0-or-later
- `freesurround_decoder.{h,cpp}`, `channelmaps.{h,cpp}` — GPL-2.0-or-later,
  Copyright (C) 2007-2010 Christian Kothe (original FreeSurround)
- `ladspa.h` — LGPL-2.1-or-later, Richard W.E. Furse, Paul Barton-Davis,
  Stefan Westerfeld (LADSPA SDK header)
- `kissfft/` — BSD-3-Clause, Copyright (c) 2003-2010 Mark Borgerding
  (see `kissfft/COPYING`)

The full license texts are reproduced in the file headers (GPL) and
`kissfft/COPYING` (BSD); the canonical GPL-2.0 text is available at
<https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>.

Note: this plugin is a separate, dlopened shared library — it is not linked into
the MIT-licensed application binary.
