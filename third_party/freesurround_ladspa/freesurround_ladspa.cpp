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
    3. label "output_limiter"        mandatory stereo-linked lookahead peak limiter
       in the application graph (-1.5 dBFS, 5 ms lookahead, 100 ms release).

    The "auto" version is used by the Virtual Surround Manager filter graph, so a
    single 8-channel virtual device reproduces the Windows chain
    (EqualizerAPO + Real3D-VST -> HeSuVi), channel detection included. The output
    limiter is the final safety stage before the application's output gate.

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ladspa.h"
#include "freesurround_decoder.h"
#include "dsp_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

// Live detector state for the application UI. Publishing is enabled only while
// the main window is visible, using the same switch as limiter telemetry. The
// filter-chain module loads this plugin inside the application process, so the
// app can dlopen() the same object and read this atomic: 1 = stereo (upmixing), 0 = multichannel
// (bypassing), -1 = not active yet. Visibility is forced because the build
// otherwise uses hidden visibility for plugin sources.
extern "C" __attribute__((visibility("default"))) std::atomic<int> vsm_auto_upmix_state{-1};

// Monotonic process counter, used by the application to tell whether audio is
// actually flowing through the graph. It advances only while UI telemetry is
// enabled and is deliberately not reset on activation.
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_auto_blocks{0};

// Limiter telemetry for the active effects graph. Values are cumulative since
// the application resets them after a graph becomes active. Only samples with
// non-silent delayed output are counted, so idle time cannot dilute the active
// reduction ratio. These atomics are diagnostic and never feed back into DSP.
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint32_t> vsm_limiter_current_gain_bits{0x3f800000};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint32_t> vsm_limiter_min_gain_bits{0x3f800000};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_limiter_active_samples{0};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_limiter_reduced_samples{0};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_limiter_latest_generation{0};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint64_t> vsm_limiter_active_generation{0};
extern "C" __attribute__((visibility("default"))) std::atomic<std::uint32_t> vsm_limiter_interval_peak_bits{0};
extern "C" __attribute__((visibility("default"))) std::atomic<bool> vsm_limiter_statistics_enabled{false};

namespace {

constexpr int kFftSize = VsmDsp::fft_size;         // 4096: 11.7 Hz bins at 48k, 128 ms total latency
constexpr int kNumOut = 8;
constexpr int kLatency = VsmDsp::upmix_latency;
// Digital classifier threshold (~-90.46 dBFS), chosen to ignore dummy-channel
// dither. dBFS alone does not establish physical audibility; quiet real surround
// content can also fall below it.
constexpr float kStereoEpsilon = 3e-5f;
// One-second release hysteresis avoids repeated classification changes near
// the threshold. Both DSP paths remain warm; no FIFO reset occurs at a switch.
// Expressed in samples so the behavior does not depend on the host quantum.
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
    const auto value = [](const LADSPA_Data *port, float fallback, float low, float high) {
        return port && std::isfinite(*port) ? std::clamp(*port, low, high) : fallback;
    };
    if (c.centerImage) decoder->center_image(value(c.centerImage, 0.7f, 0.0f, 1.0f));
    if (c.shift) decoder->shift(value(c.shift, 0.0f, -1.0f, 1.0f));
    if (c.depth) decoder->depth(value(c.depth, 1.0f, 0.0f, 4.0f));
    if (c.circularWrap) decoder->circular_wrap(value(c.circularWrap, 90.0f, 0.0f, 360.0f));
    if (c.focus) decoder->focus(value(c.focus, 0.0f, -1.0f, 1.0f));
    if (c.frontSep) decoder->front_separation(value(c.frontSep, 1.0f, 0.0f, 2.0f));
    if (c.rearSep) decoder->rear_separation(value(c.rearSep, 1.0f, 0.0f, 2.0f));
    if (c.useLfe) decoder->bass_redirection(value(c.useLfe, 0.0f, 0.0f, 1.0f) > 0.5f);
    const float hi = value(c.bassHi, 90.0f, 0.0f, std::min(150.0f, srNyquist));
    if (c.bassLo) decoder->low_cutoff(value(c.bassLo, 70.0f, 0.0f, hi) / srNyquist);
    if (c.bassHi) decoder->high_cutoff(hi / srNyquist);
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
        outFifoReadIdx = 0;
        // The decoder adds N/2; explicitly queue N zeros to make the total
        // N + N/2 samples, independent of the host's run() partitioning.
        outFifoWriteIdx = outFifoFill = kFftSize;
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
            inFifo[0][w] = l && std::isfinite(l[s]) ? l[s] : 0.0f;
            inFifo[1][w] = r && std::isfinite(r[s]) ? r[s] : 0.0f;
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

