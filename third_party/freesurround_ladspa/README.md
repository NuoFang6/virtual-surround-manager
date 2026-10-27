# FreeSurround LADSPA plugin (Real3D)

This directory vendors the **FreeSurround stereo-to-7.1 upmixer** from the
[Real3D-VST](https://github.com/NuoFang6/Real3D-VST) project as a LADSPA plugin.

The virtual surround filter graph uses the plugin label **`freesurround71_auto`**
(8 in / 8 out): when the input is stereo — i.e. all non-L/R channels are silent —
it upmixes to 7.1 with FreeSurround (Passive Surround Decoding, including bass
redirection into the LFE); as soon as any non-L/R channel carries signal, real
multichannel content is detected and the input is passed through unchanged with
matching latency compensation. This replicates the Windows chain
(EqualizerAPO + Real3D-VST → HeSuVi), stereo detection included.

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

- `freesurround71_auto` re-engages upmixing only after sustained pure-stereo
  input (`kStereoTolerance` consecutive blocks) instead of on the first silent
  block. Every bypass↔processing switch flushes the plugin's FIFOs, which is
  audible as a short click; the hysteresis keeps content hovering at the
  detection threshold from toggling the detector.
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
