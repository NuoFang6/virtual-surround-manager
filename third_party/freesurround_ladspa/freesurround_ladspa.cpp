/*
    LADSPA wrapper for the Real3D / FreeSurround decoder.

    Two plugins are provided, both using the exact same DSP code and default values
    as the Real3D VST plugin (dsp/freesurround_decoder.cpp):

    1. label "freesurround71"        stereo (2) in -> 7.1 (8) out upmixer
    2. label "freesurround71_auto"   8 in -> 8 out, replicating the VST plugin's
       automatic stereo detection: when the input is stereo (or only the front
       channels carry signal) it upmixes with FreeSurround; when real multichannel
       content is detected it bypasses the upmixer with matching latency
       compensation.

    The "auto" version is used by the Virtual Surround Manager filter graph, so a
    single 8-channel virtual device reproduces the Windows chain
    (EqualizerAPO + Real3D-VST -> HeSuVi), channel detection included.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ladspa.h"
#include "freesurround_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kFftSize = 4096;                    // core block size, same as the VST plugin
constexpr int kNumOut = 8;                        // 7.1
constexpr int kLatency = kFftSize + kFftSize / 2; // bypass compensation, same as the VST plugin
// Local patch (not in Real3D-VST, which uses 1e-6): silence floor for the stereo
// detection. -90 dBFS is far below audibility, so content this quiet on the
// non-L/R channels still counts as "fake" 8-channel output (browser/game dummy
// channels with dither noise), while anything a listener could actually hear
// on a surround channel still counts as real multichannel content.
constexpr float kStereoEpsilon = 3e-5f;
constexpr int kNonStereoTolerance = 2;            // tolerate 2 blocks before bypassing
// Local patch (not in Real3D-VST): re-engaging the upmixer flushes its FIFOs, which
// is audible as a short click. Content hovering right at the detection threshold
// must not toggle the detector, so after a bypass the input has to stay pure stereo
// for this many consecutive blocks before upmixing resumes (~1s at 1024/48k).
constexpr int kStereoTolerance = 48;

// control values shared by both plugins (port indices, relative to the control block)
enum ControlPort {
    CTRL_CENTER_IMAGE = 0,
    CTRL_SHIFT,
    CTRL_DEPTH,
    CTRL_CIRCULAR_WRAP,
    CTRL_FOCUS,
    CTRL_FRONT_SEP,
    CTRL_REAR_SEP,
    CTRL_BASS_LO,
    CTRL_BASS_HI,
    CTRL_USE_LFE,
    CTRL_UPMIX_ENABLE, // auto plugin: 1.0 = allow FreeSurround upmixing
    CTRL_LATENCY,
    CTRL_COUNT
};

struct Controls {
    LADSPA_Data *centerImage = nullptr;
    LADSPA_Data *shift = nullptr;
    LADSPA_Data *depth = nullptr;
    LADSPA_Data *circularWrap = nullptr;
    LADSPA_Data *focus = nullptr;
    LADSPA_Data *frontSep = nullptr;
    LADSPA_Data *rearSep = nullptr;
    LADSPA_Data *bassLo = nullptr;
    LADSPA_Data *bassHi = nullptr;
    LADSPA_Data *useLfe = nullptr;
    LADSPA_Data *upmixEnable = nullptr;
    LADSPA_Data *latencyOut = nullptr;
};

void connectControl(Controls &c, unsigned long port, LADSPA_Data *data) {
    switch (port) {
    case CTRL_CENTER_IMAGE: c.centerImage = data; break;
    case CTRL_SHIFT: c.shift = data; break;
    case CTRL_DEPTH: c.depth = data; break;
    case CTRL_CIRCULAR_WRAP: c.circularWrap = data; break;
    case CTRL_FOCUS: c.focus = data; break;
    case CTRL_FRONT_SEP: c.frontSep = data; break;
    case CTRL_REAR_SEP: c.rearSep = data; break;
    case CTRL_BASS_LO: c.bassLo = data; break;
    case CTRL_BASS_HI: c.bassHi = data; break;
    case CTRL_USE_LFE: c.useLfe = data; break;
    case CTRL_UPMIX_ENABLE: c.upmixEnable = data; break;
    case CTRL_LATENCY: c.latencyOut = data; break;
    default: break;
    }
}

void syncControls(freesurround_decoder *decoder, const Controls &c, unsigned long sampleRate) {
    const float srNyquist = (float)(sampleRate / 2.0);
    if (c.centerImage) decoder->center_image(*c.centerImage);
    if (c.shift) decoder->shift(*c.shift);
    if (c.depth) decoder->depth(*c.depth);
    if (c.circularWrap) decoder->circular_wrap(*c.circularWrap);
    if (c.focus) decoder->focus(*c.focus);
    if (c.frontSep) decoder->front_separation(*c.frontSep);
    if (c.rearSep) decoder->rear_separation(*c.rearSep);
    if (c.useLfe) decoder->bass_redirection(*c.useLfe > 0.5f);
    // normalized to the Nyquist frequency, exactly like the VST plugin does it
    if (c.bassLo) decoder->low_cutoff(*c.bassLo / srNyquist);
    if (c.bassHi) decoder->high_cutoff(*c.bassHi / srNyquist);
}

// decoder channel index for each named output slot, resolved from speaker semantics
struct ChannelMap {
    int outIndex[kNumOut]; // [FL, FR, FC, LFE, RL, RR, SL, SR] -> decoder channels
    void resolve() {
        const channel_id wanted[kNumOut] = {
            ci_front_left, ci_front_right, ci_front_center, ci_lfe,
            ci_back_left, ci_back_right, ci_side_center_left, ci_side_center_right};
        const unsigned n = freesurround_decoder::num_channels(cs_7point1);
        for (int i = 0; i < kNumOut; ++i) {
            outIndex[i] = 0;
            for (unsigned ch = 0; ch < n; ++ch) {
                if (freesurround_decoder::channel_at(cs_7point1, ch) == wanted[i]) {
                    outIndex[i] = (int)ch;
                    break;
                }
            }
        }
    }
};

//----------------------------------------------------------------------------
// shared FreeSurround processing (stereo in -> 7.1 out, with FIFOs)
//----------------------------------------------------------------------------
struct UpmixCore {
    freesurround_decoder *decoder = nullptr;
    ChannelMap map;
    std::vector<float> inFifo[2];
    std::size_t fifoWriteIdx = 0, fifoReadIdx = 0, inFifoFill = 0;
    std::vector<float> outFifo[kNumOut];
    std::size_t outFifoWriteIdx = 0, outFifoReadIdx = 0, outFifoFill = 0;
    std::vector<float> interleaved;

    void init(unsigned long sampleRate) {
        decoder = new freesurround_decoder(cs_7point1, kFftSize);
        decoder->set_sample_rate((double)sampleRate);
        map.resolve();
        interleaved.resize(2 * kFftSize);
        reset();
    }
    void destroy() {
        delete decoder;
        decoder = nullptr;
    }
    void reset() {
        const std::size_t inSize = 4 * kFftSize;
        const std::size_t outSize = 4 * kFftSize;
        for (int c = 0; c < 2; ++c) inFifo[c].assign(inSize, 0.0f);
        for (int c = 0; c < kNumOut; ++c) outFifo[c].assign(outSize, 0.0f);
        fifoWriteIdx = fifoReadIdx = inFifoFill = 0;
        outFifoWriteIdx = outFifoReadIdx = outFifoFill = 0;
        if (decoder) decoder->flush();
    }

    void feed(const LADSPA_Data *l, const LADSPA_Data *r, unsigned long n) {
        // n must not exceed the FIFO size (4 * kFftSize = 16384); PipeWire's
        // default maximum quantum is 8192, so this holds with headroom
        const std::size_t inSize = inFifo[0].size();
        const std::size_t outSize = outFifo[0].size();
        for (unsigned long s = 0; s < n; ++s) {
            if (inFifoFill >= inSize) {
                fifoReadIdx = (fifoReadIdx + 1) % inSize;
                --inFifoFill;
            }
            const std::size_t w = (fifoWriteIdx + s) % inSize;
            inFifo[0][w] = l ? l[s] : 0.0f;
            inFifo[1][w] = r ? r[s] : 0.0f;
            ++inFifoFill;
        }
        fifoWriteIdx = (fifoWriteIdx + n) % inSize;

        while (inFifoFill >= (std::size_t)kFftSize) {
            for (int i = 0; i < kFftSize; ++i) {
                const std::size_t idx = (fifoReadIdx + i) % inSize;
                interleaved[2 * i] = inFifo[0][idx];
                interleaved[2 * i + 1] = inFifo[1][idx];
            }
            float *decoded = decoder->decode(interleaved.data());
            const int numOut = (int)freesurround_decoder::num_channels(cs_7point1);
            for (int s = 0; s < kFftSize; ++s) {
                if (outFifoFill >= outSize) {
                    outFifoReadIdx = (outFifoReadIdx + 1) % outSize;
                    --outFifoFill;
                }
                const std::size_t w = (outFifoWriteIdx + s) % outSize;
                for (int c = 0; c < numOut && c < kNumOut; ++c)
                    outFifo[c][w] = decoded[s * numOut + c];
                ++outFifoFill;
            }
            fifoReadIdx = (fifoReadIdx + kFftSize) % inSize;
            inFifoFill -= kFftSize;
            outFifoWriteIdx = (outFifoWriteIdx + kFftSize) % outSize;
        }
    }

    bool hasOutput(unsigned long n) const { return outFifoFill >= n; }

    void writeOut(LADSPA_Data *out[kNumOut], unsigned long n) {
        const std::size_t outSize = outFifo[0].size();
        for (int port = 0; port < kNumOut; ++port) {
            if (!out[port]) continue;
            const std::vector<float> &fifo = outFifo[map.outIndex[port]];
            for (unsigned long s = 0; s < n; ++s)
                out[port][s] = fifo[(outFifoReadIdx + s) % outSize];
        }
        outFifoReadIdx = (outFifoReadIdx + n) % outSize;
        outFifoFill -= n;
    }
};

//----------------------------------------------------------------------------
// plugin 1: freesurround71 - plain stereo -> 7.1 upmixer (2 in / 8 out)
//----------------------------------------------------------------------------
struct StereoInstance {
    UpmixCore core;
    Controls ctrl;
    unsigned long sampleRate = 48000;
    LADSPA_Data *inL = nullptr;
    LADSPA_Data *inR = nullptr;
    LADSPA_Data *out[kNumOut] = {};
};

LADSPA_Handle stereoInstantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    StereoInstance *self = new StereoInstance();
    self->sampleRate = sampleRate ? sampleRate : 48000;
    self->core.init(self->sampleRate);
    return (LADSPA_Handle)self;
}

void stereoConnectPort(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    StereoInstance *self = (StereoInstance *)handle;
    if (port == 0) { self->inL = data; return; }
    if (port == 1) { self->inR = data; return; }
    if (port >= 2 && port < 10) { self->out[port - 2] = data; return; }
    connectControl(self->ctrl, port - 10, data);
    if (self->ctrl.latencyOut) *self->ctrl.latencyOut = (float)kLatency;
}

void stereoActivate(LADSPA_Handle handle) {
    StereoInstance *self = (StereoInstance *)handle;
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    self->core.reset();
}

void stereoRun(LADSPA_Handle handle, unsigned long n) {
    StereoInstance *self = (StereoInstance *)handle;
    if (!self->core.decoder || n == 0) return;
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    self->core.feed(self->inL, self->inR, n);
    if (!self->core.hasOutput(n)) {
        for (int c = 0; c < kNumOut; ++c)
            if (self->out[c]) std::memset(self->out[c], 0, sizeof(LADSPA_Data) * n);
        return;
    }
    self->core.writeOut(self->out, n);
}

void stereoCleanup(LADSPA_Handle handle) {
    StereoInstance *self = (StereoInstance *)handle;
    self->core.destroy();
    delete self;
}

//----------------------------------------------------------------------------
// plugin 2: freesurround71_auto - 8 in / 8 out with automatic stereo detection
//----------------------------------------------------------------------------
struct AutoInstance {
    UpmixCore core;
    Controls ctrl;
    unsigned long sampleRate = 48000;
    LADSPA_Data *in[kNumOut] = {};
    LADSPA_Data *out[kNumOut] = {};

    std::vector<float> bypass[kNumOut];
    std::size_t bypassWriteIdx = 0;
    bool wasBypassed = false;
    int nonStereoBlocks = 0;
    int stereoBlocks = 0;
};

LADSPA_Handle autoInstantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    AutoInstance *self = new AutoInstance();
    self->sampleRate = sampleRate ? sampleRate : 48000;
    self->core.init(self->sampleRate);
    for (int c = 0; c < kNumOut; ++c) self->bypass[c].assign(kLatency, 0.0f);
    return (LADSPA_Handle)self;
}

void autoConnectPort(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    AutoInstance *self = (AutoInstance *)handle;
    if (port < 8) { self->in[port] = data; return; }
    if (port >= 8 && port < 16) { self->out[port - 8] = data; return; }
    connectControl(self->ctrl, port - 16, data);
    if (self->ctrl.latencyOut) *self->ctrl.latencyOut = (float)kLatency;
}

void autoActivate(LADSPA_Handle handle) {
    AutoInstance *self = (AutoInstance *)handle;
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    self->core.reset();
    for (int c = 0; c < kNumOut; ++c) std::fill(self->bypass[c].begin(), self->bypass[c].end(), 0.0f);
    self->bypassWriteIdx = 0;
    self->wasBypassed = false;
    self->nonStereoBlocks = 0;
    self->stereoBlocks = kStereoTolerance; // start upmixing immediately after activation
}

void autoRun(LADSPA_Handle handle, unsigned long n) {
    AutoInstance *self = (AutoInstance *)handle;
    if (!self->core.decoder || n == 0) return;

    syncControls(self->core.decoder, self->ctrl, self->sampleRate);

    // ---- stereo detection, same rules as the VST plugin ----
    // ports 2..7 are FC, LFE, RL, RR, SL, SR in this graph's channel order
    bool extraChannelsSilent = true;
    for (int c = 2; c < kNumOut && extraChannelsSilent; ++c) {
        if (!self->in[c]) continue;
        for (unsigned long s = 0; s < n; ++s) {
            if (std::fabs(self->in[c][s]) > kStereoEpsilon) {
                extraChannelsSilent = false;
                break;
            }
        }
    }
    if (extraChannelsSilent) {
        ++self->stereoBlocks;
        self->nonStereoBlocks = 0;
    } else {
        ++self->nonStereoBlocks;
        self->stereoBlocks = 0;
    }
    bool isActuallyStereo;
    if (self->wasBypassed) {
        // hysteresis: only resume upmixing after sustained pure-stereo input
        isActuallyStereo = self->stereoBlocks >= kStereoTolerance;
    } else {
        // tolerate short non-stereo bursts before switching to bypass
        isActuallyStereo = self->nonStereoBlocks <= kNonStereoTolerance;
    }
    // the upmix switch disables FreeSurround processing entirely
    const bool upmixAllowed = !self->ctrl.upmixEnable || *self->ctrl.upmixEnable > 0.5f;
    if (!upmixAllowed)
        isActuallyStereo = false;

    if (!isActuallyStereo) {
        // ---- bypass path: pass the (delayed) input through unchanged ----
        if (!self->wasBypassed) {
            self->core.reset();
            self->wasBypassed = true;
        }
        // Delay-line semantics: dst[s] = src[s - kLatency], with samples from
        // before this run taken from the ring buffer. Reads and writes are done
        // in separate passes so this stays correct even when the block length n
        // exceeds the delay length (a single read/write-per-index pass would
        // wrap around and emit samples from this very run at the wrong delay).
        const std::size_t kLat = (std::size_t)kLatency;
        const unsigned long m = std::min<unsigned long>(n, (unsigned long)kLatency);
        const std::size_t start = (self->bypassWriteIdx + (n - m)) % kLat;
        for (int c = 0; c < kNumOut; ++c) {
            if (!self->out[c]) continue;
            std::vector<float> &buf = self->bypass[c];
            LADSPA_Data *src = self->in[c];
            LADSPA_Data *dst = self->out[c];
            // read pass: outputs older than kLatency samples come from the ring,
            // the rest can be taken directly from this run's input
            if (n < (unsigned long)kLatency) {
                for (unsigned long s = 0; s < n; ++s)
                    dst[s] = buf[(self->bypassWriteIdx + s) % kLat];
            } else {
                for (unsigned long s = 0; s < (unsigned long)kLatency; ++s)
                    dst[s] = buf[(self->bypassWriteIdx + s) % kLat];
                for (unsigned long s = (unsigned long)kLatency; s < n; ++s)
                    dst[s] = src ? src[s - kLatency] : 0.0f;
            }
            // write pass: the ring keeps the last m samples of this run
            for (unsigned long j = 0; j < m; ++j)
                buf[(start + j) % kLat] = src ? src[n - m + j] : 0.0f;
        }
        self->bypassWriteIdx = (self->bypassWriteIdx + n) % kLat;
        return;
    }

    // ---- processing path ----
    if (self->wasBypassed) {
        self->core.reset();
        self->wasBypassed = false;
    }
    self->core.feed(self->in[0], self->in[1], n);
    if (!self->core.hasOutput(n)) {
        for (int c = 0; c < kNumOut; ++c)
            if (self->out[c]) std::memset(self->out[c], 0, sizeof(LADSPA_Data) * n);
        return;
    }
    self->core.writeOut(self->out, n);
}

void autoCleanup(LADSPA_Handle handle) {
    AutoInstance *self = (AutoInstance *)handle;
    self->core.destroy();
    delete self;
}

//----------------------------------------------------------------------------
// port tables and descriptors
//----------------------------------------------------------------------------
#define AUDIO_IN (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)
#define AUDIO_OUT (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)
#define CONTROL_IN (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL)
#define CONTROL_OUT (LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL)
#define NOHINT {0, 0.0f, 0.0f}
#define CTRL(hint, lo, hi) {hint, lo, hi}
#define C_HINTS                                                                          \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 1.0f),   /* center_image  */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0, -1.0f, 1.0f),     /* shift         */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0f, 4.0f),      /* depth         */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 360.0f), /* circular_wrap */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0, -1.0f, 1.0f),     /* focus         */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0f, 2.0f),      /* front_sep     */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0f, 2.0f),      /* rear_sep      */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 150.0f), /* bass_lo      */ \
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 150.0f), /* bass_hi      */ \
    CTRL(LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f)                                          /* use_lfe      */