    void process(const LADSPA_Data *l, const LADSPA_Data *r, float output[kNumOut]) {
        feed(l, r, 1);
        const std::size_t size = outFifo[0].size();
        for (int c = 0; c < kNumOut; ++c)
            output[c] = outFifo[map.outIndex[c]][outFifoReadIdx];
        outFifoReadIdx = (outFifoReadIdx + 1) % size;
        --outFifoFill;
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
    if (port == 20)
        self->ctrl.latencyOut = data;
    else
        connectControl(self->ctrl, port - 10, data);
    if (self->ctrl.latencyOut) *self->ctrl.latencyOut = (float)kLatency;
}

void stereoActivate(LADSPA_Handle handle) {
    StereoInstance *self = (StereoInstance *)handle;
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    self->core.reset();
    if (self->ctrl.latencyOut) *self->ctrl.latencyOut = static_cast<float>(kLatency);
}

void stereoRun(LADSPA_Handle handle, unsigned long n) {
    StereoInstance *self = (StereoInstance *)handle;
    if (!self->core.decoder || n == 0) return;
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    for (unsigned long s = 0; s < n; ++s) {
        float output[kNumOut];
        self->core.process(self->inL ? self->inL + s : nullptr,
                           self->inR ? self->inR + s : nullptr, output);
        for (int c = 0; c < kNumOut; ++c)
            if (self->out[c]) self->out[c][s] = output[c];
    }
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
    std::vector<unsigned char> decisions;
    std::size_t bypassWriteIdx = 0;
    std::size_t decisionWriteIdx = 0;
    unsigned long stereoSamplesTarget = 0;
    unsigned long bypassHold = 0;
    int crossfadeSamples = 0;
    float bypassMix = 0.0f;
};

LADSPA_Handle autoInstantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    AutoInstance *self = new AutoInstance();
    self->sampleRate = sampleRate ? sampleRate : 48000;
    self->core.init(self->sampleRate);
    for (int c = 0; c < kNumOut; ++c)
        self->bypass[c].assign(kLatency, 0.0f);
    self->crossfadeSamples = std::clamp((int)std::llround(self->sampleRate * kCrossfadeSeconds), 1, kLatency);
    self->stereoSamplesTarget = std::max(1ul, (unsigned long)std::llround(self->sampleRate * kStereoToleranceSeconds));
    // Decisions travel with the audio. Remember short native events through
    // the one-second hold; do not fade before that audio reaches the output.
    self->decisions.assign(kLatency, 0);
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
    for (int c = 0; c < kNumOut; ++c)
        std::fill(self->bypass[c].begin(), self->bypass[c].end(), 0.0f);
    const bool bypass = self->ctrl.upmixEnable && *self->ctrl.upmixEnable <= 0.5f;
    std::fill(self->decisions.begin(), self->decisions.end(), bypass);
    self->bypassWriteIdx = self->decisionWriteIdx = 0;
    self->bypassHold = 0;
    self->bypassMix = bypass ? 1.0f : 0.0f;
    vsm_auto_upmix_state.store(-1, std::memory_order_relaxed);
}

