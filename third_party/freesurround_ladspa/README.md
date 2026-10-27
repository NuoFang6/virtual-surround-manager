# FreeSurround LADSPA plugin (Real3D)

This directory vendors the **FreeSurround stereo-to-7.1 upmixer** from the
[Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project as a LADSPA plugin.

The virtual surround filter graph uses the plugin label **`freesurround71_auto`**
(8 in / 8 out): when the input is stereo — i.e. all non-L/R channels are silent —
it upmixes to 7.1 with FreeSurround (Passive Surround Decoding); as soon as any
non-L/R channel carries signal, real multichannel content is detected and the
input is passed through unchanged with matching latency compensation. This
replicates the Windows chain (EqualizerAPO + Real3D-VST → HeSuVi), stereo
detection included.

The Virtual Surround Manager's graph always runs the upmixer with
`use_lfe=0`: the main channels stay full-range through the HRIRs. Its three
user-selectable LFE modes are built in the application's filter graph, not in
the plugin: `balanced` adds a −6 dB mono copy of the main channels to the LFE
while upmixing, `reference` uses native LFE only, and `impact` uses the same
send at 0 dB. The application also delays the main or LFE path per preset to
align the LR4 bass with the HRIR bass; without that, a larger send cancels.
The plugin's own bass-redirection DSP (`use_lfe=1`) remains available for other
hosts but is no longer used by this application.

The plugin is **required for stereo upmixing** and built and installed together
with the application (into the system LADSPA directory, e.g. `/usr/lib/ladspa`,
`/usr/lib64/ladspa` or `/app/lib/ladspa` inside the Flatpak). When upmixing is
disabled, the plugin is not used at all.

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
- The auto plugin has an extra `lfe_send` control. While upmixing, its LFE
  output is the mono sum of the seven main outputs scaled by this value; while
  bypassing, it remains the native LFE ×2 and the send is ignored. Keeping the
  send inside the plugin is what prevents native 5.1/7.1 content from getting
  a duplicate bass. The app passes 0.501 (balanced) or 1.0 (impact).

- `freesurround71_auto` re-engages upmixing only after sustained pure-stereo
  input (`kStereoTolerance` consecutive blocks) instead of on the first silent
  block. The hysteresis keeps content hovering at the detection threshold from
  toggling the detector.
- Both paths stay primed: the bypass delay ring is written in both modes, and
  the upmix core is fed (with its output discarded) while bypassing. A
  stereo↔multichannel content switch therefore starts with real audio instead
  of flushing a 128 ms silent/stale buffer. (Before this patch the switch had a
  measured 128 ms zero gap into bypass and a ~75 ms one back into upmix.)
- The bypass branch applies the native-LFE makeup: the LFE output is 2x the
  input LFE (+6 dB). The filter graph uses unity LFE gain while upmix is enabled
  (extracted bass must not be boosted), so native 5.1/7.1 LFE needs the same
  makeup HeSuVi's `lfc.txt` applies. Bass extracted by the upmixer stays at 1x.
- The silence floor for the stereo detection is -90 dBFS (`3e-5`) instead of
  the VST's `-120 dBFS` (`1e-6`): dummy 8-channel output that carries only
  dither-level noise on the non-L/R channels (browsers, games) still counts as
  stereo and gets upmixed, while audible surround content still bypasses.

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