const char *const stereoPortNames[] = {
    "In L", "In R",
    "Out FL", "Out FR", "Out FC", "Out LFE", "Out RL", "Out RR", "Out SL", "Out SR",
    "center_image", "shift", "depth", "circular_wrap", "focus",
    "front_sep", "rear_sep", "bass_lo", "bass_hi", "use_lfe",
    "latency",
};

const LADSPA_PortDescriptor stereoPortDescriptors[] = {
    AUDIO_IN, AUDIO_IN,
    AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_OUT,
};

const LADSPA_PortRangeHint stereoPortRangeHints[] = {
    NOHINT, NOHINT,
    NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT,
    C_HINTS,
    NOHINT, // latency (output)
};

const char *const autoPortNames[] = {
    "In FL", "In FR", "In FC", "In LFE", "In RL", "In RR", "In SL", "In SR",
    "Out FL", "Out FR", "Out FC", "Out LFE", "Out RL", "Out RR", "Out SL", "Out SR",
    "center_image", "shift", "depth", "circular_wrap", "focus",
    "front_sep", "rear_sep", "bass_lo", "bass_hi", "use_lfe",
    "upmix_enable", "latency",
};

const LADSPA_PortDescriptor autoPortDescriptors[] = {
    AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN,
    AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_IN,
    CONTROL_OUT,
};

