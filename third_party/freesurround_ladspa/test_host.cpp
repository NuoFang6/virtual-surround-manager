/*
    Host-side regression test for the FreeSurround LADSPA plugins (no PipeWire).

    Covers:
      - freesurround71 stereo upmix smoke levels (440 Hz -> FC, 60 Hz -> LFE)
      - freesurround71_auto stereo <-> multichannel transitions without a gap
      - the native-LFE bypass makeup (2x, +6 dB)

    Build: cmake -DBUILD_LADSPA_TEST_HOST=ON .. && cmake --build . --target freesurround_ladspa_test
    Run:   ./freesurround_ladspa_test path/to/freesurround_ladspa.so
*/
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ladspa.h"

namespace {

constexpr unsigned kBlock = 512;
constexpr double kRate = 48000.0;
constexpr int kNumCh = 8;

struct Rig {
    const LADSPA_Descriptor *d = nullptr;
    LADSPA_Handle h = nullptr;
    std::vector<float> in[kNumCh], out[kNumCh];
    float center = 0.7f, shift = 0.0f, depth = 1.0f, wrap = 90.0f, focus = 0.0f;
    float fsep = 1.0f, rsep = 1.0f, bassLo = 70.0f, bassHi = 90.0f;
    float useLfe = 1.0f, upmixEnable = 1.0f, latency = 0.0f, lfeSend = 0.0f;
    float prevOut[kNumCh] = {};
    double maxStep[kNumCh] = {};

    bool init(const LADSPA_Descriptor *desc) {
        d = desc;
        h = d->instantiate(d, (unsigned long)kRate);
        if (!h)
            return false;
        for (int c = 0; c < kNumCh; ++c) {
            in[c].assign(kBlock, 0.0f);
            out[c].assign(kBlock, 0.0f);
        }
        for (unsigned long i = 0; i < d->PortCount; ++i) {
            const char *n = d->PortNames[i];
            LADSPA_Data *ptr = nullptr;
            static const char *ins[8] = {"In FL", "In FR", "In FC", "In LFE", "In RL", "In RR", "In SL", "In SR"};
            static const char *insStereo[2] = {"In L", "In R"};
            static const char *outs[8] = {"Out FL", "Out FR", "Out FC", "Out LFE", "Out RL", "Out RR", "Out SL", "Out SR"};
            for (int c = 0; c < 8; ++c) {
                if (!strcmp(n, ins[c]))
                    ptr = in[c].data();
                if (!strcmp(n, outs[c]))
                    ptr = out[c].data();
            }
            for (int c = 0; c < 2; ++c)
                if (!strcmp(n, insStereo[c]))
                    ptr = in[c].data();
            if (!strcmp(n, "center_image"))
                ptr = &center;
            else if (!strcmp(n, "shift"))
                ptr = &shift;
            else if (!strcmp(n, "depth"))
                ptr = &depth;
            else if (!strcmp(n, "circular_wrap"))
                ptr = &wrap;
            else if (!strcmp(n, "focus"))
                ptr = &focus;
            else if (!strcmp(n, "front_sep"))
                ptr = &fsep;
            else if (!strcmp(n, "rear_sep"))
                ptr = &rsep;
            else if (!strcmp(n, "bass_lo"))
                ptr = &bassLo;
            else if (!strcmp(n, "bass_hi"))
                ptr = &bassHi;
            else if (!strcmp(n, "use_lfe"))
                ptr = &useLfe;
            else if (!strcmp(n, "upmix_enable"))
                ptr = &upmixEnable;
            else if (!strcmp(n, "latency"))
                ptr = &latency;
            else if (!strcmp(n, "lfe_send"))
                ptr = &lfeSend;
            if (ptr)
                d->connect_port(h, i, ptr);
        }
        activate();
        return true;
    }

    void activate() {
        if (d->deactivate)
            d->deactivate(h);
        if (d->activate)
            d->activate(h);
        for (int c = 0; c < kNumCh; ++c) {
            prevOut[c] = 0.0f;
            maxStep[c] = 0.0;
        }
    }

