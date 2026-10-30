/*
    LADSPA wrapper for the Real3D / FreeSurround decoder.

    Two plugins are provided, both using the exact same DSP code and default values
    as the Real3D VST plugin (dsp/freesurround_decoder.cpp):

    1. label "freesurround71"        stereo (2) in -> 7.1 (8) out upmixer
    2. label "freesurround71_auto"   8 in -> 8 out, replicating the VST plugin's
       automatic stereo detection: when the input is stereo (or only the front
       channels carry signal) it upmixes with FreeSurround; when real multichannel
       content is detected it bypasses the upmixer with matching latency
       compensation (20 ms crossfade on both transitions).
    3. label "output_limiter"        optional stereo-linked lookahead peak limiter
       (-1.5 dBFS, 5 ms lookahead, 100 ms release).

    The "auto" version is used by the Virtual Surround Manager filter graph, so a
    single 8-channel virtual device reproduces the Windows chain
    (EqualizerAPO + Real3D-VST -> HeSuVi), channel detection included. The
    output limiter is inserted after the output gain when the user enables it.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ladspa.h"
#include "freesurround_decoder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

// Live detector state for the application UI. The filter-chain module loads
// this plugin inside the application process, so the app can dlopen() the same
// object and read this atomic: 1 = stereo (upmixing), 0 = multichannel
// (bypassing), -1 = not active yet. Visibility is forced because the build
// otherwise uses hidden visibility for plugin sources.
extern "C" __attribute__((visibility("default"))) std::atomic<int> vsm_auto_upmix_state{-1};

// Monotonic process counter, used by the application to tell whether audio is
// actually flowing through the graph (the node state alone proved unreliable
// on some sessions). It is deliberately not reset on activation: the count
// keeps increasing across graph replacements in the same process.
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_auto_blocks{0};

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
// Local patch (not in Real3D-VST): re-engaging the upmixer flushes its FIFOs, which
// is audible as a short click. Content hovering right at the detection threshold
// must not toggle the detector, so after a bypass the input has to stay pure stereo
// for one second before upmixing resumes. Expressed in samples, not blocks, so the
// behaviour is independent of the host quantum.
constexpr double kStereoToleranceSeconds = 1.0;
// Local patch (not in Real3D-VST): crossfade instead of a hard path switch. The
// upmix and bypass paths are both kLatency samples delayed, so they represent the
// same input time and can be blended without an echo or a time jump.
constexpr double kCrossfadeSeconds = 0.020;

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
    CTRL_LFE_SEND, // auto plugin: mono main-sum send written into the LFE output
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
    LADSPA_Data *lfeSend = nullptr;
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
    case CTRL_LFE_SEND: c.lfeSend = data; break;
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

    // Advance the output FIFO without emitting anything. Used while the plugin
    // is bypassing so the upmix core stays primed: on the next switch to upmix
    // the FIFO already holds the processed audio and the output does not start
    // with a silent fill-up period.
    void discardOutput(unsigned long n) {
        const std::size_t outSize = outFifo[0].size();
        const std::size_t take = std::min<std::size_t>(n, outFifoFill);
        outFifoReadIdx = (outFifoReadIdx + take) % outSize;
        outFifoFill -= take;
    }

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
    std::vector<float> upmix[kNumOut];
    std::vector<float> bypassOut[kNumOut];
    std::size_t bypassWriteIdx = 0;

    // steadyUpmix is the last settled path. While a crossfade runs, the output
    // is a per-sample blend of both paths and steadyUpmix still refers to the
    // path being faded out.
    bool steadyUpmix = true;
    bool fading = false;
    bool fadeToBypass = false;
    int fadeElapsed = 0;
    int fadeTotal = 0;
    int crossfadeSamples = 0;
    int stereoSamplesTarget = 0;
    long long nonStereoSamples = 0;
    long long stereoSamples = 0;

    // Write the newest samples of every input channel into the bypass delay
    // ring. Kept current in *both* modes so a future switch to bypass starts
    // with real audio instead of the stale/zero ring.
    void updateBypassRing(unsigned long n) {
        const std::size_t kLat = (std::size_t)kLatency;
        const unsigned long m = std::min<unsigned long>(n, kLatency);
        const std::size_t start = (bypassWriteIdx + (n - m)) % kLat;
        for (int c = 0; c < kNumOut; ++c) {
            std::vector<float> &buf = bypass[c];
            const LADSPA_Data *src = in[c];
            for (unsigned long j = 0; j < m; ++j)
                buf[(start + j) % kLat] = src ? src[n - m + j] : 0.0f;
        }
        bypassWriteIdx = (bypassWriteIdx + n) % kLat;
    }

    // Read the delayed bypass signal before the ring is advanced for this
    // block. Native LFE gets its +6 dB makeup here, only in this branch.
    void readBypass(unsigned long n) {
        const std::size_t kLat = (std::size_t)kLatency;
        for (int c = 0; c < kNumOut; ++c) {
            const float gain = (c == 3) ? 2.0f : 1.0f;
            const LADSPA_Data *src = in[c];
            std::vector<float> &ring = bypass[c];
            std::vector<float> &dst = bypassOut[c];
            if (n < (unsigned long)kLatency) {
                for (unsigned long s = 0; s < n; ++s)
                    dst[s] = gain * ring[(bypassWriteIdx + s) % kLat];
            } else {
                for (unsigned long s = 0; s < (unsigned long)kLatency; ++s)
                    dst[s] = gain * ring[(bypassWriteIdx + s) % kLat];
                for (unsigned long s = (unsigned long)kLatency; s < n; ++s)
                    dst[s] = gain * (src ? src[s - kLatency] : 0.0f);
            }
        }
    }

    // Write the upmixer's output into temporary buffers and replace its LFE
    // channel with the optional mono main-sum send. The LFE send is applied
    // only here: the native LFE (silent while upmixing) must not be doubled.
    void writeUpmix(unsigned long n) {
        LADSPA_Data *ports[kNumOut];
        for (int c = 0; c < kNumOut; ++c)
            ports[c] = upmix[c].data();
        core.writeOut(ports, n);

        const float send = ctrl.lfeSend ? *ctrl.lfeSend : 0.0f;
        if (send == 0.0f) {
            std::memset(upmix[3].data(), 0, sizeof(LADSPA_Data) * n);
        } else {
            for (unsigned long s = 0; s < n; ++s) {
                float sum = 0.0f;
                for (const int c : {0, 1, 2, 4, 5, 6, 7})
                    sum += upmix[c][s];
                upmix[3][s] = send * sum;
            }
        }
    }

    void zeroUpmix(unsigned long n) {
        for (int c = 0; c < kNumOut; ++c)
            std::memset(upmix[c].data(), 0, sizeof(LADSPA_Data) * n);
    }
};

LADSPA_Handle autoInstantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    AutoInstance *self = new AutoInstance();
    self->sampleRate = sampleRate ? sampleRate : 48000;
    self->core.init(self->sampleRate);
    const std::size_t maxBlock = 4u * kFftSize;
    for (int c = 0; c < kNumOut; ++c) {
        self->bypass[c].assign(kLatency, 0.0f);
        self->upmix[c].assign(maxBlock, 0.0f);
        self->bypassOut[c].assign(maxBlock, 0.0f);
    }
    self->crossfadeSamples = std::max(1, (int)std::llround(self->sampleRate * kCrossfadeSeconds));
    self->stereoSamplesTarget = std::max(1, (int)std::llround(self->sampleRate * kStereoToleranceSeconds));
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
    for (int c = 0; c < kNumOut; ++c) {
        std::fill(self->bypass[c].begin(), self->bypass[c].end(), 0.0f);
        std::fill(self->upmix[c].begin(), self->upmix[c].end(), 0.0f);
        std::fill(self->bypassOut[c].begin(), self->bypassOut[c].end(), 0.0f);
    }
    self->bypassWriteIdx = 0;
    self->steadyUpmix = true;
    self->fading = false;
    self->fadeToBypass = false;
    self->fadeElapsed = 0;
    self->fadeTotal = 0;
    self->nonStereoSamples = 0;
    self->stereoSamples = self->stereoSamplesTarget; // start upmixing immediately
    vsm_auto_upmix_state.store(-1, std::memory_order_relaxed);
}

void autoRun(LADSPA_Handle handle, unsigned long n) {
    AutoInstance *self = (AutoInstance *)handle;
    if (!self->core.decoder || n == 0) return;

    vsm_auto_blocks.fetch_add(1, std::memory_order_relaxed);
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);

    // ---- stereo detection, same sample rule as the VST plugin ----
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
        self->stereoSamples += (long long)n;
        self->nonStereoSamples = 0;
    } else {
        self->nonStereoSamples += (long long)n;
        self->stereoSamples = 0;
    }

    const bool upmixAllowed = !self->ctrl.upmixEnable || *self->ctrl.upmixEnable > 0.5f;
    bool targetUpmix = upmixAllowed;
    if (upmixAllowed) {
        if (self->steadyUpmix) {
            // Keep upmixing until the native content has traversed the bypass
            // delay line *and* the crossfade is over. Starting at exactly
            // kLatency would blend with a bypass path that still contains the
            // old (pre-transition) content for the first fade samples.
            targetUpmix = self->nonStereoSamples < (long long)kLatency + self->crossfadeSamples;
        } else {
            targetUpmix = self->stereoSamples >= self->stereoSamplesTarget;
        }
    }

    // Both paths stay primed: feed the upmix core and keep the bypass ring
    // current every block, whichever path currently drives the output.
    self->core.feed(self->in[0], self->in[1], n);
    self->readBypass(n);

    if (!self->fading && targetUpmix != self->steadyUpmix) {
        self->fading = true;
        self->fadeToBypass = !targetUpmix;
        self->fadeElapsed = 0;
        self->fadeTotal = self->crossfadeSamples;
    }

    const bool needUpmix = self->steadyUpmix || self->fading;
    bool haveUpmix = false;
    if (needUpmix) {
        haveUpmix = self->core.hasOutput(n);
        if (haveUpmix)
            self->writeUpmix(n);
        else
            self->zeroUpmix(n);
    } else {
        // Bypass steady state: consume the upmix output without emitting it so
        // its read pointer stays aligned with the bypass delay line.
        self->core.discardOutput(n);
    }

    self->updateBypassRing(n);

    if (!self->fading) {
        for (int c = 0; c < kNumOut; ++c) {
            if (!self->out[c]) continue;
            const std::vector<float> &src = self->steadyUpmix ? self->upmix[c] : self->bypassOut[c];
            std::memcpy(self->out[c], src.data(), sizeof(LADSPA_Data) * n);
        }
        vsm_auto_upmix_state.store(self->steadyUpmix ? 1 : 0, std::memory_order_relaxed);
        return;
    }

    // Per-sample crossfade. Both paths are kLatency delayed, so this blends the
    // same input time; a 20 ms ramp removes the sample-step click that a hard
    // path switch produced (measured 0.26 for a 1 kHz/0.3 tone).
    for (int c = 0; c < kNumOut; ++c) {
        if (!self->out[c]) continue;
        for (unsigned long s = 0; s < n; ++s) {
            float a = self->fadeTotal > 0
                          ? (float)(self->fadeElapsed + (int)s) / (float)self->fadeTotal
                          : 1.0f;
            if (a > 1.0f)
                a = 1.0f;
            if (!self->fadeToBypass)
                a = 1.0f - a;
            self->out[c][s] = self->upmix[c][s] * (1.0f - a) + self->bypassOut[c][s] * a;
        }
    }
    self->fadeElapsed += (int)n;
    if (self->fadeElapsed >= self->fadeTotal) {
        self->steadyUpmix = !self->fadeToBypass;
        self->fading = false;
        self->fadeElapsed = 0;
        self->fadeTotal = 0;
    }
    vsm_auto_upmix_state.store(self->steadyUpmix ? 1 : 0, std::memory_order_relaxed);
}

void autoCleanup(LADSPA_Handle handle) {
    AutoInstance *self = (AutoInstance *)handle;
    self->core.destroy();
    delete self;
}

//----------------------------------------------------------------------------
// plugin 3: output_limiter - stereo-linked lookahead peak limiter
//
// The app's normal output is far below full scale, but impact mode can add
// 6 dB and a full-scale native 7.1 bass passage can still clip the device.
// This optional safety stage is transparent below the threshold: it only adds
// the lookahead delay (5 ms by default) and leaves the gain at unity. Above
// the threshold the gain is reduced ahead of the peak, with a slow release.
//----------------------------------------------------------------------------
struct LimiterInstance {
    unsigned long sampleRate = 48000;
    LADSPA_Data *in[2] = {};
    LADSPA_Data *out[2] = {};
    LADSPA_Data *thresholdDb = nullptr;
    LADSPA_Data *releaseMs = nullptr;
    LADSPA_Data *lookaheadMs = nullptr;
    LADSPA_Data *latencyOut = nullptr;

    std::vector<float> delay[2];
    std::vector<float> peaks;
    int maxDelay = 0;
    int delayLen = 0;
    int writeIdx = 0;
    int peakWrite = 0;
    float gain = 1.0f;
    float threshold = 0.841395f; // -1.5 dBFS
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    void init(unsigned long sr) {
        sampleRate = sr ? sr : 48000;
        maxDelay = std::max(1, (int)std::llround(sampleRate * 0.050));
        for (int c = 0; c < 2; ++c)
            delay[c].assign(maxDelay, 0.0f);
        peaks.assign(maxDelay + 1, 0.0f);
        configure();
        reset();
    }

    void configure() {
        const float db = thresholdDb ? *thresholdDb : -1.5f;
        threshold = std::pow(10.0f, db / 20.0f);
        const float rel = releaseMs ? *releaseMs : 100.0f;
        const float look = lookaheadMs ? *lookaheadMs : 5.0f;
        delayLen = std::clamp((int)std::llround((double)look * sampleRate / 1000.0), 0, maxDelay);
        const int attackSamples = std::max(1, delayLen / 4);
        attackCoeff = std::exp(-1.0f / (float)attackSamples);
        const int releaseSamples = std::max(1, (int)std::llround((double)rel * sampleRate / 1000.0));
        releaseCoeff = std::exp(-1.0f / (float)releaseSamples);
        if (latencyOut)
            *latencyOut = (float)delayLen;
    }

    void reset() {
        for (int c = 0; c < 2; ++c)
            std::fill(delay[c].begin(), delay[c].end(), 0.0f);
        std::fill(peaks.begin(), peaks.end(), 0.0f);
        writeIdx = 0;
        peakWrite = 0;
        gain = 1.0f;
    }
};

LADSPA_Handle limiterInstantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    LimiterInstance *self = new LimiterInstance();
    self->init(sampleRate);
    return (LADSPA_Handle)self;
}

void limiterConnectPort(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    LimiterInstance *self = (LimiterInstance *)handle;
    switch (port) {
    case 0: self->in[0] = data; break;
    case 1: self->in[1] = data; break;
    case 2: self->out[0] = data; break;
    case 3: self->out[1] = data; break;
    case 4: self->thresholdDb = data; break;
    case 5: self->releaseMs = data; break;
    case 6: self->lookaheadMs = data; break;
    case 7: self->latencyOut = data; break;
    default: break;
    }
}

void limiterActivate(LADSPA_Handle handle) {
    LimiterInstance *self = (LimiterInstance *)handle;
    self->configure();
    self->reset();
}

void limiterRun(LADSPA_Handle handle, unsigned long n) {
    LimiterInstance *self = (LimiterInstance *)handle;
    if (!self->in[0] || !self->in[1] || !self->out[0] || !self->out[1])
        return;

    const int size = (int)self->peaks.size();
    for (unsigned long s = 0; s < n; ++s) {
        const float l = self->in[0][s];
        const float r = self->in[1][s];
        const float peak = std::max(std::fabs(l), std::fabs(r));

        self->peaks[self->peakWrite] = peak;
        self->peakWrite = (self->peakWrite + 1) % size;

        float window_peak = 0.0f;
        for (int j = 0; j <= self->delayLen; ++j) {
            const int idx = (self->peakWrite - 1 - j + size) % size;
            window_peak = std::max(window_peak, self->peaks[idx]);
        }
        const float target = window_peak > self->threshold ? self->threshold / window_peak : 1.0f;
        if (target < self->gain)
            self->gain = self->attackCoeff * self->gain + (1.0f - self->attackCoeff) * target;
        else
            self->gain = self->releaseCoeff * self->gain + (1.0f - self->releaseCoeff) * target;

        float dl = l;
        float dr = r;
        if (self->delayLen > 0) {
            const int read = (self->writeIdx - self->delayLen + self->maxDelay) % self->maxDelay;
            dl = self->delay[0][read];
            dr = self->delay[1][read];
        }
        self->delay[0][self->writeIdx] = l;
        self->delay[1][self->writeIdx] = r;
        self->writeIdx = (self->writeIdx + 1) % self->maxDelay;

        self->out[0][s] = dl * self->gain;
        self->out[1][s] = dr * self->gain;
    }
}

void limiterCleanup(LADSPA_Handle handle) {
    delete (LimiterInstance *)handle;
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
    "upmix_enable", "latency", "lfe_send",
};

const LADSPA_PortDescriptor autoPortDescriptors[] = {
    AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN, AUDIO_IN,
    AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT, AUDIO_OUT,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_IN,
    CONTROL_IN,
    CONTROL_OUT, CONTROL_IN,
};

const LADSPA_PortRangeHint autoPortRangeHints[] = {
    NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT,
    NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT, NOHINT,
    C_HINTS,
    CTRL(LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f), // upmix_enable
    NOHINT,                                                        // latency (output)
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0, 0.0f, 1.0f), // lfe_send
};

const char *const limiterPortNames[] = {
    "In L", "In R", "Out L", "Out R",
    "Threshold (dBFS)", "Release (ms)", "Lookahead (ms)", "latency",
};

const LADSPA_PortDescriptor limiterPortDescriptors[] = {
    AUDIO_IN, AUDIO_IN, AUDIO_OUT, AUDIO_OUT,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_OUT,
};

const LADSPA_PortRangeHint limiterPortRangeHints[] = {
    NOHINT, NOHINT, NOHINT, NOHINT,
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, -20.0f, 0.0f), // threshold
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 1.0f, 1000.0f), // release
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 20.0f), // lookahead
    NOHINT, // latency (output)
};

const LADSPA_Descriptor descriptorLimiter = {
    .UniqueID = 0x52453346, // "RE3F"
    .Label = "output_limiter",
    .Name = "Stereo Lookahead Output Limiter",
    .Maker = "Virtual Surround Manager",
    .Copyright = "GPL-2.0-or-later",
    .PortCount = 8,
    .PortDescriptors = limiterPortDescriptors,
    .PortNames = limiterPortNames,
    .PortRangeHints = limiterPortRangeHints,
    .ImplementationData = nullptr,
    .instantiate = limiterInstantiate,
    .connect_port = limiterConnectPort,
    .activate = limiterActivate,
    .run = limiterRun,
    .run_adding = nullptr,
    .set_run_adding_gain = nullptr,
    .deactivate = nullptr,
    .cleanup = limiterCleanup,
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
    .PortCount = 29,
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

const LADSPA_Descriptor *const kDescriptors[] = {&descriptorStereo, &descriptorAuto, &descriptorLimiter};

} // namespace

extern "C" const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    return index < std::size(kDescriptors) ? kDescriptors[index] : nullptr;
}