const LADSPA_PortRangeHint autoPortRangeHints[] = {
    NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT,
    NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT,
    C_HINTS,
    CTRL(LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f), // upmix_enable
    NOHINT,                                                        // latency (output)
};

const LADSPA_Descriptor descriptorStereo = {
    .UniqueID = 0x52453344, // "RE3D"
    .Label = "freesurround71",
    .Name = "Real3D FreeSurround Stereo to 7.1 Upmixer",
    .Maker = "Real3D-VST",
    .Copyright = "GPL-2.0-or-later",
    .PortCount = 21,
    .PortDescriptors = stereoPortDescriptors,
    .PortNames = stereoPortNames,
    .PortRangeHints = stereoPortRangeHints,
    .ImplementationData = nullptr,
    .instantiate = stereoInstantiate,
    .connect_port = stereoConnectPort,
    .activate = stereoActivate,
    .run = stereoRun,
    .run_adding = nullptr,
    .set_run_adding_gain = nullptr,
    .deactivate = nullptr,
    .cleanup = stereoCleanup,
};

const LADSPA_Descriptor descriptorAuto = {
    .UniqueID = 0x52453345, // "RE3E"
    .Label = "freesurround71_auto",
    .Name = "Real3D FreeSurround 7.1 Auto Upmixer (stereo detection + bypass)",
    .Maker = "Real3D-VST",
    .Copyright = "GPL-2.0-or-later",
    .PortCount = 28,
    .PortDescriptors = autoPortDescriptors,
    .PortNames = autoPortNames,
    .PortRangeHints = autoPortRangeHints,
    .ImplementationData = nullptr,
    .instantiate = autoInstantiate,
    .connect_port = autoConnectPort,
    .activate = autoActivate,
    .run = autoRun,
    .run_adding = nullptr,
    .set_run_adding_gain = nullptr,
    .deactivate = nullptr,
    .cleanup = autoCleanup,
};

const LADSPA_Descriptor *const kDescriptors[] = {&descriptorStereo, &descriptorAuto};

} // namespace

extern "C" const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    return index < 2 ? kDescriptors[index] : nullptr;
}