    // mask bit 0 = FL/L, 1 = FR/R, 2 = FC, 3 = LFE
    void block(long long t, unsigned mask) {
        const double wLR = 2.0 * M_PI * 440.0 / kRate;
        const double wC = 2.0 * M_PI * 1000.0 / kRate;
        const double wSub = 2.0 * M_PI * 40.0 / kRate;
        for (unsigned s = 0; s < kBlock; ++s) {
            const long long i = t + s;
            in[0][s] = (mask & 1) ? (float)(0.1 * sin(wLR * i)) : 0.0f;
            in[1][s] = (mask & 2) ? (float)(0.1 * sin(wLR * i)) : 0.0f;
            in[2][s] = (mask & 4) ? (float)(0.1 * sin(wC * i)) : 0.0f;
            in[3][s] = (mask & 8) ? (float)(0.25 * sin(wSub * i)) : 0.0f;
            for (int c = 4; c < kNumCh; ++c)
                in[c][s] = 0.0f;
        }
        d->run(h, kBlock);
        for (int c = 0; c < kNumCh; ++c) {
            for (unsigned s = 0; s < kBlock; ++s) {
                const double step = std::fabs((double)out[c][s] - (double)prevOut[c]);
                maxStep[c] = std::max(maxStep[c], step);
                prevOut[c] = out[c][s];
            }
        }
    }

    void resetSteps() {
        for (int c = 0; c < kNumCh; ++c)
            maxStep[c] = 0.0;
    }

    double maxStepFor(int c) const { return maxStep[c]; }

    double rms(int c) const {
        double acc = 0.0;
        for (float x : out[c])
            acc += (double)x * x;
        return sqrt(acc / out[c].size());
    }

    double totalRms() const {
        double acc = 0.0;
        for (int c = 0; c < kNumCh; ++c)
            for (float x : out[c])
                acc += (double)x * x;
        return sqrt(acc / (kNumCh * kBlock));
    }

