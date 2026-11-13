#include "ladspa.h"
#include "dsp_config.h"
#include <complex>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace {
constexpr unsigned rate = 48000;
constexpr unsigned latency = VsmDsp::upmix_latency;
int failures = 0;
void check(const char *name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Host {
    const LADSPA_Descriptor *d;
    LADSPA_Handle h;
    std::vector<std::vector<float>> ports;
    Host(const LADSPA_Descriptor *descriptor, unsigned capacity) : d(descriptor), h(d->instantiate(d, rate)), ports(d->PortCount) {
        for (unsigned p = 0; p < d->PortCount; ++p) {
            ports[p].assign(d->PortDescriptors[p] & LADSPA_PORT_AUDIO ? capacity : 1, 0.0f);
            d->connect_port(h, p, ports[p].data());
        }
        set("center_image", 0.7f); set("depth", 1); set("circular_wrap", 90);
        set("front_sep", 1); set("rear_sep", 1); set("bass_lo", 70); set("bass_hi", 90);
        set("upmix_enable", 1); set("Threshold (dBFS)", -1.5f); set("Release (ms)", 100); set("Lookahead (ms)", 5);
        if (d->activate) d->activate(h);
    }
    ~Host() { d->cleanup(h); }
    unsigned index(const char *name) const {
        for (unsigned p = 0; p < d->PortCount; ++p) if (!std::strcmp(name, d->PortNames[p])) return p;
        return d->PortCount;
    }
    void set(const char *name, float value) { const unsigned p = index(name); if (p < ports.size()) ports[p][0] = value; }
    void run(unsigned count) { d->run(h, count); }
};
std::vector<float> render(const LADSPA_Descriptor *descriptor, const std::vector<unsigned> &blocks) {
    Host h(descriptor, *std::max_element(blocks.begin(), blocks.end()));
    std::vector<float> result;
    for (unsigned position = 0, block = 0; position < 32768; ++block) {
        const unsigned n = std::min(blocks[block % blocks.size()], 32768 - position);
        for (unsigned s = 0; s < n; ++s) h.ports[0][s] = h.ports[1][s] = position + s == 8192 ? 0.25f : 0.0f;
        h.run(n);
        result.insert(result.end(), h.ports[10].begin(), h.ports[10].begin() + n);
        position += n;
    }
    return result;
}

// Independent reconstructed-peak estimate: 32 phases with a 128-tap
// Blackman-windowed sinc. The limiter detector itself is not used here.
double sincPeak(const std::vector<float> &signal, size_t begin, size_t end) {
    constexpr int phases = 32, taps = 128, half = taps / 2;
    double peak = 0.0;
    for (size_t n = begin; n < end; ++n) {
        for (int phase = 0; phase < phases; ++phase) {
            const double t = double(n) + double(phase) / phases;
            double value = 0.0;
            for (int k = -half; k < half; ++k) {
                const long index = static_cast<long>(std::floor(t)) + k;
                if (index < 0 || index >= static_cast<long>(signal.size()))
                    continue;
                const double x = t - index;
                if (x <= -1.0 || x >= 1.0)
                    continue;
                const double sinc = x == 0.0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
                const double window = 0.42 + 0.5 * std::cos(M_PI * x / half) +
                                      0.08 * std::cos(2 * M_PI * x / half);
                value += signal[index] * sinc * window;
            }
            peak = std::max(peak, std::abs(value));
        }
    }
    return peak;
}
void blockTests(const LADSPA_Descriptor *descriptor) {
    const auto reference = render(descriptor, {1});
    const auto peak = std::max_element(reference.begin(), reference.end());
    check("sample latency equals advertised", peak - reference.begin() == 8192 + latency);
    for (const unsigned size : {256u, 512u, 1000u, 1024u, 2048u, 4096u, 8192u, 16385u, 32768u}) {
        const auto output = render(descriptor, {size});
        check("fixed block partition preserves every sample", output == reference);
    }
    check("variable block partition preserves every sample", render(descriptor, {1, 997, 4096, 7, 8192, 256}) == reference);
    for (unsigned channel = 2; channel < 8; ++channel) {
        Host h(descriptor, 512);
        float peak = 0.0f;
        for (unsigned pos = 0; pos < 32768; pos += 512) {
            for (unsigned s = 0; s < 512; ++s) h.ports[channel][s] = pos + s >= 8192 && pos + s < 10240 ? 0.25f : 0.0f;
            h.run(512);
            for (float x : h.ports[channel + 8]) peak = std::max(peak, std::abs(x));
        }
        check("short native event is preserved", std::abs(peak - (channel == 3 ? 0.25f * VsmDsp::native_lfe_gain : 0.25f)) < 1e-6f);
    }
}
void limiterTests(const LADSPA_Descriptor *descriptor, void *library) {
    const float ceiling = std::pow(10.0f, -1.5f / 20.0f);
    auto *currentBits = reinterpret_cast<std::atomic<std::uint32_t> *>(dlsym(library, "vsm_limiter_current_gain_bits"));
    auto *minimumBits = reinterpret_cast<std::atomic<std::uint32_t> *>(dlsym(library, "vsm_limiter_min_gain_bits"));
    auto *activeSamples = reinterpret_cast<std::atomic<std::uint64_t> *>(dlsym(library, "vsm_limiter_active_samples"));
    auto *reducedSamples = reinterpret_cast<std::atomic<std::uint64_t> *>(dlsym(library, "vsm_limiter_reduced_samples"));
    auto *latestGeneration = reinterpret_cast<std::atomic<std::uint64_t> *>(dlsym(library, "vsm_limiter_latest_generation"));
    auto *activeGeneration = reinterpret_cast<std::atomic<std::uint64_t> *>(dlsym(library, "vsm_limiter_active_generation"));
    auto *intervalPeakBits = reinterpret_cast<std::atomic<std::uint32_t> *>(dlsym(library, "vsm_limiter_interval_peak_bits"));
    auto *statisticsEnabled = reinterpret_cast<std::atomic<bool> *>(dlsym(library, "vsm_limiter_statistics_enabled"));
    check("limiter statistics exports are available", currentBits && minimumBits && activeSamples && reducedSamples &&
          latestGeneration && activeGeneration && intervalPeakBits && statisticsEnabled);
    for (float amplitude : {1.0f, 10.0f, 100.0f}) {
        Host h(descriptor, 2048);
        h.ports[0][0] = amplitude; h.ports[1][0] = amplitude / 10;
        h.run(2048);
        const float peak = *std::max_element(h.ports[2].begin(), h.ports[2].end());
        check("isolated impulse respects ceiling", peak <= ceiling + 1e-6f && peak > ceiling - 1e-4f);
        check("limiter preserves stereo linkage", std::abs(h.ports[3][240] * 10 - h.ports[2][240]) < 1e-6f);
    }
    Host h(descriptor, 4096);
    std::fill(h.ports[0].begin(), h.ports[0].end(), 0.5f);
    h.run(4096); h.set("Threshold (dBFS)", -20); h.run(4096);
    check("threshold changes take effect while running", h.ports[2].back() <= 0.100001f);
    h.set("Lookahead (ms)", 0); h.run(4096);
    check("minimum lookahead covers reconstruction detector", h.ports[h.index("latency")][0] >= 25.0f);

    if (currentBits && minimumBits && activeSamples && reducedSamples && latestGeneration &&
        activeGeneration && intervalPeakBits && statisticsEnabled) {
        statisticsEnabled->store(false);
        currentBits->store(std::bit_cast<std::uint32_t>(1.0f));
        minimumBits->store(std::bit_cast<std::uint32_t>(1.0f));
        activeSamples->store(0);
        reducedSamples->store(0);
        intervalPeakBits->store(0);
        activeGeneration->store(latestGeneration->load());
        Host stats(descriptor, 1024);
        activeGeneration->store(latestGeneration->load());
        stats.run(1024);
        check("limiter silence is excluded from statistics", activeSamples->load() == 0);
        check("limiter silence has no input peak", intervalPeakBits->load() == 0);
        std::fill(stats.ports[0].begin(), stats.ports[0].end(), 1.0f);
        std::fill(stats.ports[1].begin(), stats.ports[1].end(), 1.0f);
        stats.run(1024);
        check("disabled telemetry does not count active samples", activeSamples->load() == 0);
        check("disabled telemetry does not publish input peaks", intervalPeakBits->load() == 0);
        statisticsEnabled->store(true);
        stats.run(1024);
        check("limiter counts active samples", activeSamples->load() > 0);
        check("limiter counts materially reduced samples", reducedSamples->load() > 0 && reducedSamples->load() <= activeSamples->load());
        check("limiter exports current reduction", std::bit_cast<float>(currentBits->load()) < 1.0f);
        check("limiter exports maximum reduction", std::bit_cast<float>(minimumBits->load()) < 1.0f);
        check("limiter exports reconstructed input peak", std::bit_cast<float>(intervalPeakBits->load()) >= 1.0f);
    }
}
void telemetryGatingTests(const LADSPA_Descriptor *autoDescriptor, void *library) {
    auto *statisticsEnabled = reinterpret_cast<std::atomic<bool> *>(dlsym(library, "vsm_limiter_statistics_enabled"));
    auto *blocks = reinterpret_cast<std::atomic<std::uint64_t> *>(dlsym(library, "vsm_auto_blocks"));
    auto *state = reinterpret_cast<std::atomic<int> *>(dlsym(library, "vsm_auto_upmix_state"));
    check("input detector telemetry exports are available", statisticsEnabled && blocks && state);
    if (!statisticsEnabled || !blocks || !state)
        return;

    Host host(autoDescriptor, 512);
    std::fill(host.ports[0].begin(), host.ports[0].end(), 0.1f);
    std::fill(host.ports[1].begin(), host.ports[1].end(), 0.1f);
    statisticsEnabled->store(false);
    const uint64_t hiddenBlocks = blocks->load();
    const int hiddenState = state->load();
    host.run(512);
    check("hidden UI does not update input heartbeat", blocks->load() == hiddenBlocks);
    check("hidden UI does not publish input state", state->load() == hiddenState);
    statisticsEnabled->store(true);
    host.run(512);
    check("visible UI updates input heartbeat", blocks->load() == hiddenBlocks + 1);
    check("visible UI publishes input state", state->load() != -1);
    statisticsEnabled->store(false);
}

void qualityTests(const LADSPA_Descriptor *autoDesc, const LADSPA_Descriptor *limiterDesc) {
    // Upstream FreeSurround stores center_image but never reads it in the
    // decode; both values must produce identical output.
    Host c0(autoDesc,512), c1(autoDesc,512);
    c0.set("center_image",0); c1.set("center_image",1);
    double maxDiff=0;
    for(int b=0;b<100;++b) {
        for(int s=0;s<512;++s) c0.ports[0][s]=c0.ports[1][s]=c1.ports[0][s]=c1.ports[1][s]=.2f*std::sin(2*M_PI*1000*(b*512+s)/rate);
        c0.run(512);c1.run(512);
        for(int ch=8;ch<16;++ch) for(int s=0;s<512;++s)
            maxDiff=std::max(maxDiff,(double)std::abs(c0.ports[ch][s]-c1.ports[ch][s]));
    }
    check("center_image remains the upstream no-op",maxDiff==0.0);
    Host lim(limiterDesc,4096);
    double maximum=0;
    for(int b=0;b<30;++b) {
        for(int s=0;s<4096;++s) lim.ports[0][s]=lim.ports[1][s]=.84f*std::cos(M_PI/2*(b*4096+s)+M_PI/4)/std::sqrt(.5);
        lim.run(4096);
        if(b>15) {
            double samplePeak=0;for(auto x:lim.ports[2])samplePeak=std::max(samplePeak,double(std::abs(x)));
            maximum=std::max(maximum,samplePeak*std::sqrt(2.));
        }
    }
    std::printf("reconstructed 12k sine peak %.6f\n",maximum);
    check("intersample sine reconstruction limited",maximum<.85);

    // Independent reconstruction check on hostile full-scale content. The
    // audio path is not resampled; only this test's detector is oversampled.
    const double ceiling = std::pow(10.0, -1.5 / 20.0);
    const struct {
        const char *name;
        double (*sample)(int);
    } cases[] = {
        {"12 kHz phase-shifted tone reconstruction limited",
         [](int n) { return 1.2 * std::sin(2 * M_PI * 12000.0 * n / rate + M_PI / 4); }},
        {"23 kHz near-Nyquist tone reconstruction limited",
         [](int n) { return 1.2 * std::sin(2 * M_PI * 23000.0 * n / rate); }},
        {"200 Hz square reconstruction limited",
         [](int n) { return std::sin(2 * M_PI * 200.0 * n / rate) >= 0 ? 1.2 : -1.2; }},
    };
    for (const auto &test : cases) {
        Host h(limiterDesc, 4096);
        std::vector<float> output(4096);
        for (int block = 0; block < 2; ++block) {
            for (int sample = 0; sample < 4096; ++sample)
                h.ports[0][sample] = h.ports[1][sample] =
                    static_cast<float>(test.sample(block * 4096 + sample));
            h.run(4096);
            if (block == 1)
                std::copy(h.ports[2].begin(), h.ports[2].end(), output.begin());
        }
        const double reconstructed = sincPeak(output, 400, output.size());
        const double samplePeak = *std::max_element(output.begin(), output.end(),
            [](float a, float b) { return std::abs(a) < std::abs(b); });
        std::printf("%s: sample %.4f reconstructed %.4f\n", test.name,
                    std::abs(samplePeak), reconstructed);
        check(test.name, reconstructed <= ceiling + 0.02 && std::abs(samplePeak) <= ceiling + 1e-3);
    }
}
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto descriptor = reinterpret_cast<LADSPA_Descriptor_Function>(dlsym(library, "ladspa_descriptor"));
    if (!descriptor || !descriptor(0) || !descriptor(1) || !descriptor(2)) return 2;
    {
        Host stereo(descriptor(0), 512);
        check("stereo latency control is connected", stereo.ports[stereo.index("latency")][0] == latency);
    }
    blockTests(descriptor(1));
    limiterTests(descriptor(2), library);
    telemetryGatingTests(descriptor(1), library);
    qualityTests(descriptor(1),descriptor(2));
    dlclose(library);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