void autoRun(LADSPA_Handle handle, unsigned long n) {
    AutoInstance *self = (AutoInstance *)handle;
    if (!self->core.decoder || n == 0) return;
    const bool publishState = vsm_limiter_statistics_enabled.load(std::memory_order_relaxed);
    if (publishState)
        vsm_auto_blocks.fetch_add(1, std::memory_order_relaxed);
    syncControls(self->core.decoder, self->ctrl, self->sampleRate);
    const bool upmixAllowed = !self->ctrl.upmixEnable || *self->ctrl.upmixEnable > 0.5f;
    // 0 = no send, 0.501 = balanced's historical -6 dB, 1.0 = impact's 0 dB.
    // The graph writes this control at runtime (no graph rebuild).
    const float send = self->ctrl.lfeSend && std::isfinite(*self->ctrl.lfeSend)
        ? std::clamp<float>(*self->ctrl.lfeSend, 0.0f, 1.0f) : 0.0f;
    const float fadeStep = 1.0f / self->crossfadeSamples;
    for (unsigned long s = 0; s < n; ++s) {
        float input[kNumOut], native[kNumOut], upmix[kNumOut];
        bool extra = false;
        for (int c = 0; c < kNumOut; ++c) {
            input[c] = self->in[c] ? self->in[c][s] : 0.0f;
            if (!std::isfinite(input[c])) input[c] = 0.0f;
            if (c >= 2 && std::fabs(input[c]) > kStereoEpsilon) extra = true;
            native[c] = self->bypass[c][self->bypassWriteIdx] *
                        (c == 3 ? VsmDsp::native_lfe_gain : 1.0f);
            self->bypass[c][self->bypassWriteIdx] = input[c];
        }
        self->bypassWriteIdx = (self->bypassWriteIdx + 1) % kLatency;
        if (extra)
            self->bypassHold = self->stereoSamplesTarget;
        else if (self->bypassHold > 0)
            --self->bypassHold;
        const bool bypass = self->decisions[self->decisionWriteIdx] != 0;
        self->decisions[self->decisionWriteIdx] = !upmixAllowed || self->bypassHold > 0;
        self->decisionWriteIdx = (self->decisionWriteIdx + 1) % self->decisions.size();
        self->bypassMix = std::clamp(self->bypassMix + (bypass ? fadeStep : -fadeStep), 0.0f, 1.0f);

        self->core.process(&input[0], &input[1], upmix);
        // Optional mono LFE send, as in the pre-shelf architecture. It is
        // summed from the seven upmix main outputs and rendered through the
        // graph's diffuse LFE path, so it does not inherit the front HRIR
        // image. The native LFE is silent while upmixing; in bypass the
        // native[c] term below carries its calibrated makeup instead.
        if (send > 0.0f) {
            upmix[3] = send * (upmix[0] + upmix[1] + upmix[2] +
                               upmix[4] + upmix[5] + upmix[6] + upmix[7]);
        } else {
            upmix[3] = 0.0f;
        }
        for (int c = 0; c < kNumOut; ++c) {
            if (!self->out[c]) continue;
            // Native discrete channels are never faded away by a stereo
            // classifier. Only the synthesized stereo field/LR bypass blend.
            self->out[c][s] = c < 2
                ? upmix[c] * (1.0f - self->bypassMix) + native[c] * self->bypassMix
                : upmix[c] * (1.0f - self->bypassMix) + native[c];
        }
    }
    if (publishState)
        vsm_auto_upmix_state.store(self->bypassMix < 0.5f ? 1 : 0, std::memory_order_relaxed);
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
    LADSPA_Data *statisticsGeneration = nullptr;

    // 8x, 48-tap/polyphase windowed-sinc reconstruction detector. Audio stays
    // at the native rate; only the detector is oversampled, avoiding an extra
    // resampling pass through the programme signal. The history is duplicated
    // so the 48-tap window is linear and the inner loop has no modulo.
    std::array<std::array<double, 48>, 8> interpolation{};
    std::array<std::array<double, 48>, 8> interpolationRev{};
    std::array<std::array<float, 96>, 2> detectorHistory{};
    unsigned detectorWrite = 0;
    int detectorDelay = 24;

    std::vector<float> delay[2];
    std::vector<float> peaks;
    std::vector<int> peakQueue;
    unsigned queueHead = 0, queueTail = 0;
    uint64_t sampleIndex = 0;
    float lastThresholdDb = NAN;
    float lastReleaseMs = NAN;
    float lastLookaheadMs = NAN;
    int maxDelay = 0;
    int delayLen = 0;
    int writeIdx = 0;
    int peakWrite = 0;
    float gain = 1.0f;
    float threshold = 0.841395f; // -1.5 dBFS
    float releaseCoeff = 0.0f;
    std::uint64_t generation = 0;

    void init(unsigned long sr) {
        generation = vsm_limiter_latest_generation.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t inactive = 0;
        if (vsm_limiter_active_generation.compare_exchange_strong(inactive, generation, std::memory_order_relaxed)) {
            vsm_limiter_current_gain_bits.store(std::bit_cast<std::uint32_t>(1.0f), std::memory_order_relaxed);
            vsm_limiter_min_gain_bits.store(std::bit_cast<std::uint32_t>(1.0f), std::memory_order_relaxed);
            vsm_limiter_active_samples.store(0, std::memory_order_relaxed);
            vsm_limiter_reduced_samples.store(0, std::memory_order_relaxed);
            vsm_limiter_interval_peak_bits.store(0, std::memory_order_relaxed);
        }
        sampleRate = sr ? sr : 48000;
        maxDelay = std::max(1, (int)std::llround(sampleRate * 0.050));
        for (int c = 0; c < 2; ++c)
            delay[c].assign(maxDelay, 0.0f);
        peaks.assign(maxDelay + 64, 0.0f);
        peakQueue.assign(peaks.size(), 0);
        for (int phase = 0; phase < 8; ++phase) {
            double sum = 0;
            for (int tap = 0; tap < 48; ++tap) {
                const double x = tap - detectorDelay + phase / 8.0;
                const double window = 0.42 - 0.5 * std::cos(2 * M_PI * tap / 47) + 0.08 * std::cos(4 * M_PI * tap / 47);
                interpolation[phase][tap] = window * (x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x));
                sum += interpolation[phase][tap];
            }
            for (auto &v : interpolation[phase]) v /= sum;
            // Store the reversed kernel for a contiguous history read.
            for (int tap = 0; tap < 48; ++tap)
                interpolationRev[phase][tap] = interpolation[phase][47 - tap];
        }
        configure();
        reset();
    }

    void configure() {
        const auto control = [](const LADSPA_Data *p, float fallback, float low, float high) {
            return p && std::isfinite(*p) ? std::clamp(*p, low, high) : fallback;
        };
        const float db = control(thresholdDb, -1.5f, -20.0f, 0.0f);
        const float rel = control(releaseMs, 100.0f, 1.0f, 1000.0f);
        const float look = control(lookaheadMs, 5.0f, 0.0f, 20.0f);
        if (latencyOut)
            *latencyOut = static_cast<float>(delayLen);
        if (db == lastThresholdDb && rel == lastReleaseMs && look == lastLookaheadMs)
            return;
        lastThresholdDb = db;
        lastReleaseMs = rel;
        lastLookaheadMs = look;
        threshold = std::pow(10.0f, db / 20.0f);
        delayLen = std::clamp((int)std::llround((double)look * sampleRate / 1000.0), detectorDelay + 1, maxDelay);
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
        detectorWrite = 0;
        detectorHistory = {};
        queueHead = queueTail = 0;
        sampleIndex = 0;
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
    case 8: self->statisticsGeneration = data; break;
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

    self->configure();
    const bool collectStatistics = vsm_limiter_statistics_enabled.load(std::memory_order_relaxed);
    const float configuredGeneration = self->statisticsGeneration ? *self->statisticsGeneration : 0.0f;
    if (std::isfinite(configuredGeneration) && configuredGeneration >= 1.0f)
        self->generation = static_cast<std::uint64_t>(std::llround(configuredGeneration));
    const int size = (int)self->peaks.size();
    std::uint64_t activeSamples = 0;
    std::uint64_t reducedSamples = 0;
    float minimumGain = 1.0f;
    float intervalPeak = 0.0f;
    for (unsigned long s = 0; s < n; ++s) {
        const float l = std::isfinite(self->in[0][s]) ? self->in[0][s] : 0.0f;
        const float r = std::isfinite(self->in[1][s]) ? self->in[1][s] : 0.0f;
        self->detectorHistory[0][self->detectorWrite] = l;
        self->detectorHistory[1][self->detectorWrite] = r;
        self->detectorHistory[0][self->detectorWrite + 48] = l;
        self->detectorHistory[1][self->detectorWrite + 48] = r;
        const float *windowL = &self->detectorHistory[0][self->detectorWrite + 1];
        const float *windowR = &self->detectorHistory[1][self->detectorWrite + 1];
        double reconstructedPeak = 0;
        for (int phase = 0; phase < 8; ++phase) {
            double left = 0, right = 0;
            for (int tap = 0; tap < 48; ++tap) {
                const double coefficient = self->interpolationRev[phase][tap];
                left += coefficient * windowL[tap];
                right += coefficient * windowR[tap];
            }
            reconstructedPeak = std::max({reconstructedPeak, std::abs(left), std::abs(right)});
        }
        self->detectorWrite = (self->detectorWrite + 1) % 48;
        const float peak = static_cast<float>(std::max(reconstructedPeak, static_cast<double>(std::max(std::fabs(l), std::fabs(r)))));
        if (collectStatistics && peak > 1e-6f)
            intervalPeak = std::max(intervalPeak, peak);
        self->peaks[self->peakWrite] = peak;
        // Monotonic queue: O(1) peak window rather than a lookahead-length scan
        // for every audio sample. Hold past the detector delay and release
        // slowly enough to avoid creating new intersample modulation peaks.
        while (self->queueHead != self->queueTail) {
            const unsigned last = (self->queueTail + size - 1) % size;
            if (self->peaks[self->peakQueue[last]] > peak) break;
            self->queueTail = last;
        }
        self->peakQueue[self->queueTail] = self->peakWrite;
        self->queueTail = (self->queueTail + 1) % size;
        const int expired = (self->peakWrite + size - self->delayLen - self->detectorDelay - 2) % size;
        if (self->queueHead != self->queueTail && self->peakQueue[self->queueHead] == expired)
            self->queueHead = (self->queueHead + 1) % size;
        const float window_peak = self->peaks[self->peakQueue[self->queueHead]];
        self->peakWrite = (self->peakWrite + 1) % size;
        const float target = window_peak > self->threshold ? self->threshold / window_peak : 1.0f;
        if (target < self->gain)
            self->gain = target;
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

        // A finite exponential attack never reaches its target exactly. Enforce
        // the ceiling on the actual delayed stereo sample, including isolated
        // impulses and zero-lookahead operation, without unlinking the channels.
        const float delayedPeak = std::max(std::fabs(dl), std::fabs(dr));
        if (delayedPeak > self->threshold)
            self->gain = std::min(self->gain, self->threshold / delayedPeak);
        self->out[0][s] = dl * self->gain;
        self->out[1][s] = dr * self->gain;
        if (collectStatistics && delayedPeak > 1e-6f) { // -120 dBFS telemetry silence floor
            ++activeSamples;
            minimumGain = std::min(minimumGain, self->gain);
            if (self->gain < 0.9885531f) // at least 0.10 dB of gain reduction
                ++reducedSamples;
        }
    }
    if (collectStatistics &&
        self->generation == vsm_limiter_active_generation.load(std::memory_order_relaxed)) {
        vsm_limiter_current_gain_bits.store(std::bit_cast<std::uint32_t>(self->gain), std::memory_order_relaxed);
        std::uint32_t peakObserved = vsm_limiter_interval_peak_bits.load(std::memory_order_relaxed);
        const std::uint32_t peakCandidate = std::bit_cast<std::uint32_t>(intervalPeak);
        while (peakCandidate > peakObserved &&
               !vsm_limiter_interval_peak_bits.compare_exchange_weak(peakObserved, peakCandidate,
                                                                     std::memory_order_relaxed)) {
        }
        if (activeSamples == 0)
            return;
        vsm_limiter_active_samples.fetch_add(activeSamples, std::memory_order_relaxed);
        vsm_limiter_reduced_samples.fetch_add(reducedSamples, std::memory_order_relaxed);
        std::uint32_t observed = vsm_limiter_min_gain_bits.load(std::memory_order_relaxed);
        const std::uint32_t candidate = std::bit_cast<std::uint32_t>(minimumGain);
        while (candidate < observed &&
               !vsm_limiter_min_gain_bits.compare_exchange_weak(observed, candidate, std::memory_order_relaxed)) {
        }
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
    "Threshold (dBFS)", "Release (ms)", "Lookahead (ms)", "latency", "Statistics Generation",
};

const LADSPA_PortDescriptor limiterPortDescriptors[] = {
    AUDIO_IN, AUDIO_IN, AUDIO_OUT, AUDIO_OUT,
    CONTROL_IN, CONTROL_IN, CONTROL_IN, CONTROL_OUT, CONTROL_IN,
};

const LADSPA_PortRangeHint limiterPortRangeHints[] = {
    NOHINT, NOHINT, NOHINT, NOHINT,
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, -20.0f, 0.0f), // threshold
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 1.0f, 1000.0f), // release
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 0.0f, 20.0f), // lookahead
    NOHINT, // latency (output)
    CTRL(LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_0 | LADSPA_HINT_INTEGER, 0.0f, 0.0f), // statistics generation
};

const LADSPA_Descriptor descriptorLimiter = {
    .UniqueID = 0x52453346, // "RE3F"
    .Label = "output_limiter",
    .Name = "Stereo Lookahead Output Limiter",
    .Maker = "Virtual Surround Manager",
    .Copyright = "GPL-2.0-or-later",
    .PortCount = 9,
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