    void cleanup() {
        if (d && d->cleanup)
            d->cleanup(h);
        h = nullptr;
    }
};

int failures = 0;

void check(const char *what, double value, double expected, double tolerance) {
    const bool ok = std::fabs(value - expected) <= tolerance;
    printf("  [%s] %s: %.5f (expected %.5f +/- %.5f)\n", ok ? "PASS" : "FAIL", what, value, expected, tolerance);
    if (!ok)
        ++failures;
}

void stereoSmoke(const LADSPA_Descriptor *desc) {
    printf("\nfreesurround71 (stereo upmix smoke test)\n");
    for (double freq : {440.0, 60.0}) {
        Rig r;
        if (!r.init(desc)) {
            printf("  [FAIL] instantiate\n");
            ++failures;
            return;
        }
        double phase = 0.0;
        double peak[kNumCh] = {0};
        for (int b = 0; b < 40; ++b) {
            for (unsigned s = 0; s < kBlock; ++s) {
                const double v = 0.5 * sin(phase);
                phase += 2.0 * M_PI * freq / kRate;
                r.in[0][s] = (float)v;
                r.in[1][s] = (float)v;
            }
            r.d->run(r.h, kBlock);
            if (b >= 32)
                for (int c = 0; c < kNumCh; ++c)
                    for (unsigned s = 0; s < kBlock; ++s)
                        peak[c] = std::max(peak[c], (double)std::fabs(r.out[c][s]));
        }
        printf("  input %.0f Hz, L=R, amplitude 0.5 -> FC=%.4f LFE=%.4f\n", freq, peak[2], peak[3]);
        if (freq == 440.0) {
            check("440 Hz reaches FC", peak[2], 0.66, 0.05);
            check("440 Hz does not leak into LFE", peak[3], 0.0, 0.005);
        } else {
            check("60 Hz reaches LFE", peak[3], 0.71, 0.05);
        }
        r.cleanup();
    }
}

void autoTransitions(const LADSPA_Descriptor *desc) {
    printf("\nfreesurround71_auto (stereo <-> multichannel transitions)\n");
    Rig r;
    if (!r.init(desc)) {
        printf("  [FAIL] instantiate\n");
        ++failures;
        return;
    }
    long long t = 0;
    // Steady upmix (centered stereo -> FC) ...
    for (int b = 0; b < 200; ++b) {
        r.block(t, 0x3);
        t += kBlock;
    }
    const double upmixTotal = r.totalRms();

    // ... add FC. The plugin must wait for kLatency + crossfade samples before
    // switching, so the native FC is already in the bypass delay line when the
    // crossfade starts. The old 2-block switch produced an 85 ms centre hole.
    r.resetSteps();
    double fcMin = 1e9, bypassMin = 1e9, bypassSteady = 0.0;
    for (int b = 0; b < 30; ++b) {
        r.block(t, 0x7);
        t += kBlock;
        fcMin = std::min(fcMin, r.rms(2));
        if (b >= 16) {
            bypassMin = std::min(bypassMin, r.totalRms());
            bypassSteady = r.totalRms();
        }
    }
    check("centre channel stays present through upmix->bypass (min block RMS)", fcMin, 0.11, 0.06);
    check("upmix->bypass keeps audio (min block RMS)", bypassMin, bypassSteady, bypassSteady * 0.5);
    check("no click at upmix->bypass", std::max(r.maxStepFor(0), r.maxStepFor(2)), 0.0, 0.02);

    // Drop FC: the detector re-engages upmixing after one second of pure stereo.
    double switchMin = 1e9;
    for (int b = 0; b < 120; ++b) {
        r.block(t, 0x3);
        t += kBlock;
        // The test drops the FC input abruptly; its last delayed samples leave
        // the bypass line after kLatency. That source step is not a plugin
        // click, so start the click measurement after it has passed.
        if (b == 20)
            r.resetSteps();
        if (b >= 90 && b <= 100)
            switchMin = std::min(switchMin, r.totalRms());
    }
    check("bypass->upmix keeps audio (min block RMS)", switchMin, upmixTotal, upmixTotal * 0.5);
    check("no click at bypass->upmix", std::max(r.maxStepFor(0), r.maxStepFor(2)), 0.0, 0.02);
    r.cleanup();
}

void autoSteadySteps(const LADSPA_Descriptor *desc) {
    printf("\nfreesurround71_auto (steady upmix block steps)\n");
    Rig r;
    if (!r.init(desc)) { printf("  [FAIL] instantiate\n"); ++failures; return; }
    long long t = 0;
    for (int b = 0; b < 200; ++b) { r.block(t, 0x3); t += kBlock; }
    r.resetSteps();
    for (int b = 0; b < 20; ++b) { r.block(t, 0x3); t += kBlock; }
    check("steady upmix FL block step", r.maxStepFor(0), 0.0, 0.02);
    check("steady upmix FC block step", r.maxStepFor(2), 0.0, 0.02);
    r.cleanup();
}

void autoLfeSend(const LADSPA_Descriptor *desc) {
    printf("\nfreesurround71_auto (upmix LFE send gain)\n");
    Rig r;
    if (!r.init(desc)) {
        printf("  [FAIL] instantiate\n");
        ++failures;
        return;
    }
    r.useLfe = 0.0f;
    r.lfeSend = 0.5f;
    long long t = 0;
    for (int b = 0; b < 200; ++b) {
        r.block(t, 0x3);
        t += kBlock;
    }
    double max_err = 0.0, max_send = 0.0;
    for (unsigned s = 0; s < kBlock; ++s) {
        const double expected = 0.5 * ((double)r.out[0][s] + r.out[1][s] + r.out[2][s] +
                                       r.out[4][s] + r.out[5][s] + r.out[6][s] + r.out[7][s]);
        max_err = std::max(max_err, std::fabs((double)r.out[3][s] - expected));
        max_send = std::max(max_send, std::fabs(expected));
    }
    check("upmix LFE send tracks 0.5 * main sum", max_err, 0.0, std::max(1e-5, max_send * 1e-5));
    r.cleanup();
}

void autoNativeLfe(const LADSPA_Descriptor *desc) {
    printf("\nfreesurround71_auto (native LFE makeup)\n");
    Rig r;
    if (!r.init(desc)) {
        printf("  [FAIL] instantiate\n");
        ++failures;
        return;
    }
    long long t = 0;
    // FC (0.1) + LFE (0.25) only: non-L/R content -> bypass. Wait out the
    // 6144-sample delay plus the 20 ms crossfade, then compare the steady last
    // blocks against the generated input (the block RMS of a 40 Hz tone is not
    // stable on its own).
    double inLfeAcc = 0, outLfeAcc = 0, inFcAcc = 0, outFcAcc = 0;
    for (int b = 0; b < 300; ++b) {
        r.block(t, 0xC);
        t += kBlock;
        if (b >= 292) {
            for (unsigned s = 0; s < kBlock; ++s) {
                inLfeAcc += (double)r.in[3][s] * r.in[3][s];
                outLfeAcc += (double)r.out[3][s] * r.out[3][s];
                inFcAcc += (double)r.in[2][s] * r.in[2][s];
                outFcAcc += (double)r.out[2][s] * r.out[2][s];
            }
        }
    }
    check("native LFE bypass gain", sqrt(outLfeAcc / inLfeAcc), 2.0, 0.05);
    check("FC bypass gain", sqrt(outFcAcc / inFcAcc), 1.0, 0.02);
    r.cleanup();
}

void limiterTest(const LADSPA_Descriptor *desc) {
    printf("\noutput_limiter (lookahead safety limiter)\n");
    if (!desc) { printf("  [FAIL] missing descriptor\n"); ++failures; return; }
    struct LimRig {
        const LADSPA_Descriptor *d = nullptr;
        LADSPA_Handle h = nullptr;
        float in[2][kBlock] = {};
        float out[2][kBlock] = {};
        float threshold = -1.5f, release = 100.0f, lookahead = 5.0f, latency = 0.0f;
        bool init(const LADSPA_Descriptor *descriptor) {
            d = descriptor;
            h = d->instantiate(d, (unsigned long)kRate);
            if (!h) return false;
            for (unsigned long i = 0; i < d->PortCount; ++i) {
                const char *n = d->PortNames[i];
                LADSPA_Data *ptr = nullptr;
                if (!strcmp(n, "In L")) ptr = in[0];
                else if (!strcmp(n, "In R")) ptr = in[1];
                else if (!strcmp(n, "Out L")) ptr = out[0];
                else if (!strcmp(n, "Out R")) ptr = out[1];
                else if (!strcmp(n, "Threshold (dBFS)")) ptr = &threshold;
                else if (!strcmp(n, "Release (ms)")) ptr = &release;
                else if (!strcmp(n, "Lookahead (ms)")) ptr = &lookahead;
                else if (!strcmp(n, "latency")) ptr = &latency;
                if (ptr) d->connect_port(h, i, ptr);
            }
            if (d->activate) d->activate(h);
            return true;
        }
        void block(long long t, double lAmp, double rAmp) {
            const double w = 2.0 * M_PI * 1000.0 / kRate;
            for (unsigned s = 0; s < kBlock; ++s) {
                const long long i = t + s;
                in[0][s] = (float)(lAmp * sin(w * i));
                in[1][s] = (float)(rAmp * sin(w * i));
            }
            d->run(h, kBlock);
        }
        void cleanup() { if (d && d->cleanup) d->cleanup(h); h = nullptr; }
    };

    LimRig r;
    if (!r.init(desc)) { printf("  [FAIL] instantiate\n"); ++failures; return; }
    check("limiter latency (samples)", r.latency, 240.0, 1.0);

    // Below threshold: unity gain, only the lookahead delay.
    long long t = 0;
    double inAcc = 0, outAcc = 0;
    for (int b = 0; b < 40; ++b) {
        r.block(t, 0.1, 0.1);
        t += kBlock;
        if (b >= 20) {
            for (unsigned s = 0; s < kBlock; ++s) {
                const long long i = t - kBlock + s - 240;
                const double expected = 0.1 * sin(2.0 * M_PI * 1000.0 * i / kRate);
                inAcc += expected * expected;
                outAcc += (double)r.out[0][s] * r.out[0][s];
            }
        }
    }
    check("limiter unity gain below threshold", sqrt(outAcc / inAcc), 1.0, 0.01);

    // Above threshold: stereo-linked gain reduction, peak stays below the limit.
    r.cleanup();
    LimRig h;
    if (!h.init(desc)) { printf("  [FAIL] instantiate (hot)\n"); ++failures; return; }
    t = 0;
    double peak = 0.0, rPeak = 0.0, rInput = 0.0;
    for (int b = 0; b < 80; ++b) {
        h.block(t, 1.0, 0.1);
        t += kBlock;
        if (b >= 40) {
            for (unsigned s = 0; s < kBlock; ++s) {
                peak = std::max(peak, (double)std::fabs(h.out[0][s]));
                rPeak = std::max(rPeak, (double)std::fabs(h.out[1][s]));
                rInput = std::max(rInput, (double)std::fabs(h.in[1][s]));
            }
        }
    }
    check("limiter caps the hot channel", peak, 0.8414, 0.03);
    check("limiter links the quiet channel", rPeak, 0.0841, 0.015);
    printf("  quiet channel input peak %.4f, linked output peak %.4f\n", rInput, rPeak);
    h.cleanup();
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/ladspa/freesurround_ladspa.so";
    void *so = dlopen(path, RTLD_NOW);
    if (!so) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto descFunc = (const LADSPA_Descriptor *(*)(unsigned long))dlsym(so, "ladspa_descriptor");
    if (!descFunc) {
        printf("no ladspa_descriptor\n");
        return 1;
    }
    const LADSPA_Descriptor *stereo = descFunc(0);
    const LADSPA_Descriptor *autoDesc = descFunc(1);
    const LADSPA_Descriptor *limiterDesc = descFunc(2);
    if (!stereo || !autoDesc || !limiterDesc) {
        printf("missing descriptors\n");
        return 1;
    }

    printf("plugin: %s (%s) ports=%lu\n", stereo->Name, stereo->Label, stereo->PortCount);
    for (unsigned long i = 0; i < stereo->PortCount; ++i)
        printf("  port %2lu: %-16s desc=%08x\n", i, stereo->PortNames[i], stereo->PortDescriptors[i]);

    stereoSmoke(stereo);
    autoTransitions(autoDesc);
    autoSteadySteps(autoDesc);
    autoLfeSend(autoDesc);
    autoNativeLfe(autoDesc);
    limiterTest(limiterDesc);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    dlclose(so);
    return failures ? 1 : 0;
}
