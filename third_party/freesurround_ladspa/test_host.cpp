/* Quick host-side test for the FreeSurround LADSPA plugin (no PipeWire involved). */
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include "ladspa.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/ladspa/freesurround_ladspa.so";
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto descFunc = (const LADSPA_Descriptor *(*)(unsigned long))dlsym(h, "ladspa_descriptor");
    if (!descFunc) { printf("no ladspa_descriptor\n"); return 1; }
    const LADSPA_Descriptor *d = descFunc(0);
    if (!d) { printf("no descriptor\n"); return 1; }
    printf("plugin: %s (%s) ports=%lu\n", d->Name, d->Label, d->PortCount);
    for (unsigned long i = 0; i < d->PortCount; ++i)
        printf("  port %2lu: %-16s desc=%08x\n", i, d->PortNames[i], d->PortDescriptors[i]);

    LADSPA_Handle inst = d->instantiate(d, 48000);
    if (!inst) { printf("instantiate failed\n"); return 1; }

    const unsigned long N = 512;
    std::vector<float> inL(N), inR(N);
    std::vector<float> out[8];
    for (auto &o : out) o.assign(N, 0.0f);

    float centerImage = 0.7f, shift = 0.0f, depth = 1.0f, wrap = 90.0f, focus = 0.0f;
    float fsep = 1.0f, rsep = 1.0f, bassLo = 80.0f, bassHi = 111.0f, useLfe = 1.0f, latency = 0.0f;

    for (unsigned long i = 0; i < d->PortCount; ++i) {
        const char *n = d->PortNames[i];
        LADSPA_Data *ptr = nullptr;
        if (!strcmp(n, "In L")) ptr = inL.data();
        else if (!strcmp(n, "In R")) ptr = inR.data();
        else if (!strcmp(n, "Out FL")) ptr = out[0].data();
        else if (!strcmp(n, "Out FR")) ptr = out[1].data();
        else if (!strcmp(n, "Out FC")) ptr = out[2].data();
        else if (!strcmp(n, "Out LFE")) ptr = out[3].data();
        else if (!strcmp(n, "Out RL")) ptr = out[4].data();
        else if (!strcmp(n, "Out RR")) ptr = out[5].data();
        else if (!strcmp(n, "Out SL")) ptr = out[6].data();
        else if (!strcmp(n, "Out SR")) ptr = out[7].data();
        else if (!strcmp(n, "center_image")) ptr = &centerImage;
        else if (!strcmp(n, "shift")) ptr = &shift;
        else if (!strcmp(n, "depth")) ptr = &depth;
        else if (!strcmp(n, "circular_wrap")) ptr = &wrap;
        else if (!strcmp(n, "focus")) ptr = &focus;
        else if (!strcmp(n, "front_sep")) ptr = &fsep;
        else if (!strcmp(n, "rear_sep")) ptr = &rsep;
        else if (!strcmp(n, "bass_lo")) ptr = &bassLo;
        else if (!strcmp(n, "bass_hi")) ptr = &bassHi;
        else if (!strcmp(n, "use_lfe")) ptr = &useLfe;
        else if (!strcmp(n, "latency")) ptr = &latency;
        if (ptr) d->connect_port(inst, i, ptr);
    }
    if (d->activate) d->activate(inst);

    const char *names[8] = {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};

    for (double freq : {440.0, 60.0}) {
        // reset state and run some warmup blocks + measurement block
        if (d->deactivate) d->deactivate(inst);
        if (d->activate) d->activate(inst);
        double phase = 0.0;
        double peak[8] = {0};
        long total = 0;
        const long blocks = 40; // 40 * 512 = 20480 samples > 4 * 4096 warmup
        for (long b = 0; b < blocks; ++b) {
            for (unsigned long s = 0; s < N; ++s) {
                double v = 0.5 * sin(phase);
                phase += 2.0 * M_PI * freq / 48000.0;
                inL[s] = (float)v;
                inR[s] = (float)v; // L=R in-phase
            }
            d->run(inst, N);
            if (b >= blocks - 8) { // measure last blocks
                for (int c = 0; c < 8; ++c)
                    for (unsigned long s = 0; s < N; ++s)
                        peak[c] = fmax(peak[c], fabs(out[c][s]));
                total += N;
            }
        }
        printf("\ninput %.0f Hz, L=R, amplitude 0.5 -> output peaks:\n  ", freq);
        for (int c = 0; c < 8; ++c) printf("%s=%.4f  ", names[c], peak[c]);
        printf("\n");
    }

    d->cleanup(inst);
    dlclose(h);
    return 0;
}