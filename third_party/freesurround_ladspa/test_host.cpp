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
    }

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
    // ... add FC: the detector switches to bypass after 2 blocks.
    r.block(t, 0x7);
    t += kBlock;
    r.block(t, 0x7);
    t += kBlock;
    double bypassMin = 1e9, bypassSteady = 0;
    for (int b = 2; b < 20; ++b) {
        r.block(t, 0x7);
        t += kBlock;
        const double total = r.totalRms();
        bypassMin = std::min(bypassMin, total);
        bypassSteady = total;
    }
    check("upmix->bypass keeps audio (min block RMS)", bypassMin, bypassSteady, bypassSteady * 0.5);

    // Drop FC: the detector switches back after the 48-block hysteresis.
    double switchMin = 1e9;
    for (int b = 0; b < 80; ++b) {
        r.block(t, 0x3);
        t += kBlock;
        if (b >= 46 && b <= 50)
            switchMin = std::min(switchMin, r.totalRms());
    }
    check("bypass->upmix keeps audio (min block RMS)", switchMin, upmixTotal, upmixTotal * 0.5);
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
    // 6144-sample delay, then compare the steady last blocks against the
    // generated input (the block RMS of a 40 Hz tone is not stable on its own).
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
    if (!stereo || !autoDesc) {
        printf("missing descriptors\n");
        return 1;
    }

    printf("plugin: %s (%s) ports=%lu\n", stereo->Name, stereo->Label, stereo->PortCount);
    for (unsigned long i = 0; i < stereo->PortCount; ++i)
        printf("  port %2lu: %-16s desc=%08x\n", i, stereo->PortNames[i], stereo->PortDescriptors[i]);

    stereoSmoke(stereo);
    autoTransitions(autoDesc);
    autoLfeSend(autoDesc);
    autoNativeLfe(autoDesc);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    dlclose(so);
    return failures ? 1 : 0;
}
