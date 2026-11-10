#include "pipewire_manager.h"
#include "hrir_wav.h"
#include "acoustics.h"
#include "../third_party/freesurround_ladspa/dsp_config.h"

#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "../third_party/freesurround_ladspa/ladspa.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <QList>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cstdlib>
#include <chrono>
#include <complex>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <numbers>
#include <numeric>
#include <string_view>
#include <thread>
#include <time.h>

using LfeLowpass = PipeWireManager::LfeLowpass;
using LfeTimbre = PipeWireManager::LfeTimbre;

//
// Escapes a path for embedding into a JSON string value of the module
// arguments, so quotes or backslashes in a file name cannot break the config.
//
static string json_escape(const string &value) {
    const QByteArray json = QJsonDocument(QJsonArray{QString::fromStdString(value)}).toJson(QJsonDocument::Compact);
    return json.sliced(2, json.size() - 4).toStdString();
}

//
// Match the delta's magnitude to a defined digital reference: the arithmetic
// mean of the center pair at 40/50/60/70/80 Hz. This is a project convention,
// not an acoustic SPL calibration or a universal binaural LFE gain standard.
// Delta is flat before the path's low-pass/all-pass; headphones still color it.
//
static double lfe_reference_gain(const string &hrir_path) {
    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(hrir_path), wav))
        return -1.0;
    const auto frames = wav.frames;
    const auto rate = wav.rate;

    // Center HRIR pair (HeSuVi LFE uses these) over a 40-80 Hz grid. A plain DFT
    // is enough: at most ~48000 samples * 10 evaluations for a 1 s HRIR.
    static constexpr double kFreqs[] = {40.0, 50.0, 60.0, 70.0, 80.0};
    double magnitude_sum = 0.0;
    int magnitude_count = 0;
    for (const double f : kFreqs) {
        for (const uint32_t channel : {6u, 13u}) {
            double re = 0.0, im = 0.0;
            for (uint32_t n = 0; n < frames; ++n) {
                const double s = wav.sample(n, channel);
                const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(n) / rate;
                re += s * std::cos(phase);
                im -= s * std::sin(phase);
            }
            magnitude_sum += std::sqrt(re * re + im * im);
            ++magnitude_count;
        }
    }
    if (magnitude_count == 0)
        return -1.0;
    // Preserve the reference level of valid files. Clamping to 0.05 used to
    // boost quiet presets relative to their main channels.
    const double gain = magnitude_sum / magnitude_count;
    if (!std::isfinite(gain) || gain <= 0.0)
        return -1.0;
    qInfo("lfe_reference_gain: '%s' center HRIR 40-80 Hz gain %.4f (%.2f dB)",
          hrir_path.c_str(), gain, 20.0 * std::log10(gain));
    return gain;
}

static std::array<std::complex<double>, 14> hrir_response(const HrirWav &wav, double frequency) {
    std::array<std::complex<double>, 14> response{};
    const auto rotation = std::polar(1.0, -2.0 * std::numbers::pi * frequency / wav.rate);
    std::complex<double> phase = 1.0;
    for (uint32_t n = 0; n < wav.frames; ++n) {
        for (uint32_t channel = 0; channel < 14; ++channel)
            response[channel] += wav.sample(n, channel) * phase;
        phase *= rotation;
    }
    return response;
}

struct LfeAlignment {
    bool valid = false;
    bool delay_main = false;
    bool delay_lfe = false;
    double seconds = 0.0;
    double residual_degrees = 0.0;        // linear slope-fit residual
    double average_residual_degrees = 0.0; // legacy name: wrapped reference phase RMS
    double worst_channel_residual_degrees = 0.0;
    bool allpass_valid = false;
    double allpass_freq = 0.0;
    double allpass_q = 0.0;
    double allpass_residual_degrees = 0.0;
    double allpass_worst_channel_degrees = 0.0;
    // Sample rate of the HRIR file itself. The graph always runs at 48 kHz; a
    // non-48 kHz file is resampled at load time by the convolver, so the phase
    // is measured on the file's time axis while the LR4/all-pass are designed
    // for the graph rate.
    uint32_t source_rate = 0;

    // Selected LFE path configuration (manager defaults: BW4/diffuse/per-ear).
    LfeLowpass lowpass = LfeLowpass::Butterworth4;
    LfeTimbre timbre = LfeTimbre::Diffuse;
    bool per_ear = true;
    double main_delay_seconds = 0.0;
    double lfe_delay_l_seconds = 0.0;
    double lfe_delay_r_seconds = 0.0;
    double average_residual_l_degrees = 0.0;
    double average_residual_r_degrees = 0.0;
    double worst_channel_residual_l_degrees = 0.0;
    double worst_channel_residual_r_degrees = 0.0;
    bool allpass_l_valid = false;
    double allpass_l_freq = 0.0;
    double allpass_l_q = 0.0;
    double allpass_l_residual_degrees = 0.0;
    double allpass_l_worst_channel_degrees = 0.0;
    bool allpass_r_valid = false;
    double allpass_r_freq = 0.0;
    double allpass_r_q = 0.0;
    double allpass_r_residual_degrees = 0.0;
    double allpass_r_worst_channel_degrees = 0.0;
};

// Phase error the send has to survive. The post-alignment absolute residual is
// the right metric: a clean delay fit (small slope residual) can still leave a
// large absolute offset when no delay was inserted. The -6 dB send is only 6 dB
// below the main path, so a large residual can still produce audible
// frequency-dependent cancellation; 30 deg is the chosen bound for any send.
// 15 deg is required for the 0 dB impact send. Measured bad presets:
// AtmosRenderer-Far 249 deg, dtshx 143 deg; aligned presets are 2-8 deg.
static double lfe_effective_residual_degrees(const LfeAlignment &alignment) {
    if (alignment.per_ear) {
        const double left = alignment.allpass_l_valid ? alignment.allpass_l_residual_degrees
                                                       : alignment.average_residual_l_degrees;
        const double right = alignment.allpass_r_valid ? alignment.allpass_r_residual_degrees
                                                        : alignment.average_residual_r_degrees;
        return std::max(left, right);
    }
    return alignment.allpass_valid ? alignment.allpass_residual_degrees : alignment.average_residual_degrees;
}

static bool lfe_send_safe(const LfeAlignment &alignment) {
    return alignment.valid && lfe_effective_residual_degrees(alignment) <= 30.0;
}

static bool lfe_impact_allowed(const LfeAlignment &alignment) {
    return alignment.valid && lfe_effective_residual_degrees(alignment) <= 15.0;
}

struct LfeLowpassSpec {
    int sections = 2;
    std::array<double, 4> q{0.7071067811865476, 0.7071067811865476, 0.0, 0.0};
};

// A 4th-order Butterworth low-pass is two biquads with the standard Q pair
// (0.5412, 1.3066); two Q=0.7071 sections would be a Linkwitz-Riley (and -6 dB
// at the corner instead of -3 dB).

static LfeLowpassSpec lfe_lowpass_spec(LfeLowpass lowpass) {
    switch (lowpass) {
    case LfeLowpass::Butterworth4:
        return {2, {0.5411961001461970, 1.3065629648763770, 0.0, 0.0}};
    case LfeLowpass::Butterworth8:
        return {4, {0.5097955791041592, 0.6013448869350453, 0.8999762231364156, 2.5629154477415055}};
    case LfeLowpass::Lr4:
    default:
        return {2, {0.7071067811865476, 0.7071067811865476, 0.0, 0.0}};
    }
}

static std::complex<double> bq_lowpass_response(double f, double fc, double q, uint32_t rate) {
    const double w0 = 2.0 * std::numbers::pi * fc / static_cast<double>(rate);
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double b0 = (1.0 - cosw0) / 2.0;
    const double b1 = 1.0 - cosw0;
    const double b2 = b0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;
    const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * std::numbers::pi * f / rate));
    return (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
}

static std::complex<double> lfe_lowpass_response(LfeLowpass lowpass, double f, uint32_t rate) {
    const LfeLowpassSpec spec = lfe_lowpass_spec(lowpass);
    std::complex<double> result = 1.0;
    for (int s = 0; s < spec.sections; ++s)
        result *= bq_lowpass_response(f, 120.0, spec.q[s], rate);
    return result;
}

// Shared frequency grid for the LFE/main alignment.
constexpr double kLfeFitFreqs[] = {20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 110.0, 120.0};
constexpr size_t kLfeFitCount = std::size(kLfeFitFreqs);

//
// The LFE path's 120 Hz low-pass has several ms of group delay at 40-120 Hz,
// while each HRIR has its own low-frequency group delay. Without compensation
// the send/native LFE and the main HRIR bass arrive with a frequency-dependent
// phase difference (measured -37..-171 degrees for the bundled presets), which
// makes a larger LFE send cancel instead of reinforce. Fit the phase difference
// between the low-pass and the summed main HRIR response over 40-120 Hz; delay the
// main path when the LFE path lags, or the LFE path when the HRIR is longer.
// Presets whose phase is not a clean delay (e.g. synthetic AtmosRenderer) get
// no correction rather than a wrong one.
//
static double allpass_phase_radians(double f, double f0, double q, double rate) {
    const double w0 = 2.0 * std::numbers::pi * f0 / rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double b0 = 1.0 - alpha;
    const double b1 = -2.0 * cosw0;
    const double b2 = 1.0 + alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;
    const double zr = std::cos(-2.0 * std::numbers::pi * f / rate);
    const double zi = std::sin(-2.0 * std::numbers::pi * f / rate);
    // z = zr + j zi, evaluate the biquad with two complex multiplies
    const std::complex<double> z(zr, zi);
    const std::complex<double> num = b0 + b1 * z + b2 * z * z;
    const std::complex<double> den = a0 + a1 * z + a2 * z * z;
    return std::arg(num / den);
}

// One ear's (or the common path's) phase fit.
struct PhaseFit {
    bool valid = false;
    double tau = 0.0;              // signed: >0 main delay, <0 LFE delay
    double slope_residual = 0.0;
    double average_residual = 0.0;
    double worst_channel = 0.0;
    bool allpass_valid = false;
    double allpass_freq = 0.0;
    double allpass_q = 0.0;
    double allpass_residual = 0.0;
    double allpass_worst_channel = 0.0;
};

static PhaseFit fit_phase(const double diff_in[kLfeFitCount],
                          const std::complex<double> channel_resp[14][kLfeFitCount],
                          const int *channels, int n_channels,
                          const std::complex<double> lfe_resp[kLfeFitCount],
                          uint32_t graph_rate) {
    PhaseFit result;
    double diff[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        if (!std::isfinite(diff_in[i]))
            return {};
        diff[i] = diff_in[i];
    }
    for (size_t i = 1; i < kLfeFitCount; ++i) {
        while (diff[i] - diff[i - 1] > std::numbers::pi)
            diff[i] -= 2.0 * std::numbers::pi;
        while (diff[i] - diff[i - 1] < -std::numbers::pi)
            diff[i] += 2.0 * std::numbers::pi;
    }

    const double mean_f = std::accumulate(std::begin(kLfeFitFreqs), std::end(kLfeFitFreqs), 0.0) / kLfeFitCount;
    const double mean_d = std::accumulate(std::begin(diff), std::end(diff), 0.0) / kLfeFitCount;
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        num += (kLfeFitFreqs[i] - mean_f) * (diff[i] - mean_d);
        den += (kLfeFitFreqs[i] - mean_f) * (kLfeFitFreqs[i] - mean_f);
    }
    const double slope = num / den;
    double slope_residual_sq = 0.0;
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        const double predicted = mean_d + slope * (kLfeFitFreqs[i] - mean_f);
        slope_residual_sq += std::pow(diff[i] - predicted, 2);
    }
    const double slope_residual = std::sqrt(slope_residual_sq / kLfeFitCount);

    double tau = -slope / (2.0 * std::numbers::pi);
    if (slope_residual > 0.26)
        tau = 0.0;
    else if (std::abs(tau) < 0.0002)
        tau = 0.0;
    else {
        tau = std::clamp(tau, -0.02, 0.02);
        tau = std::round(tau * graph_rate) / graph_rate;
    }

    double average_sq = 0.0;
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        const double r = Acoustics::phase_error(diff[i] + 2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau);
        average_sq += r * r;
    }
    const double average_residual = std::sqrt(average_sq / kLfeFitCount);

    double worst = 0.0;
    for (int k = 0; k < n_channels; ++k) {
        double sum = 0.0;
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            const double phase = Acoustics::relative_phase(lfe_resp[i], channel_resp[channels[k]][i]);
            if (!std::isfinite(phase)) {
                sum = std::numeric_limits<double>::infinity();
                break;
            }
            const double r = Acoustics::phase_error(phase + 2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau);
            sum += r * r;
        }
        worst = std::max(worst, std::sqrt(sum / kLfeFitCount));
    }

    result.valid = true;
    result.tau = tau;
    result.slope_residual = slope_residual;
    result.average_residual = average_residual;
    result.worst_channel = worst;

    if (slope_residual <= 0.26) {
        // Search circular phase error. A causal all-pass can have a positive
        // principal phase after wrapping; that is not a noncausal time advance.
        double target[kLfeFitCount];
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            const double r = Acoustics::phase_error(diff[i] + 2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau);
            target[i] = -r;
        }
        {
            double best_error = 1e9, best_freq = 0.0, best_q = 0.0;
            constexpr int kFreqSteps = 80;
            constexpr int kQSteps = 40;
            const double f_min = 20.0, f_max = 12000.0;
            const double q_min = 0.1, q_max = 10.0;
            for (int fi = 0; fi < kFreqSteps; ++fi) {
                const double f0 = f_min * std::pow(f_max / f_min, static_cast<double>(fi) / (kFreqSteps - 1));
                for (int qi = 0; qi < kQSteps; ++qi) {
                    const double q = q_min * std::pow(q_max / q_min, static_cast<double>(qi) / (kQSteps - 1));
                    double error = 0.0;
                    for (size_t i = 0; i < kLfeFitCount; ++i) {
                        const double err = Acoustics::phase_error(
                            allpass_phase_radians(kLfeFitFreqs[i], f0, q, graph_rate) - target[i]);
                        error += err * err;
                    }
                    error = std::sqrt(error / kLfeFitCount);
                    if (error < best_error) {
                        best_error = error;
                        best_freq = f0;
                        best_q = q;
                    }
                }
            }
            if (best_error < average_residual - 0.0087) {
                result.allpass_valid = true;
                result.allpass_freq = best_freq;
                result.allpass_q = best_q;
                result.allpass_residual = best_error;
                double worst_after = 0.0;
                for (int k = 0; k < n_channels; ++k) {
                    double sum = 0.0;
                    for (size_t i = 0; i < kLfeFitCount; ++i) {
                        const double phase = Acoustics::relative_phase(lfe_resp[i], channel_resp[channels[k]][i]);
                        if (!std::isfinite(phase)) {
                            sum = std::numeric_limits<double>::infinity();
                            break;
                        }
                        const double r = Acoustics::phase_error(phase +
                            2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau +
                            allpass_phase_radians(kLfeFitFreqs[i], best_freq, best_q, graph_rate));
                        sum += r * r;
                    }
                    worst_after = std::max(worst_after, std::sqrt(sum / kLfeFitCount));
                }
                result.allpass_worst_channel = worst_after;
            }
        }
    }
    return result;
}

//
// The LFE path's 120 Hz low-pass has several ms of group delay at low
// frequencies, while each HRIR has its own low-frequency group delay. Without
// compensation the send/native LFE and the main HRIR bass arrive with a
// frequency-dependent phase difference, which makes a larger LFE send cancel
// instead of reinforce. Fit the phase difference between the low-pass and the summed
// main HRIR response over 20-120 Hz; delay the main path when the LFE path lags,
// or the LFE path when the HRIR is longer. The remaining absolute phase error is
// then reduced with an automatically fitted all-pass section in the LFE path.
// Presets whose phase is not a clean delay (e.g. synthetic AtmosRenderer) get
// no correction rather than a wrong one.
//
static const char *lfe_lowpass_name(LfeLowpass lowpass) {
    switch (lowpass) {
    case LfeLowpass::Butterworth4: return "butterworth4";
    case LfeLowpass::Butterworth8: return "butterworth8";
    case LfeLowpass::Lr4:
    default: return "lr4";
    }
}

static const char *lfe_timbre_name(LfeTimbre timbre) {
    switch (timbre) {
    case LfeTimbre::CenterHrir: return "center-hrir";
    case LfeTimbre::Diffuse: return "diffuse";
    case LfeTimbre::Delta:
    default: return "delta";
    }
}

//
// Fit the selected low-pass/timbre with independent left/right LFE delays and
// all-pass sections. The main path keeps one common delay (Dmain), while the
// Response of a real IR at one frequency on its own sample-rate axis. The
// LFE timbre IR may be resampled by the convolver when the file rate differs
// from the 48 kHz graph, which preserves the physical-frequency response.
template <typename Container>
static std::complex<double> ir_response(const Container &ir, double frequency, uint32_t rate) {
    if (rate == 0)
        return 0.0;
    std::complex<double> sum = 0.0;
    std::complex<double> phase = 1.0;
    const std::complex<double> rotation =
        std::polar(1.0, -2.0 * std::numbers::pi * frequency / static_cast<double>(rate));
    for (const double value : ir) {
        sum += value * phase;
        phase *= rotation;
    }
    return sum;
}

static LfeAlignment lfe_alignment_configured(const string &hrir_path, uint32_t graph_rate,
                                               LfeLowpass lowpass, LfeTimbre timbre,
                                               bool per_ear) {
    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(hrir_path), wav) || graph_rate == 0)
        return {};
    const auto source_rate = wav.rate;

    std::complex<double> channel_resp[14][kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        const auto response = hrir_response(wav, kLfeFitFreqs[i]);
        for (uint32_t channel = 0; channel < 14; ++channel)
            channel_resp[channel][i] = response[channel];
    }

    std::complex<double> left[kLfeFitCount], right[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        left[i] = 0.0;
        right[i] = 0.0;
        double magnitude_sum = 0.0;
        for (uint32_t channel = 0; channel < 14; ++channel) {
            (channel % 2 == 0 ? left[i] : right[i]) += channel_resp[channel][i];
            magnitude_sum += std::abs(channel_resp[channel][i]);
        }
        // A relative cancellation tolerance preserves phase under global gain
        // changes; an absolute 1e-9 floor rejected otherwise identical quiet IRs.
        const double tolerance = 64.0 * std::numeric_limits<double>::epsilon() * magnitude_sum;
        if (std::abs(left[i]) <= tolerance || std::abs(right[i]) <= tolerance)
            return {};
    }

    std::complex<double> lpf[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i)
        lpf[i] = lfe_lowpass_response(lowpass, kLfeFitFreqs[i], graph_rate);

    std::complex<double> timbre_l[kLfeFitCount], timbre_r[kLfeFitCount];
    if (timbre == LfeTimbre::CenterHrir) {
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            timbre_l[i] = channel_resp[6][i];
            timbre_r[i] = channel_resp[13][i];
        }
    } else if (timbre == LfeTimbre::Diffuse) {
        // The diffuse-field LFE IR is the average of the seven left/right HRIRs,
        // normalized to the center pair's 40-80 Hz magnitude so the LFE level
        // stays comparable across timbre modes. With the BW4 low-pass the native
        // LFE lands at +6.0 dB at 60 Hz (0.5 dB above the +5.5 dB reference; the
        // old LR4 gave +5.5). The writer below uses the same normalization.
        //
        // This is also the same seven-HRIR average that forms the main sum, so
        // lfe/left below is exactly a real multiple of the low-pass. The HRIR
        // phase cancels: for diffuse the fit is preset-independent and the L/R
        // (per-ear) results are identical.
        const double center_gain = lfe_reference_gain(hrir_path);
        if (center_gain <= 0.0)
            return {};
        double diffuse_gain = 0.0;
        int diffuse_count = 0;
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            if (kLfeFitFreqs[i] < 40.0 || kLfeFitFreqs[i] > 80.0)
                continue;
            diffuse_gain += (std::abs(left[i]) + std::abs(right[i])) / (2.0 * 7.0);
            ++diffuse_count;
        }
        if (diffuse_count == 0 || !(diffuse_gain > 0.0))
            return {};
        diffuse_gain /= diffuse_count;
        const double scale = center_gain / diffuse_gain;
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            timbre_l[i] = (left[i] / 7.0) * scale;
            timbre_r[i] = (right[i] / 7.0) * scale;
        }
    } else {
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            timbre_l[i] = 1.0;
            timbre_r[i] = 1.0;
        }
    }

    std::complex<double> lfe_l[kLfeFitCount], lfe_r[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        lfe_l[i] = lpf[i] * timbre_l[i];
        lfe_r[i] = lpf[i] * timbre_r[i];
    }

    LfeAlignment result;
    result.valid = true;
    result.source_rate = source_rate;
    result.lowpass = lowpass;
    result.timbre = timbre;
    result.per_ear = per_ear;

    int all_channels[14];
    int even_channels[7] = {0, 2, 4, 6, 8, 10, 12};
    int odd_channels[7] = {1, 3, 5, 7, 9, 11, 13};
    for (int c = 0; c < 14; ++c)
        all_channels[c] = c;

    if (!per_ear) {
        std::complex<double> lfe_avg[kLfeFitCount];
        double diff[kLfeFitCount];
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            lfe_avg[i] = (lfe_l[i] + lfe_r[i]) / 2.0;
            const std::complex<double> main_avg = (left[i] + right[i]) / 2.0;
            if (std::abs(main_avg) < 1e-9)
                return {};
            diff[i] = Acoustics::relative_phase(lfe_avg[i], main_avg);
        }
        const PhaseFit fit = fit_phase(diff, channel_resp, all_channels, 14, lfe_avg, graph_rate);
        if (!fit.valid)
            return {};
        result.delay_main = fit.tau > 0.0;
        result.delay_lfe = fit.tau < 0.0;
        result.seconds = std::abs(fit.tau);
        result.residual_degrees = fit.slope_residual * 180.0 / std::numbers::pi;
        result.average_residual_degrees = fit.average_residual * 180.0 / std::numbers::pi;
        result.worst_channel_residual_degrees = fit.worst_channel * 180.0 / std::numbers::pi;
        result.allpass_valid = fit.allpass_valid;
        result.allpass_freq = fit.allpass_freq;
        result.allpass_q = fit.allpass_q;
        result.allpass_residual_degrees = fit.allpass_residual * 180.0 / std::numbers::pi;
        result.allpass_worst_channel_degrees = fit.allpass_worst_channel * 180.0 / std::numbers::pi;
        qInfo("lfe_experiment: '%s' source %u graph %u lowpass %s timbre %s common, fit %.2f ms, "
              "slope-residual %.2f deg, absolute avg %.2f deg, worst %.2f deg%s",
              hrir_path.c_str(), source_rate, graph_rate, lfe_lowpass_name(lowpass), lfe_timbre_name(timbre),
              std::abs(fit.tau) * 1000.0, result.residual_degrees, result.average_residual_degrees,
              result.worst_channel_residual_degrees, fit.allpass_valid ? " (+ all-pass)" : "");
        return result;
    }

    double diff_l[kLfeFitCount], diff_r[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        diff_l[i] = Acoustics::relative_phase(lfe_l[i], left[i]);
        diff_r[i] = Acoustics::relative_phase(lfe_r[i], right[i]);
    }
    const PhaseFit fit_l = fit_phase(diff_l, channel_resp, even_channels, 7, lfe_l, graph_rate);
    const PhaseFit fit_r = fit_phase(diff_r, channel_resp, odd_channels, 7, lfe_r, graph_rate);
    if (!fit_l.valid || !fit_r.valid)
        return {};

    // Realize both ear fits with one common main delay and per-ear LFE delays.
    // Dmain >= max(0, tauL, tauR), Dlfe = Dmain - tau, so both delays are causal.
    const double main_delay = std::max({0.0, fit_l.tau, fit_r.tau});
    const double lfe_delay_l = main_delay - fit_l.tau;
    const double lfe_delay_r = main_delay - fit_r.tau;
    result.main_delay_seconds = main_delay;
    result.lfe_delay_l_seconds = std::max(0.0, lfe_delay_l);
    result.lfe_delay_r_seconds = std::max(0.0, lfe_delay_r);
    result.delay_main = main_delay > 0.0;
    result.delay_lfe = result.lfe_delay_l_seconds > 0.0 || result.lfe_delay_r_seconds > 0.0;
    result.seconds = main_delay > 0.0 ? main_delay
                                      : std::max(result.lfe_delay_l_seconds, result.lfe_delay_r_seconds);
    result.average_residual_l_degrees = fit_l.average_residual * 180.0 / std::numbers::pi;
    result.average_residual_r_degrees = fit_r.average_residual * 180.0 / std::numbers::pi;
    result.worst_channel_residual_l_degrees = fit_l.worst_channel * 180.0 / std::numbers::pi;
    result.worst_channel_residual_r_degrees = fit_r.worst_channel * 180.0 / std::numbers::pi;
    result.residual_degrees = std::max(fit_l.slope_residual, fit_r.slope_residual) * 180.0 / std::numbers::pi;
    result.average_residual_degrees = std::max(result.average_residual_l_degrees,
                                               result.average_residual_r_degrees);
    result.worst_channel_residual_degrees = std::max(result.worst_channel_residual_l_degrees,
                                                     result.worst_channel_residual_r_degrees);
    result.allpass_l_valid = fit_l.allpass_valid;
    result.allpass_l_freq = fit_l.allpass_freq;
    result.allpass_l_q = fit_l.allpass_q;
    result.allpass_l_residual_degrees = fit_l.allpass_residual * 180.0 / std::numbers::pi;
    result.allpass_l_worst_channel_degrees = fit_l.allpass_worst_channel * 180.0 / std::numbers::pi;
    result.allpass_r_valid = fit_r.allpass_valid;
    result.allpass_r_freq = fit_r.allpass_freq;
    result.allpass_r_q = fit_r.allpass_q;
    result.allpass_r_residual_degrees = fit_r.allpass_residual * 180.0 / std::numbers::pi;
    result.allpass_r_worst_channel_degrees = fit_r.allpass_worst_channel * 180.0 / std::numbers::pi;
    qInfo("lfe_experiment: '%s' source %u graph %u lowpass %s timbre %s per-ear, "
          "main %.2f ms, LFE L %.2f / R %.2f ms, residual L %.2f / R %.2f deg",
          hrir_path.c_str(), source_rate, graph_rate, lfe_lowpass_name(lowpass), lfe_timbre_name(timbre),
          main_delay * 1000.0, result.lfe_delay_l_seconds * 1000.0, result.lfe_delay_r_seconds * 1000.0,
          result.average_residual_l_degrees, result.average_residual_r_degrees);
    return result;
}

//
// Writes a "dirac delta" impulse response with the exact format and length of
// the selected HRIR: a single sample at t = 0 followed by silence, scaled to the
// HRIR's low-frequency magnitude (see lfe_reference_gain). Convolving the LFE
// path with this instead of the HRIR leaves the signal uncolored (equal L/R, no
// spatial processing) while keeping the convolver branch identical in IR length
// to the convolved channels it sums with.
//
static bool write_float_ir(const string &path, uint32_t rate, uint16_t channels,
                           const std::vector<float> &samples) {
    QByteArray header;
    auto le16 = [&](uint16_t value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        header.append(bytes, 2);
    };
    auto le32 = [&](uint32_t value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        header.append(bytes, 4);
    };
    const uint32_t size = static_cast<uint32_t>(samples.size() * sizeof(float));
    header.append("RIFF", 4); le32(36 + size); header.append("WAVEfmt ", 8);
    le32(16); le16(3); le16(channels); le32(rate);
    le32(rate * channels * 4); le16(channels * 4); le16(32);
    header.append("data", 4); le32(size);
    QByteArray data(size, Qt::Uninitialized);
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i]))
            return false;
        qToLittleEndian(std::bit_cast<uint32_t>(samples[i]), data.data() + i * 4);
    }
    QSaveFile file(QString::fromStdString(path));
    return file.open(QIODevice::WriteOnly) && file.write(header) == header.size() &&
           file.write(data) == data.size() && file.commit();
}

static bool write_lfe_bypass_delta(const string &hrir_path, const string &delta_path) {
    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(hrir_path), wav))
        return false;
    const double gain = lfe_reference_gain(hrir_path);
    if (!std::isfinite(gain) || gain <= 0.0)
        return false;
    std::vector<float> samples(wav.frames, 0.0f);
    samples[0] = static_cast<float>(gain);
    return write_float_ir(delta_path, wav.rate, 1, samples);
}

// Coherent diffuse-field LFE IR: the per-ear average of the seven left/right
// HRIRs, normalized to the center pair's 40-80 Hz magnitude (the same
// reference as the delta IR). The result is a stereo IR at the source rate;
// the LFE path convolves L with channel 0 and R with channel 1.
// Float intermediates preserve linear FIR coefficients, including values above
// unity. Clipping individual taps changes both the timbre and phase alignment.
static bool write_lfe_diffuse_ir(const string &hrir_path, const string &diffuse_path) {
    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(hrir_path), wav))
        return false;
    if (wav.channels != 14 || wav.frames == 0 || wav.rate == 0)
        return false;
    std::vector<double> left(wav.frames), right(wav.frames);
    for (uint32_t n = 0; n < wav.frames; ++n) {
        for (uint32_t channel = 0; channel < 14; ++channel) {
            const double s = wav.sample(n, channel);
            (channel % 2 == 0 ? left : right)[n] += s;
        }
        left[n] /= 7.0;
        right[n] /= 7.0;
    }
    // Keep the historical native-LFE calibration: the timbre IR's 40-80 Hz
    // average magnitude (both ears) equals the center-HRIR reference gain.
    // Both ears share one real scale, so their old L/R difference is preserved.
    const double reference = lfe_reference_gain(hrir_path);
    if (!(reference > 0.0))
        return false;
    double magnitude = 0.0;
    int count = 0;
    for (const double f : {40.0, 50.0, 60.0, 70.0, 80.0}) {
        magnitude += (std::abs(ir_response(left, f, wav.rate)) +
                      std::abs(ir_response(right, f, wav.rate))) / 2.0;
        ++count;
    }
    if (count == 0 || !(magnitude > 0.0))
        return false;
    const double scale = reference * count / magnitude;
    std::vector<float> interleaved(wav.frames * 2);
    for (uint32_t n = 0; n < wav.frames; ++n) {
        interleaved[2 * n] = static_cast<float>(left[n] * scale);
        interleaved[2 * n + 1] = static_cast<float>(right[n] * scale);
    }
    return write_float_ir(diffuse_path, wav.rate, 2, interleaved);
}

QString PipeWireManager::find_ladspa_plugin() {
    QStringList candidates;

#ifdef IS_APPIMAGE
    // The AppImage is mounted at a random temporary path, resolved via $APPDIR
    const QString appdir = qEnvironmentVariable("APPDIR");
    if (!appdir.isEmpty()) {
        candidates << appdir + QStringLiteral("/usr/lib/ladspa/freesurround_ladspa.so")
                   << appdir + QStringLiteral("/usr/lib64/ladspa/freesurround_ladspa.so");
    }
#endif
#ifndef IS_APPIMAGE
    // Where this application installed the plugin to (absolute path set at build time)
    candidates << QStringLiteral(VSM_LADSPA_PLUGIN_PATH);
#endif

    // Distro or manually installed copies in the usual LADSPA directories
    candidates << QStringLiteral("/usr/lib64/ladspa/freesurround_ladspa.so")
               << QStringLiteral("/usr/lib/ladspa/freesurround_ladspa.so")
               << QStringLiteral("/usr/local/lib/ladspa/freesurround_ladspa.so");

    candidates.prepend(QCoreApplication::applicationDirPath() + QStringLiteral("/freesurround_ladspa.so"));
    for (const QString &candidate : candidates) {
        if (!QFileInfo(candidate).isFile())
            continue;
        void *handle = dlopen(candidate.toUtf8().constData(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            continue;
        const auto descriptor = reinterpret_cast<LADSPA_Descriptor_Function>(dlsym(handle, "ladspa_descriptor"));
        bool have_auto = false, have_limiter = false;
        if (descriptor) {
            for (unsigned long i = 0; i < 32; ++i) {
                const auto *plugin = descriptor(i);
                if (!plugin)
                    break;
                if (!plugin->Label || !plugin->instantiate || !plugin->connect_port || !plugin->run || !plugin->cleanup)
                    continue;
                have_auto |= strcmp(plugin->Label, "freesurround71_auto") == 0;
                have_limiter |= strcmp(plugin->Label, "output_limiter") == 0;
            }
        }
        dlclose(handle);
        if (have_auto && have_limiter)
            return candidate;
    }
    return {};
}

string PipeWireManager::parse_default_sink_name(const char *value) {
    if (!value)
        return {};
    const auto json = QJsonDocument::fromJson(value);
    if (json.isObject())
        return json.object().value(QStringLiteral("name")).toString().toStdString();
    return value[0] == '{' ? string() : string(value);
}

string PipeWireManager::resolve_sink_name(const string &key) const {
    if (key.empty())
        return {};
    for (auto it = sinks.cbegin(); it != sinks.cend(); ++it)
        if (it.value().node_name == key || it.value().serial == key)
            return it.value().node_name;
    return {};
}

bool PipeWireManager::is_our_sink_name(const string &name) const {
    if (name.empty())
        return false;
    if (name == capture_node_name || name == effects_capture_node_name)
        return true;
    return name.rfind(capture_node_name + ".effects.", 0) == 0;
}

bool PipeWireManager::is_effects_sink_name(const string &name) const {
    return !name.empty() && name.rfind(capture_node_name + ".effects.", 0) == 0;
}

string PipeWireManager::choose_physical_default_sink() const {
    // Prefer the device that was default before a virtual sink took over
    if (!last_physical_default_sink.empty() && !is_our_sink_name(last_physical_default_sink)) {
        for (auto it = sinks.cbegin(); it != sinks.cend(); ++it) {
            if (it.value().node_name == last_physical_default_sink)
                return last_physical_default_sink;
        }
    }

    string best_name;
    int best_rank = -1000000;
    for (auto it = sinks.cbegin(); it != sinks.cend(); ++it) {
        const SinkInfo &info = it.value();
        if (is_our_sink_name(info.node_name))
            continue;
        // Hardware outputs beat other virtual sinks; priority.session decides
        // among them, matching WirePlumber's own ranking.
        const bool hardware = info.node_name.rfind("alsa_output.", 0) == 0 ||
                              info.node_name.rfind("bluez_output.", 0) == 0;
        const int rank = info.priority + (hardware ? 100000 : 0);
        if (best_name.empty() || rank > best_rank) {
            best_name = info.node_name;
            best_rank = rank;
        }
    }
    return best_name;
}

bool PipeWireManager::physical_default_is_internal() const {
    string name = last_physical_default_sink;
    if (name.empty() || is_our_sink_name(name))
        name = default_sink_name;
    if (name.empty() || is_our_sink_name(name))
        name = choose_physical_default_sink();
    if (name.empty() || is_our_sink_name(name))
        return false;

    for (auto it = sinks.cbegin(); it != sinks.cend(); ++it) {
        if (it.value().node_name == name)
            return it.value().form_factor == "internal";
    }
    return false;
}

bool PipeWireManager::restore_physical_default() {
    if (!metadata)
        return false;
    const string restore = choose_physical_default_sink();
    if (restore.empty() || is_our_sink_name(restore))
        return false;

    // Set the state before writing so the echo of our own writes is not mistaken
    // for another virtual default selection
    default_sink_name = restore;
    if (!is_our_sink_name(restore))
        last_physical_default_sink = restore;

    const string payload = "{\"name\":\"" + json_escape(restore) + "\"}";
    // default.configured is WirePlumber's persisted user preference; updating it
    // keeps the virtual sink out of the default history, so the next startup or
    // "revert default" cannot select it again.
    pw_metadata_set_property(metadata, 0, "default.configured.audio.sink", "Spa:String:JSON", payload.c_str());
    pw_metadata_set_property(metadata, 0, "default.audio.sink", "Spa:String:JSON", payload.c_str());
    qInfo("restore_physical_default: virtual sink was selected as default, restored '%s'", restore.c_str());
    update_speaker_bypass();
    return true;
}

void PipeWireManager::update_speaker_bypass() {
    const bool bypass = physical_default_is_internal();
    if (speaker_bypass.exchange(bypass) == bypass)
        return;

    qInfo("update_speaker_bypass: built-in speaker output %s", bypass ? "detected, skipping virtualization" : "gone, resuming virtualization");
    if (bypass)
        release_routed_streams();
    else
        route_default_streams();
}

void PipeWireManager::release_routed_streams() {
    if (!metadata)
        return;
    // Clear the set first: our metadata callback re-routes tracked streams whose
    // target is removed, which would otherwise undo this release.
    const QList<uint32_t> ids = routed_node_ids.values();
    routed_node_ids.clear();
    for (uint32_t id : ids) {
        pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", nullptr);
        qInfo("release_routed_streams: Node %u left on the physical output", id);
    }
    update_playing_state();
}

void PipeWireManager::write_controls_locked(std::initializer_list<std::pair<const char *, float>> values) {
    if (!effects_playback_control_proxy || !is_valid()) return;
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame object, params;
    spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&builder, &params);
    for (const auto &[name, value] : values) {
        spa_pod_builder_string(&builder, name);
        spa_pod_builder_float(&builder, value);
    }
    spa_pod_builder_pop(&builder, &params);
    auto *pod = static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &object));
    pw_node_set_param(reinterpret_cast<pw_node *>(effects_playback_control_proxy), SPA_PARAM_Props, 0, pod);
}

void PipeWireManager::write_output_gate_locked(float gain) {
    write_controls_locked({{"gateL:Mult", gain}, {"gateR:Mult", gain}});
}

bool PipeWireManager::update_processing(bool upmix_enabled, LfeMode mode) {
    if (!thread_loop || !is_valid()) return false;
    pw_thread_loop_lock(thread_loop);
    if (!effects_playback_control_proxy) {
        pw_thread_loop_unlock(thread_loop);
        return false;
    }
    // The gated send levels were fixed at graph build time by the HRIR's
    // post-alignment phase residual; a runtime mode switch may only choose
    // between them, never exceed them.
    const float lfe_send = upmix_enabled && mode != LfeMode::Reference
        ? static_cast<float>(mode == LfeMode::Impact ? lfe_send_impact_ : lfe_send_balanced_)
        : 0.0f;
    // The mono main-sum send lives in the auto plugin, so only the upmix and
    // send controls change at runtime.
    write_controls_locked({{"fs:upmix_enable", upmix_enabled ? 1.0f : 0.0f},
                           {"fs:lfe_send", lfe_send}});
    graph_upmix_enabled.store(upmix_enabled, std::memory_order_relaxed);
    lfe_mode = mode;
    const bool ok = sync_with_core(1);
    pw_thread_loop_unlock(thread_loop);
    return ok;
}

void PipeWireManager::clear_effects_control_locked() {
    ++output_gate_generation;
    write_output_gate_locked(1.0f);
    if (effects_playback_control_proxy)
        pw_proxy_destroy(effects_playback_control_proxy);
    effects_playback_control_proxy = nullptr;
    effects_playback_control_id = SPA_ID_INVALID;
}

void PipeWireManager::bind_effects_control_locked() {
    if (effects_playback_control_id == effects_playback_id)
        return;
    clear_effects_control_locked();
    if (registry && effects_playback_id != SPA_ID_INVALID) {
        effects_playback_control_proxy = static_cast<pw_proxy *>(
            pw_registry_bind(registry, effects_playback_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (effects_playback_control_proxy)
            effects_playback_control_id = effects_playback_id;
    }
}

bool PipeWireManager::gate_effects_output_locked() {
    if (!effects_playback_control_proxy || !is_valid())
        return false;
    auto *proxy = effects_playback_control_proxy;
    const int hold_ms = graph_upmix_enabled.load(std::memory_order_relaxed) ? 150 : 30;
    write_output_gate_locked(0.0f);
    const uint64_t generation = ++output_gate_generation;
    qInfo("gate_effects_output: internal gate for %d ms (generation %llu)", hold_ms,
          static_cast<unsigned long long>(generation));
    for (int step = 1; step <= 20; ++step) {
        QTimer::singleShot(hold_ms + step * 5, this, [this, proxy, generation, step]() {
            if (!thread_loop)
                return;
            pw_thread_loop_lock(thread_loop);
            if (generation == output_gate_generation.load(std::memory_order_relaxed) &&
                effects_playback_control_proxy == proxy)
                write_output_gate_locked(static_cast<float>(step) / 20.0f);
            pw_thread_loop_unlock(thread_loop);
        });
    }
    return true;
}

void PipeWireManager::fade_in_new_stream(uint32_t id) {
    if (node_bindings.contains(id))
        gate_effects_output_locked();
    // Never borrow the application's mute control as a fallback: a delayed
    // unmute cannot distinguish our temporary mute from the user's intent.
}

void PipeWireManager::route_default_streams() {
    if (!metadata || routing_stopping || !is_registry_listener_added ||
        speaker_bypass.load(std::memory_order_relaxed))
        return;
    for (auto it = node_bindings.begin(); it != node_bindings.end(); ++it) {
        const uint32_t id = it.key();
        if (routed_node_ids.contains(id))
            continue;
        if (!stream_follows_default(id, it.value()->props_target))
            continue;
        // New stream: start with 30 ms of silence so the physical-device-first
        // block / bridge move cannot pop at the first sample of the track.
        fade_in_new_stream(id);
        pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", capture_node_name.c_str());
        routed_node_ids.insert(id);
        attach_binding_listener(it.value());
        qInfo("route_default_streams: Node %u routed to sink '%s'", id, capture_node_name.c_str());
    }
    update_playing_state();
}

bool PipeWireManager::stream_targets_ours(uint32_t id, const string &props_target) const {
    // The metadata target takes precedence over the node's own props target
    string target = metadata_targets.value(id, string());
    if (target.empty())
        target = props_target;
    if (target.empty())
        return false;

    // Any generation of our virtual sink, including the internal effects sink
    if (target.rfind(capture_node_name_base, 0) == 0)
        return true;
    const string sink = resolve_sink_name(target);
    return !sink.empty() && is_our_sink_name(sink);
}

bool PipeWireManager::stream_follows_default(uint32_t id, const string &props_target) const {
    // The metadata target takes precedence over the node's own props target
    string target = metadata_targets.value(id, string());
    if (target.empty())
        target = props_target;

    if (target.empty())
        return true;

    // Streams that still target our virtual sink (any generation) - e.g. routed by a
    // previous instance of this application, or manually assigned to the internal
    // effects sink - keep being managed instead of being treated as assigned elsewhere.
    if (target.rfind(capture_node_name_base, 0) == 0)
        return true;

    const string sink = resolve_sink_name(target);
    if (sink.empty()) {
        // The target matches no current node. That happens after a restart or a
        // hard kill: WirePlumber restores a stream's old target serial, but the
        // sink (e.g. a previous instance's virtual sink) is gone. The stream is
        // not actually assigned to another device, and leaving it alone would keep
        // it out of the virtual device forever, so treat it as following default.
        return true;
    }
    if (is_our_sink_name(sink))
        return true;
    return sink == default_sink_name;
}

void PipeWireManager::decide_stream(uint32_t id, const string &props_target) {
    if (!metadata || routing_stopping || !is_registry_listener_added || decided_streams.contains(id))
        return;

    decided_streams.insert(id);

    const bool explicitly_ours = stream_targets_ours(id, props_target);
    if (!explicitly_ours && speaker_bypass.load(std::memory_order_relaxed) &&
        stream_follows_default(id, props_target)) {
        qInfo("decide_stream: Node %u left on the built-in speaker output", id);
        return;
    }

    if (explicitly_ours || stream_follows_default(id, props_target)) {
        // This also redirects a stream manually assigned to the internal effects
        // sink to the stable bridge, so a later graph replacement cannot move it.
        fade_in_new_stream(id);
        pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", capture_node_name.c_str());
        routed_node_ids.insert(id);
        qInfo("decide_stream: Node %u routed to sink '%s'", id, capture_node_name.c_str());
    } else {
        qInfo("decide_stream: Node %u left alone (assigned to another device)", id);
    }
}

void PipeWireManager::on_node_info(void *data, const struct pw_node_info *info) {
    NodeBinding *binding = static_cast<NodeBinding *>(data);
    PipeWireManager *manager = binding->manager;

    if (!info)
        return;

    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE)
        binding->state = info->state;
    if (info->props) {
        const char *target = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT);
        if (!target)
            target = spa_dict_lookup(info->props, "target.node");
        binding->props_target = (target && strlen(target) > 0) ? target : "";
    }
    manager->decide_stream(binding->id, binding->props_target);
    // Keep listening on routed streams: their state changes indicate playback.
    // Detaching is only safe for streams that are not managed by us.
    if (manager->decided_streams.contains(binding->id) && !manager->routed_node_ids.contains(binding->id))
        manager->detach_binding_listener(binding);
    manager->update_playing_state();
}

void PipeWireManager::on_sink_info(void *data, const struct pw_node_info *info) {
    SinkBinding *binding = static_cast<SinkBinding *>(data);
    PipeWireManager *manager = binding->manager;
    if (!info || !info->props)
        return;

    const char *form_factor = spa_dict_lookup(info->props, "device.form-factor");
    if (!form_factor)
        form_factor = spa_dict_lookup(info->props, "device.form_factor");
    const char *priority_text = spa_dict_lookup(info->props, "priority.session");
    if (!priority_text)
        priority_text = spa_dict_lookup(info->props, "priority.driver");

    auto it = manager->sinks.find(binding->id);
    if (it != manager->sinks.end()) {
        if (form_factor)
            it.value().form_factor = form_factor;
        if (priority_text)
            it.value().priority = std::atoi(priority_text);
    }

    if (manager->is_our_sink_name(manager->default_sink_name))
        manager->restore_physical_default();
    else
        manager->update_speaker_bypass();
}

int PipeWireManager::on_metadata_property(void *data,
                                          uint32_t subject, // Node ID
                                          const char *key,
                                          [[maybe_unused]] const char *type,
                                          const char *value) { // nullptr if property removed
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    if (!key || manager->routing_stopping || !manager->is_registry_listener_added)
        return 0;

    // Persisted user preference (WirePlumber's default history); use it as the
    // physical fallback when the current default is one of our virtual sinks
    if (strcmp(key, "default.configured.audio.sink") == 0) {
        const string configured = parse_default_sink_name(value);
        if (!configured.empty() && !manager->is_our_sink_name(configured))
            manager->last_physical_default_sink = configured;
        return 0;
    }

    // Track the default sink so we know which streams follow it
    if (strcmp(key, "default.audio.sink") == 0) {
        const string new_default = parse_default_sink_name(value);
        if (new_default.empty())
            return 0;
        if (manager->is_our_sink_name(new_default)) {
            // WirePlumber must never keep one of our virtual sinks as the default.
            // This happens when the user selects it manually or when the default
            // history restores it on startup / after "revert default".
            if (!manager->restore_physical_default())
                manager->default_sink_name = new_default;
            return 0;
        }

        const bool changed = new_default != manager->default_sink_name;
        manager->default_sink_name = new_default;
        manager->last_physical_default_sink = new_default;
        if (changed)
            qDebug("on_metadata_property: Default sink is now '%s'", new_default.c_str());
        manager->update_speaker_bypass();
        return 0;
    }

    if (strcmp(key, PW_KEY_TARGET_OBJECT) != 0 && strcmp(key, "target.node") != 0)
        return 0;
    // Our own bridge/effects nodes are internal; their target changes are not
    // application routing decisions
    if (manager->internal_playback_ids.contains(subject))
        return 0;

    // WirePlumber uses "-1" to mean "no explicit target, follow the default sink"
    const bool no_target = !value || strlen(value) == 0 || strcmp(value, "-1") == 0;

    // A stream explicitly assigned to any generation of our virtual sink (or to
    // the internal effects sink) must stay on the stable bridge. This also covers
    // manual per-application routing through the duplicate internal device.
    bool ours_target = false;
    if (!no_target) {
        ours_target = strncmp(value, manager->capture_node_name_base, strlen(manager->capture_node_name_base)) == 0;
        if (!ours_target) {
            const string sink = manager->resolve_sink_name(value);
            ours_target = !sink.empty() && manager->is_our_sink_name(sink);
        }
    }
    if (ours_target) {
        if (strcmp(value, manager->capture_node_name.c_str()) != 0) {
            pw_metadata_set_property(manager->metadata, subject, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->capture_node_name.c_str());
            qDebug("on_metadata_property: Node %u redirected from '%s' to the stable bridge", subject, value);
        }
        manager->metadata_targets[subject] = manager->capture_node_name;
        manager->routed_node_ids.insert(subject);
        auto binding = manager->node_bindings.find(subject);
        if (binding != manager->node_bindings.end())
            manager->attach_binding_listener(binding.value());
        manager->update_playing_state();
        return 0;
    }

    // Ignore the echo of our own routing writes
    if (value && strcmp(value, manager->capture_node_name.c_str()) == 0)
        return 0;
    if (no_target)
        manager->metadata_targets.remove(subject);
    else
        manager->metadata_targets[subject] = value;

    if (no_target && manager->routed_node_ids.contains(subject)) {
        // Re-assert routing if a stream we virtualize had its target removed.
        // Clears to "-1" are left alone: WirePlumber uses them to make a stream
        // follow the default sink, and answering them starts a write battle. The
        // bridge sink survives graph swaps, so they no longer occur mid-switch.
        if (value == nullptr && !manager->speaker_bypass.load(std::memory_order_relaxed)) {
            pw_metadata_set_property(manager->metadata, subject, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->capture_node_name.c_str());
            qDebug("on_metadata_property: Re-routed node %u to sink '%s' after target removal", subject, manager->capture_node_name.c_str());
        }
    } else if (!no_target && manager->routed_node_ids.contains(subject) &&
               strcmp(value, manager->capture_node_name.c_str()) != 0) {
        // The stream was explicitly moved to another device, so it is no longer ours
        manager->routed_node_ids.remove(subject);
        auto binding = manager->node_bindings.find(subject);
        if (binding != manager->node_bindings.end())
            manager->detach_binding_listener(binding.value());
        manager->update_playing_state();
        qDebug("on_metadata_property: Node %u released to target '%s'", subject, value);
    }

    return 0;
}

void PipeWireManager::registry_event_global(void *data,
                                            uint32_t id,
                                            [[maybe_unused]] uint32_t permissions,
                                            const char *type,
                                            [[maybe_unused]] uint32_t version,
                                            const struct spa_dict *props) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    // Get the default metadata object
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *metadata_name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (metadata_name && strcmp(metadata_name, "default") == 0) {
            // Check if there already is a metadata object
            if (manager->metadata) {
                spa_hook_remove(&manager->metadata_listener);
                pw_proxy_destroy((pw_proxy *)manager->metadata);
                manager->metadata = nullptr;
            }

            // State of the previous metadata object does not apply to the new one
            manager->metadata_targets.clear();
            manager->metadata_global_id = id;

            // Save the metadata object for reuse later
            manager->metadata = static_cast<pw_metadata *>(
                pw_registry_bind(manager->registry, id, type, PW_VERSION_METADATA, 0));
            if (!manager->metadata) {
                qWarning("registry_event_global: Failed to bind metadata object with ID %u", id);
                manager->metadata_global_id = SPA_ID_INVALID;
                return;
            }
            // Add listener for changes to node metadata
            spa_zero(manager->metadata_listener);
            pw_metadata_add_listener(manager->metadata, &manager->metadata_listener, &manager->metadata_events, manager);

            // Streams that were tracked before the metadata object appeared have not
            // been decided yet, so decide them now that routing is possible
            for (auto it = manager->node_bindings.cbegin(); it != manager->node_bindings.cend(); ++it) {
                manager->decide_stream(it.key(), it.value()->props_target);
                if (manager->decided_streams.contains(it.key()) && !manager->routed_node_ids.contains(it.key()))
                    manager->detach_binding_listener(it.value());
            }

            // Routing we wrote into a previous metadata object was stored there and
            // is gone with it (e.g. after a WirePlumber restart), so re-assert it
            for (const uint32_t node_id : manager->routed_node_ids) {
                pw_metadata_set_property(manager->metadata, node_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->capture_node_name.c_str());
                qDebug("registry_event_global: Re-routed node %u to sink '%s' after metadata change", node_id, manager->capture_node_name.c_str());
            }
            // The bridge's target metadata is just as volatile, but its node property
            // still names the sink of the first generation. Without this the bridge
            // would follow a stale target after a WirePlumber restart.
            if (manager->bridge_playback_id != SPA_ID_INVALID && !manager->effects_capture_node_name.empty()) {
                pw_metadata_set_property(manager->metadata, manager->bridge_playback_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", manager->effects_capture_node_name.c_str());
                qDebug("registry_event_global: Re-linked bridge node %u to effects sink '%s'", manager->bridge_playback_id, manager->effects_capture_node_name.c_str());
            }
        }
    }

    // Track links so a graph swap can wait until the bridge really feeds the new
    // effects sink before the previous sink is removed
    if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char *output = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        const char *input = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        if (output && input) {
            const uint32_t out_id = static_cast<uint32_t>(std::strtoul(output, nullptr, 10));
            const uint32_t in_id = static_cast<uint32_t>(std::strtoul(input, nullptr, 10));
            manager->links.insert(id, LinkInfo{out_id, in_id});
            if (manager->link_wait_active.load(std::memory_order_relaxed))
                pw_thread_loop_signal(manager->thread_loop, false);

            // A stream manually assigned to the internal effects sink (the second
            // identically-looking entry in per-application device lists) must be
            // moved back to the stable bridge: otherwise the next graph rebuild
            // destroys its sink and the stream falls back to the physical output.
            if (manager->node_bindings.contains(out_id) && !manager->internal_playback_ids.contains(out_id)) {
                const bool effects_input = in_id == manager->effects_capture_id ||
                                           (manager->internal_capture_ids.contains(in_id) &&
                                            in_id != manager->bridge_capture_id);
                if (effects_input) {
                    manager->routed_node_ids.insert(out_id);
                    manager->attach_binding_listener(manager->node_bindings.value(out_id));
                    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
                    const qint64 last_ms = manager->effects_redirect_ms.value(out_id, 0);
                    if (now_ms - last_ms > 1500) {
                        manager->effects_redirect_ms[out_id] = now_ms;
                        manager->metadata_targets[out_id] = manager->capture_node_name;
                        if (manager->metadata) {
                            pw_metadata_set_property(manager->metadata, out_id, PW_KEY_TARGET_OBJECT,
                                                     "Spa:String:JSON", manager->capture_node_name.c_str());
                        }
                        qInfo("registry_event_global: Node %u redirected from the internal effects sink to the bridge", out_id);
                    }
                    manager->update_playing_state();
                } else if (in_id == manager->bridge_capture_id) {
                    manager->routed_node_ids.insert(out_id);
                    manager->attach_binding_listener(manager->node_bindings.value(out_id));
                    manager->update_playing_state();
                }
            }
        }
        return;
    }

    // Check if object is a node
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    // Get node name and media class
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *node_media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *node_media_role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);

    if (!node_media_class)
        return;

    // Ignore system notification sounds, they are very short and can cause crackling or even a crash
    if (node_name && std::ranges::find(ignored_node_names, std::string_view(node_name)) != ignored_node_names.end())
        return;

    // Track output sinks to resolve stream targets to a device (excluding our own,
    // including sinks of previous graph generations that are still tearing down)
    if (strcmp(node_media_class, "Audio/Sink") == 0 || strcmp(node_media_class, "Audio/Duplex") == 0) {
        const size_t base_len = strlen(manager->capture_node_name_base);
        if (node_name && strncmp(node_name, manager->capture_node_name_base, base_len) == 0) {
            // The app-facing bridge sink and the current effects sink are internal
            manager->internal_capture_ids.insert(id);
            if (strcmp(node_name, manager->capture_node_name_base) == 0)
                manager->bridge_capture_id = id;
            else if (node_name == manager->effects_capture_node_name)
                manager->effects_capture_id = id;
            if (manager->link_wait_active.load(std::memory_order_relaxed))
                pw_thread_loop_signal(manager->thread_loop, false);
        } else if (node_name) {
            const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
            const char *form_factor = spa_dict_lookup(props, "device.form-factor");
            if (!form_factor)
                form_factor = spa_dict_lookup(props, "device.form_factor");
            const char *priority_text = spa_dict_lookup(props, "priority.session");
            if (!priority_text)
                priority_text = spa_dict_lookup(props, "priority.driver");
            manager->sinks[id] = SinkInfo{node_name, serial ? serial : "", form_factor ? form_factor : "",
                                          priority_text ? std::atoi(priority_text) : 0};
            // The registry global does not carry device.form-factor, so bind the
            // sink node to read its full info. This is also what tells the
            // built-in speaker detector which output is internal.
            if (!manager->sink_bindings.contains(id)) {
                pw_proxy *proxy = static_cast<pw_proxy *>(
                    pw_registry_bind(manager->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
                if (proxy) {
                    SinkBinding *binding = new SinkBinding{manager, id, proxy, {}};
                    manager->sink_bindings.insert(id, binding);
                    pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &binding->listener,
                                         &manager->sink_node_events, binding);
                }
            }
        }
        return;
    }

    // Check if object is an audio output stream, but exclude system events and our virtual surround manager source.
    if (strcmp(node_media_class, "Stream/Output/Audio") != 0)
        return;
    if (node_media_role && strcmp(node_media_role, "Notification") == 0)
        return;
    if (node_name && strncmp(node_name, manager->playback_node_name_base, strlen(manager->playback_node_name_base)) == 0) {
        manager->internal_playback_ids.insert(id);
        if (strcmp(node_name, manager->playback_node_name_base) == 0) {
            manager->bridge_playback_id = id;
        } else if (node_name == manager->effects_playback_node_name) {
            manager->effects_playback_id = id;
            if (!manager->replacing_effects)
                manager->bind_effects_control_locked();
        }
        if (manager->link_wait_active.load(std::memory_order_relaxed))
            pw_thread_loop_signal(manager->thread_loop, false);
        return;
    }

    if (manager->node_bindings.contains(id))
        return;

    // Bind the node to read target.object, which the registry global does not include
    pw_proxy *proxy = static_cast<pw_proxy *>(pw_registry_bind(manager->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!proxy) {
        qWarning("registry_event_global: Failed to bind node with ID %u", id);
        return;
    }

    NodeBinding *binding = new NodeBinding{manager, id, proxy, {}, {}};
    spa_zero(binding->listener);
    // Insert before adding the listener: add_listener dispatches the initial node info
    // synchronously and the handler needs to find this binding in the map already.
    manager->node_bindings.insert(id, binding);
    pw_node_add_listener(reinterpret_cast<pw_node *>(binding->proxy), &binding->listener, &manager->node_events, binding);

    qDebug("registry_event_global: Tracking output stream '%s' with ID %u", node_name ? node_name : "(null)", id);
}

void PipeWireManager::registry_event_global_remove(void *data, uint32_t id) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    // The bound metadata proxy is only marked removed by the core, not freed,
    // so it is safe to destroy it here ourselves
    if (id == manager->metadata_global_id) {
        if (manager->metadata) {
            spa_hook_remove(&manager->metadata_listener);
            pw_proxy_destroy((pw_proxy *)manager->metadata);
            manager->metadata = nullptr;
        }
        manager->metadata_global_id = SPA_ID_INVALID;
        manager->metadata_targets.clear();
    }

    auto link_it = manager->links.find(id);
    if (link_it != manager->links.end()) {
        const LinkInfo link = link_it.value();
        manager->links.remove(id);
        if (link.input_node == manager->bridge_capture_id && manager->node_bindings.contains(link.output_node)) {
            // A manual move away from the bridge leaves no link; stop treating the
            // stream as routed so the playback/detection state stays truthful.
            bool still_on_bridge = false;
            for (const LinkInfo &other : manager->links) {
                if (other.output_node == link.output_node && other.input_node == manager->bridge_capture_id) {
                    still_on_bridge = true;
                    break;
                }
            }
            if (!still_on_bridge) {
                manager->routed_node_ids.remove(link.output_node);
                manager->update_playing_state();
                qInfo("registry_event_global_remove: Node %u left the bridge sink", link.output_node);
            }
        }
        if (manager->link_wait_active.load(std::memory_order_relaxed))
            pw_thread_loop_signal(manager->thread_loop, false);
    }

    auto sink_binding = manager->sink_bindings.find(id);
    if (sink_binding != manager->sink_bindings.end()) {
        spa_hook_remove(&sink_binding.value()->listener);
        pw_proxy_destroy(sink_binding.value()->proxy);
        delete sink_binding.value();
        manager->sink_bindings.erase(sink_binding);
    }

    manager->sinks.remove(id);
    manager->metadata_targets.remove(id);
    manager->routed_node_ids.remove(id);
    manager->decided_streams.remove(id);
    manager->effects_redirect_ms.remove(id);
    manager->internal_playback_ids.remove(id);
    manager->internal_capture_ids.remove(id);

    // Do not let stale IDs suppress metadata handling of future nodes reusing them
    if (manager->effects_playback_control_id == id)
        manager->clear_effects_control_locked();
    if (manager->bridge_capture_id == id)
        manager->bridge_capture_id = SPA_ID_INVALID;
    if (manager->bridge_playback_id == id)
        manager->bridge_playback_id = SPA_ID_INVALID;
    if (manager->effects_capture_id == id)
        manager->effects_capture_id = SPA_ID_INVALID;
    if (manager->effects_playback_id == id)
        manager->effects_playback_id = SPA_ID_INVALID;

    auto binding = manager->node_bindings.find(id);
    if (binding != manager->node_bindings.end()) {
        manager->destroy_binding(binding.value());
        manager->node_bindings.erase(binding);
        manager->update_playing_state();
    }
}

void PipeWireManager::destroy_binding(NodeBinding *binding) {
    if (binding->listener_attached)
        spa_hook_remove(&binding->listener);
    pw_proxy_destroy(binding->proxy);
    delete binding;
}

void PipeWireManager::detach_binding_listener(NodeBinding *binding) {
    if (!binding->listener_attached)
        return;
    spa_hook_remove(&binding->listener);
    binding->listener_attached = false;
}

void PipeWireManager::attach_binding_listener(NodeBinding *binding) {
    if (binding->listener_attached)
        return;
    spa_zero(binding->listener);
    binding->listener_attached = true;
    pw_node_add_listener(reinterpret_cast<pw_node *>(binding->proxy), &binding->listener, &node_events, binding);
}

string PipeWireManager::build_effects_module_args(const string &hrir_wav_path, bool upmix_enabled,
                                                  const string &capture_name, const string &playback_name,
                                                  double output_trim_db, bool output_limiter) {
    if (hrir_wav_path.empty()) {
        qWarning("build_effects_module_args: No HRIR file provided");
        return {};
    }

    HrirWav wav;
    QString wav_error;
    if (!HrirWav::read(QString::fromStdString(hrir_wav_path), wav, &wav_error)) {
        Q_EMIT error_occurred(QStringLiteral("无法使用所选 HRIR：%1").arg(wav_error));
        return {};
    }

    // The path is interpolated into the JSON module arguments, so escape it
    const string hrir_path = json_escape(hrir_wav_path);

    // Default timbre is the normalized coherent seven-direction average
    // (legacy config name diffuse). Delta adds no HRIR timbre; center uses
    // the original center pair. These are digital responses, not earphone EQ.
    const QString app_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(app_data);
    string lfe_conv_path;
    string lfe_channel_l = "0";
    string lfe_channel_r = "0";
    if (lfe_timbre == LfeTimbre::CenterHrir) {
        lfe_conv_path = hrir_path;
        lfe_channel_l = "6";
        lfe_channel_r = "13";
    } else if (lfe_timbre == LfeTimbre::Diffuse) {
        const QString diffuse_file = app_data + QStringLiteral("/lfe_diffuse_ir.wav");
        if (!write_lfe_diffuse_ir(hrir_wav_path, diffuse_file.toStdString())) {
            qWarning("build_effects_module_args: Could not create the diffuse LFE IR for '%s'", hrir_wav_path.c_str());
            Q_EMIT error_occurred(QStringLiteral("无法为所选 HRIR 生成扩散场 LFE IR。"));
            return {};
        }
        lfe_conv_path = json_escape(diffuse_file.toStdString());
        lfe_channel_l = "0";
        lfe_channel_r = "1";
    } else {
        const QString delta_file = app_data + QStringLiteral("/lfe_bypass_delta.wav");
        if (!write_lfe_bypass_delta(hrir_wav_path, delta_file.toStdString())) {
            qWarning("build_effects_module_args: Could not create LFE bypass impulse for '%s'", hrir_wav_path.c_str());
            Q_EMIT error_occurred(QStringLiteral("无法使用所选 HRIR。它必须是有效的 14 声道 HeSuVi WAV 文件（参见配置文件列表说明）。"));
            return {};
        }
        lfe_conv_path = json_escape(delta_file.toStdString());
    }

    string upmix_nodes;
    string upmix_links;
    string graph_inputs;
    const string use_lfe = "0.0";
    const LfeLowpassSpec lpf = lfe_lowpass_spec(lfe_lowpass);
    string lpf_nodes;
    string lpf_chain;
    for (int s = 0; s < lpf.sections; ++s) {
        lpf_nodes += "        { type = builtin label = bq_lowpass name = lfeLP" + std::to_string(s + 1) +
                     " control = { \"Freq\" = 120.0 \"Q\" = " + std::to_string(lpf.q[s]) + " } }\n";
        if (s > 0)
            lpf_chain += "        { output = \"lfeLP" + std::to_string(s) + ":Out\" input=\"lfeLP" +
                         std::to_string(s + 1) + ":In\" }\n";
    }
    const string lpf_first = "lfeLP1";
    const string lpf_last = "lfeLP" + std::to_string(lpf.sections);

    // Align the LFE path with the binaural main path. The fit compensates the
    // 120 Hz low-pass plus the LFE timbre IR against the main HRIR low-frequency
    // phase. One common delay is used for the main path so the L/R interaural
    // timing is untouched; LFE delays are per ear. The fitted all-pass only
    // rotates residual phase (unity magnitude).
    const LfeAlignment alignment =
        lfe_alignment_configured(hrir_wav_path, 48000, lfe_lowpass, lfe_timbre, lfe_per_ear_alignment);
    // Inert unless VSM_TEST_DISABLE_LFE_ALIGNMENT is set; the isolated
    // regression uses it as the unaligned control.
    const bool alignment_disabled = qEnvironmentVariableIsSet("VSM_TEST_DISABLE_LFE_ALIGNMENT");
    const double main_delay = alignment.valid && !alignment_disabled ? alignment.main_delay_seconds : 0.0;
    const double lfe_delay_l = alignment.valid && !alignment_disabled ? alignment.lfe_delay_l_seconds : 0.0;
    const double lfe_delay_r = alignment.valid && !alignment_disabled ? alignment.lfe_delay_r_seconds : 0.0;
    const bool allpass_l = alignment.valid && !alignment_disabled && alignment.allpass_l_valid;
    const bool allpass_r = alignment.valid && !alignment_disabled && alignment.allpass_r_valid;
    const string native_delay_l = std::to_string(lfe_delay_l);
    const string native_delay_r = std::to_string(lfe_delay_r);

    // The mono main-sum LFE send is the bass effect. It is rendered through
    // the diffuse LFE path (120 Hz BW4 + diffuse IR + alignment), so the added
    // low-frequency layer is non-frontal.
    // - Balanced: the seven main channels are summed to mono at -6 dB inside
    //   the auto plugin's upmix branch; native LFE passes through the bypass
    //   branch (2x makeup there).
    // - Reference: no send; native LFE only.
    // - Impact: the same send at 0 dB instead of -6 dB (2x amplitude).
    // A send is only safe when the post-alignment phase residual is small; for
    // presets without a usable delay fit the send can cancel the main bass
    // (measured -4.6 dB at 40 Hz for AtmosRenderer-Far, -3.5 dB at 100 Hz for
    // dtshx). Disable the send entirely in that case; impact additionally needs
    // the tighter 15 deg bound for its 0 dB level, otherwise it falls back to
    // the -6 dB send.
    const bool send_safe = lfe_send_safe(alignment);
    const bool impact_allowed = lfe_impact_allowed(alignment);
    const double lfe_send_balanced = send_safe ? VsmDsp::balanced_lfe_send : 0.0;
    const double lfe_send_impact = send_safe
        ? (impact_allowed ? VsmDsp::impact_lfe_send : VsmDsp::balanced_lfe_send) : 0.0;
    // Persist the gated levels: runtime LFE-mode switches (update_processing)
    // must not exceed what the currently loaded graph was fitted for.
    lfe_send_balanced_ = lfe_send_balanced;
    lfe_send_impact_ = lfe_send_impact;
    const double lfe_send = upmix_enabled && lfe_mode != LfeMode::Reference
        ? (lfe_mode == LfeMode::Impact ? lfe_send_impact : lfe_send_balanced) : 0.0;
    if (upmix_enabled && lfe_mode != LfeMode::Reference && !send_safe) {
        qInfo("build_effects_module_args: LFE send disabled for this HRIR "
              "(post-alignment residual %.1f deg > 30 deg)",
              lfe_effective_residual_degrees(alignment));
    }

    string alignment_nodes;
    if (main_delay > 0.0) {
        const string delay_value = std::to_string(main_delay);
        alignment_nodes += R"graph(
        # Common main-path delay for LFE/main low-frequency alignment.
        # The built-in delay reports its *config* latency, not the runtime
        # "Delay (s)" control, so both must be set for the graph's reported
        # latency to include this alignment delay.
        { type = builtin label = delay name = mainAlignL config = { "max-delay" = 0.1 "latency" = )graph" +
                           delay_value + R"graph( } control = { "Delay (s)" = )graph" + delay_value + R"graph( } }
        { type = builtin label = delay name = mainAlignR config = { "max-delay" = 0.1 "latency" = )graph" +
                           delay_value + R"graph( } control = { "Delay (s)" = )graph" + delay_value + R"graph( } }
)graph";
    }

    string lfe_source_link = R"graph({ output = "copyLFE:Out" input=")graph" + lpf_first + R"graph(:In" })graph";
    string lfe_align_links;
    string lfe_phase_nodes;
    if (allpass_l) {
        lfe_phase_nodes += "        { type = builtin label = bq_allpass name = apLFE_L control = { \"Freq\" = " +
                           std::to_string(alignment.allpass_l_freq) + " \"Q\" = " +
                           std::to_string(alignment.allpass_l_q) + " } }\n";
    }
    if (allpass_r) {
        lfe_phase_nodes += "        { type = builtin label = bq_allpass name = apLFE_R control = { \"Freq\" = " +
                           std::to_string(alignment.allpass_r_freq) + " \"Q\" = " +
                           std::to_string(alignment.allpass_r_q) + " } }\n";
    }
    const auto lfe_gain_link = [&](const char *ear, bool allpass) {
        const string gain = string("gainLFE_") + ear;
        if (!allpass)
            return "        { output = \"" + lpf_last + ":Out\" input=\"" + gain + ":In\" }\n";
        const string ap = string("apLFE_") + ear;
        return "        { output = \"" + lpf_last + ":Out\" input=\"" + ap + ":In\" }\n" +
               "        { output = \"" + ap + ":Out\" input=\"" + gain + ":In\" }\n";
    };
    const string lfe_phase_links = "\n" + lfe_gain_link("L", allpass_l) + lfe_gain_link("R", allpass_r);
    string main_chain_links;
    if (main_delay > 0.0) {
        main_chain_links = R"graph(
        { output = "mixL:Out" input="mainAlignL:In" }
        { output = "mixR:Out" input="mainAlignR:In" }
        { output = "mainAlignL:Out" input="finalL:In 1" }
        { output = "mainAlignR:Out" input="finalR:In 1" }
)graph";
    } else {
        main_chain_links = R"graph(
        { output = "mixL:Out" input="finalL:In 1" }
        { output = "mixR:Out" input="finalR:In 1" }
)graph";
    }
    string main_input_links;
    const char *speakers[] = {"FL", "FR", "FC", "RL", "RR", "SL", "SR"};
    for (const char *speaker : speakers) {
        for (const char *ear : {"L", "R"})
            main_input_links += "        { output = \"copy" + string(speaker) + ":Out\" input=\"conv" +
                                speaker + "_" + ear + ":In\" }\n";
    }
    const char *mode_name = lfe_mode == LfeMode::Balanced ? "balanced"
                            : lfe_mode == LfeMode::Reference ? "reference"
                                                             : "impact";
    qInfo("build_effects_module_args: LFE mode %s (send %.3f, %s), trim %+.2f dB, "
          "LFE/main alignment main %.2f ms / LFE L %.2f R %.2f ms (residual L %.1f R %.1f deg)%s",
          mode_name, lfe_send, send_safe ? "residual within 30 deg" : "send gated off", output_trim_db, main_delay * 1000.0, lfe_delay_l * 1000.0,
          lfe_delay_r * 1000.0, alignment.average_residual_l_degrees,
          alignment.average_residual_r_degrees, allpass_l || allpass_r ? ", all-pass enabled" : "");
    if (alignment.valid)
        qInfo("build_effects_module_args: all-pass L %s %.2f Hz Q %.3f (res %.2f deg) / R %s %.2f Hz Q %.3f (res %.2f deg)",
              allpass_l ? "on" : "off", alignment.allpass_l_freq, alignment.allpass_l_q,
              alignment.allpass_l_residual_degrees, allpass_r ? "on" : "off",
              alignment.allpass_r_freq, alignment.allpass_r_q, alignment.allpass_r_residual_degrees);
    const QString plugin_path = find_ladspa_plugin();
    if (plugin_path.isEmpty()) {
        Q_EMIT error_occurred(QStringLiteral("未找到新版DSP插件。请重新安装。"));
        return {};
    }
    const bool limiter_enabled = output_limiter;
    string limiter_nodes;
    string limiter_links;
    string graph_outputs = R"graph( [ "gateL:Out" "gateR:Out" ])graph";
    string gate_links = R"graph(
        { output = "outGainL:Out" input="gateL:In" }
        { output = "outGainR:Out" input="gateR:In" }
)graph";
    if (limiter_enabled) {
        const string limiter_plugin = json_escape(plugin_path.toStdString());
        limiter_nodes = R"json(
        # Optional output safety limiter (stereo-linked, 5 ms lookahead,
        # -1.5 dBFS threshold, 100 ms release). Transparent below threshold;
        # adds 5 ms latency. The user can turn it off in Settings.
        { type = ladspa name = limiter plugin = ")json" + limiter_plugin + R"json(" label = "output_limiter"
          control = { "Threshold (dBFS)" = -1.5 "Release (ms)" = 100.0 "Lookahead (ms)" = 5.0 } }
)json";
        limiter_links = R"json(
        { output = "outGainL:Out" input="limiter:In L" }
        { output = "outGainR:Out" input="limiter:In R" }
)json";
        gate_links = R"graph(
        { output = "limiter:Out L" input="gateL:In" }
        { output = "limiter:Out R" input="gateR:In" }
)graph";
        qInfo("build_effects_module_args: Output limiter enabled (-1.5 dBFS, 5 ms lookahead)");
    }

    {
        qInfo("create_virtual_surround_module: Using FreeSurround plugin '%s'", plugin_path.toStdString().c_str());

        // The plugin also runs in this process (the filter-chain module loads it
        // here), so dlopen() returns the already loaded object. The state export
        // is read later by upmix_auto_state() for the live input detection.
        if (!upmix_state_handle) {
            const QByteArray plugin_path_utf8 = plugin_path.toUtf8();
            upmix_state_handle = dlopen(plugin_path_utf8.constData(), RTLD_LAZY | RTLD_NOLOAD);
            if (!upmix_state_handle)
                upmix_state_handle = dlopen(plugin_path_utf8.constData(), RTLD_LAZY);
            if (upmix_state_handle) {
                upmix_state = reinterpret_cast<std::atomic<int> *>(dlsym(upmix_state_handle, "vsm_auto_upmix_state"));
                upmix_blocks = reinterpret_cast<std::atomic<uint64_t> *>(dlsym(upmix_state_handle, "vsm_auto_blocks"));
            } else {
                qWarning("build_effects_module_args: Could not open '%s' to read the upmix detection state",
                         plugin_path_utf8.constData());
            }
            if (!upmix_state)
                qWarning("build_effects_module_args: The plugin does not export vsm_auto_upmix_state");
        }

        // FreeSurround stereo to 7.1 upmixer with the Real3D-VST auto-detection:
        // stereo content is upmixed, real multichannel content is passed through
        // unchanged (with matching latency compensation). Bass redirection stays
        // off (use_lfe=0); the LFE send is handled by this graph instead.
        upmix_nodes = R"graph(
        # FreeSurround stereo to 7.1 upmixer with automatic stereo detection:
        # only upmixes while all non-L/R input channels are silent
        { type = ladspa name = fs plugin = ")graph" + json_escape(plugin_path.toStdString()) + R"graph(" label = "freesurround71_auto"
          control = {
              # center_image is intentionally not set: upstream FreeSurround
              # stores it but never reads it in the decode (no-op).
              "shift" = 0.0
              "depth" = 1.0
              "circular_wrap" = 90.0
              "focus" = 0.0
              "front_sep" = 1.0
              "rear_sep" = 1.0
              "bass_lo" = 70.0
              "bass_hi" = 90.0
              "use_lfe" = )graph" + use_lfe + R"graph(
              "upmix_enable" = )graph" + string(upmix_enabled ? "1.0" : "0.0") + R"graph(
              "lfe_send" = )graph" + std::to_string(lfe_send) + R"graph(
          }
        }
)graph";
        upmix_links = R"graph(
        # upmixer outputs feed the copy nodes (which fan out to the HRIR convolvers)
        { output = "fs:Out FL"  input="copyFL:In"  }
        { output = "fs:Out FR"  input="copyFR:In"  }
        { output = "fs:Out FC"  input="copyFC:In"  }
        { output = "fs:Out RL"  input="copyRL:In"  }
        { output = "fs:Out RR"  input="copyRR:In"  }
        { output = "fs:Out SL"  input="copySL:In"  }
        { output = "fs:Out SR"  input="copySR:In"  }
)graph";
        upmix_links += R"graph(
        # The plugin's LFE output is the native LFE (2x) in bypass mode and the
        # mono main-sum send while upmixing, so the graph always takes it.
        { output = "fs:Out LFE" input="copyLFE:In" }
)graph";
        graph_inputs = R"graph(  [ "fs:In FL" "fs:In FR" "fs:In FC" "fs:In LFE" "fs:In RL" "fs:In RR" "fs:In SL" "fs:In SR" ])graph";
    }

    // Native-LFE digital makeup remains 2x (+6.0206 dB), the historical product
    // convention. It is not a universal headphone/SPL calibration. Actual LFE
    // relative level depends on the selected IR, frequency and signal covariance.
    // The auto bypass already applies 2x; do not apply it twice or to the send.
    const string lfe_gain = "1.0";
    // Set 7.1 filter graph for virtual surround module, automatically creating playback and capture nodes
    const string filter_graph = R"graph(
{
    nodes = [)graph" + upmix_nodes + R"graph(
        # duplicate inputs
        { type = builtin label = copy name = copyFL  }
        { type = builtin label = copy name = copyFR  }
        { type = builtin label = copy name = copyFC  }
        { type = builtin label = copy name = copyRL  }
        { type = builtin label = copy name = copyRR  }
        { type = builtin label = copy name = copySL  }
        { type = builtin label = copy name = copySR  }
        { type = builtin label = copy name = copyLFE }


        # apply hrir - HeSuVi 14-channel WAV (not the *-.wav variants) (note: 44 in HeSuVi are the same, but resampled to 44100)
        # Channel order follows HeSuVi's convolution channel line
        # (L0 R1 SL0 SR1 RL0 RR1 C0 R0 L1 SR0 SL1 RR0 RL1 C1 from the stock
        # hesuvi.txt): channels 7-12 interleave the right-ear IRs of the
        # right-side speakers, which is why the indices look shuffled against
        # the speaker labels. Independently verified via the ITD and level
        # asymmetry of the IR files themselves (see "HRIR channel mapping" in
        # README.md) - do not "fix" these based on recordings.
        # resample_quality = 14: PipeWire's convolver defaults to 4 when the IR
        # file rate differs from the graph rate. Imported 44.1/96 kHz HeSuVi
        # presets then get the highest-quality one-time IR resample; 48 kHz
        # files are not resampled, so this costs nothing for the bundled set.
        { type = builtin label = convolver name = convFL_L config = { filename = ")graph" +
                                    hrir_path + R"graph(" channel =  0 resample_quality = 14 } }
        { type = builtin label = convolver name = convFL_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  1 resample_quality = 14 } }
        { type = builtin label = convolver name = convSL_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  2 resample_quality = 14 } }
        { type = builtin label = convolver name = convSL_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  3 resample_quality = 14 } }
        { type = builtin label = convolver name = convRL_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  4 resample_quality = 14 } }
        { type = builtin label = convolver name = convRL_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  5 resample_quality = 14 } }
        { type = builtin label = convolver name = convFC_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  6 resample_quality = 14 } }
        { type = builtin label = convolver name = convFR_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  7 resample_quality = 14 } }
        { type = builtin label = convolver name = convFR_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  8 resample_quality = 14 } }
        { type = builtin label = convolver name = convSR_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel =  9 resample_quality = 14 } }
        { type = builtin label = convolver name = convSR_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel = 10 resample_quality = 14 } }
        { type = builtin label = convolver name = convRR_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel = 11 resample_quality = 14 } }
        { type = builtin label = convolver name = convRR_L config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel = 12 resample_quality = 14 } }
        { type = builtin label = convolver name = convFC_R config = { filename = ")graph" +
                                        hrir_path + R"graph(" channel = 13 resample_quality = 14 } }

        # LFE path: convolved with the selected LFE timbre IR, all of the same
        # length as the HRIR so both branches stay time-aligned. delta = unit
        # impulse (uncolored, equal L/R); diffuse = 7-pair coherent average
        # normalized to the center HRIR; center = center HRIR pair. The last
        # two deliberately apply HRIR timbre and may differ slightly between
        # ears; only delta is the strict conventional LFE render.
        { type = builtin label = convolver name = convLFE_L config = { filename = ")graph" +
                                        lfe_conv_path + R"graph(" channel = )graph" +
                                        lfe_channel_l + R"graph( resample_quality = 14 } }
        { type = builtin label = convolver name = convLFE_R config = { filename = ")graph" +
                                        lfe_conv_path + R"graph(" channel = )graph" +
                                        lfe_channel_r + R"graph( resample_quality = 14 } }

        # LFE path low-pass: fixed 120 Hz Butterworth-4 (Q=0.5412/1.3066, 24 dB/oct).
        # At 120 Hz this is -3 dB, so the LFE is 3 dB hotter at 100-120 Hz than the
        # earlier -6 dB LR4; that is the intended BW4 impact, not a level bug.
        # The IR is the selected diffuse/delta/center LFE timbre IR. The always
        # present auto plugin applies the native 2x makeup in its bypass branch
        # (and the balanced/impact send is already at its intended level), so
        # this graph gain stays 1.0 for both branches.
)graph" + lpf_nodes + lfe_phase_nodes + R"graph(
        { type = builtin label = linear name = gainLFE_L control = { "Mult" = )graph" + lfe_gain + R"graph( } }
        { type = builtin label = linear name = gainLFE_R control = { "Mult" = )graph" + lfe_gain + R"graph( } }

        # stereo output
        { type = builtin label = mixer name = mixL }
        { type = builtin label = mixer name = mixR }
        { type = builtin label = delay name = nativeDelayL config = { "max-delay" = 0.1 "latency" = )graph" + native_delay_l + R"graph( }
          control = { "Delay (s)" = )graph" + native_delay_l + R"graph( } }
        { type = builtin label = delay name = nativeDelayR config = { "max-delay" = 0.1 "latency" = )graph" + native_delay_r + R"graph( }
          control = { "Delay (s)" = )graph" + native_delay_r + R"graph( } }
        { type = builtin label = mixer name = finalL }
        { type = builtin label = mixer name = finalR }
)graph" + alignment_nodes + R"graph(

        # Nominal digital attenuation: 0.30 = -10.46 dB before optional trim.
        # Not a bound on arbitrary multichannel/FIR peaks or acoustic SPL.
        { type = builtin label = linear name = outGainL control = { "Mult" = )graph" + std::to_string(0.30 * std::pow(10.0, output_trim_db / 20.0)) + R"graph( } }
        { type = builtin label = linear name = outGainR control = { "Mult" = )graph" + std::to_string(0.30 * std::pow(10.0, output_trim_db / 20.0)) + R"graph( } }
)graph" + limiter_nodes + R"graph(
        # Application-owned gate, independent of the user's node mute/volume.
        { type = builtin label = linear name = gateL control = { "Mult" = 1.0 } }
        { type = builtin label = linear name = gateR control = { "Mult" = 1.0 } }
    ]
    links = [)graph" + upmix_links + R"graph(
        # input
        )graph" + main_input_links + R"graph(
        )graph" + lfe_source_link + R"graph(
        )graph" + lfe_align_links + R"graph(
        )graph" + lpf_chain + lfe_phase_links + R"graph(
        { output = "gainLFE_L:Out" input="convLFE_L:In" }
        { output = "gainLFE_R:Out" input="convLFE_R:In" }

        # output
        { output = "convFL_L:Out"  input="mixL:In 1" }
        { output = "convFL_R:Out"  input="mixR:In 1" }
        { output = "convSL_L:Out"  input="mixL:In 2" }
        { output = "convSL_R:Out"  input="mixR:In 2" }
        { output = "convRL_L:Out"  input="mixL:In 3" }
        { output = "convRL_R:Out"  input="mixR:In 3" }
        { output = "convFC_L:Out"  input="mixL:In 4" }
        { output = "convFC_R:Out"  input="mixR:In 4" }
        { output = "convFR_R:Out"  input="mixR:In 5" }
        { output = "convFR_L:Out"  input="mixL:In 5" }
        { output = "convSR_R:Out"  input="mixR:In 6" }
        { output = "convSR_L:Out"  input="mixL:In 6" }
        { output = "convRR_R:Out"  input="mixR:In 7" }
        { output = "convRR_L:Out"  input="mixL:In 7" }
)graph" + main_chain_links + R"graph(
        { output = "convLFE_L:Out" input="nativeDelayL:In" }
        { output = "convLFE_R:Out" input="nativeDelayR:In" }
        { output = "nativeDelayL:Out" input="finalL:In 2" }
        { output = "nativeDelayR:Out" input="finalR:In 2" }
        { output = "finalL:Out" input="outGainL:In" }
        { output = "finalR:Out" input="outGainR:In" }
)graph" + limiter_links + gate_links + R"graph(
    ]
    inputs  = )graph" + graph_inputs + R"graph(
    outputs = )graph" + graph_outputs + R"graph(
}
        )graph";

    // node.latency is a scheduling suggestion, not a minimum quantum (see
    // pipewire-props(7)): the graph may run at a smaller or forced quantum.
    // The upmixer must therefore preserve its timebase for arbitrary blocks.
    // audio.rate requests 48 kHz at the stream boundary; PipeWire converts
    // device/client rates as needed. Verify the negotiated graph rate when
    // measuring latency rather than inferring it from this property alone.
    const string capture_props = R"graph(
{
    node.name = )graph" + capture_name +
                                         R"graph(
    node.description = "虚拟环绕声（内部效果）"
    media.class = "Audio/Sink"
    priority.session = 0
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 8
    audio.position = [ FL FR FC LFE RL RR SL SR ]
}
        )graph";

    // Set playback properties for virtual surround module
    const string playback_props = R"graph(
{
    node.name = )graph" + playback_name +
                                          R"graph(
    node.passive = true
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 2
    audio.position = [ FL FR ]
}
        )graph";

    // Combine the properites to a single JSON string
    const string args = R"graph(
{
    node.description = "虚拟环绕声（内部效果）"
    media.name = "虚拟环绕声（内部效果）"
    filter.graph = )graph" +
               filter_graph +
               R"graph(,
    capture.props = )graph" +
               capture_props + R"graph(,
    playback.props = )graph" +
               playback_props + R"graph(
}
        )graph";

    if (qEnvironmentVariableIsSet("VSM_DUMP_GRAPH"))
        qInfo("build_effects_module_args: %s", args.c_str());
    return args;
}

bool PipeWireManager::create_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                                     double output_trim_db, bool output_limiter) {
    if (!ensure_connection()) {
        Q_EMIT error_occurred(QStringLiteral("连接 PipeWire 音频服务失败。"));
        return false;
    }
    pw_thread_loop_lock(thread_loop);
    if (bridge_module) {
        const bool ready = effects_module != nullptr;
        pw_thread_loop_unlock(thread_loop);
        return ready;
    }

    capture_node_name = capture_node_name_base;
    effects_generation = 0;

    qInfo("create_virtual_surround_module: Creating device with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    pw_impl_module *new_effects = load_effects_module_locked(hrir_wav_path, upmix_enabled, output_trim_db, output_limiter);
    if (!new_effects) {
        effects_capture_node_name.clear();
        effects_playback_node_name.clear();
        pw_thread_loop_unlock(thread_loop);
        Q_EMIT error_occurred(QStringLiteral("创建虚拟环绕声设备失败。"));
        return false;
    }

    // The bridge names the effects sink as its target, so let the server register
    // the effects nodes first
    sync_with_core(1);

    if (!is_valid() || !module_bindings.contains(new_effects) || !load_bridge_module_locked()) {
        destroy_module_locked(new_effects);
        sync_with_core(1);
        effects_capture_node_name.clear();
        effects_playback_node_name.clear();
        pw_thread_loop_unlock(thread_loop);
        qWarning("create_virtual_surround_module: Failed to load the bridge module");
        Q_EMIT error_occurred(QStringLiteral("创建虚拟环绕声设备失败。"));
        return false;
    }

    effects_module = new_effects;
    graph_upmix_enabled.store(upmix_enabled, std::memory_order_relaxed);
    sync_with_core(1);
    const bool created = is_valid() && bridge_module && module_bindings.contains(new_effects);
    pw_thread_loop_unlock(thread_loop);

    if (!created) {
        Q_EMIT error_occurred(QStringLiteral("音频设备创建过程中连接已断开。"));
        return false;
    }
    qInfo("create_virtual_surround_module: Successfully created device");
    return true;
}

bool PipeWireManager::replace_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                                      double output_trim_db, bool output_limiter) {
    if (!thread_loop)
        return false;
    pw_thread_loop_lock(thread_loop);
    if (!bridge_module) {
        pw_thread_loop_unlock(thread_loop);
        return create_virtual_surround_module(hrir_wav_path, upmix_enabled, output_trim_db, output_limiter);
    }

    if (!is_valid()) {
        pw_thread_loop_unlock(thread_loop);
        Q_EMIT error_occurred(QStringLiteral("连接 PipeWire 音频服务失败。"));
        return false;
    }

    if (!is_registry_listener_added || !metadata) {
        pw_thread_loop_unlock(thread_loop);
        // Only called while routing is enabled; without the registry listener the
        // internal node IDs are unknown and the bridge cannot be re-targeted
        qWarning("replace_virtual_surround_module: Routing is not enabled");
        return false;
    }

    qInfo("replace_virtual_surround_module: Replacing effects graph with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    QElapsedTimer switch_timer;
    switch_timer.start();

    // Pick up node globals that arrived since the last iteration.
    if (!sync_with_core(1) || !bridge_module || !effects_module || !metadata) {
        pw_thread_loop_unlock(thread_loop);
        return false;
    }

    const string old_capture_name = effects_capture_node_name;
    const string old_playback_name = effects_playback_node_name;
    const uint32_t old_capture_id = effects_capture_id;
    const uint32_t old_playback_id = effects_playback_id;

    if (bridge_playback_id == SPA_ID_INVALID || old_capture_id == SPA_ID_INVALID) {
        pw_thread_loop_unlock(thread_loop);
        qWarning("replace_virtual_surround_module: Bridge or effects nodes are not known yet");
        Q_EMIT error_occurred(QStringLiteral("虚拟环绕声设备尚未就绪，请重试。"));
        return false;
    }

    replacing_effects = true;
    pw_impl_module *old_effects = effects_module;
    const auto restore_state = [&]() {
        effects_capture_node_name = old_capture_name;
        effects_playback_node_name = old_playback_name;
        effects_capture_id = old_capture_id;
        effects_playback_id = old_playback_id;
        replacing_effects = false;
    };
    pw_impl_module *new_effects = load_effects_module_locked(hrir_wav_path, upmix_enabled, output_trim_db, output_limiter);
    if (!new_effects) {
        restore_state();
        pw_thread_loop_unlock(thread_loop);
        qWarning("replace_virtual_surround_module: Failed to load the new effects module, keeping the old one");
        Q_EMIT error_occurred(QStringLiteral("无法切换虚拟环绕声配置。上一配置仍在运行，请检查所选 HRIR 文件。"));
        return false;
    }

    // The filter-chain module registers its nodes through its own client connection,
    // so they are not covered by our core sync. Wait for the new sink (and its
    // playback node) to show up in the registry before pointing the bridge at it.
    const bool sink_ready = wait_for([this]() { return effects_capture_id != SPA_ID_INVALID; }, 1000);
    if (sink_ready)
        wait_for([this]() { return effects_playback_id != SPA_ID_INVALID; }, 500);
    qDebug("replace_virtual_surround_module: New effects sink '%s' id %u, playback id %u",
           effects_capture_node_name.c_str(), effects_capture_id, effects_playback_id);

    // Give the new effects output a device link first, so it is already processing
    // when the audio arrives. A graph without a device link would consume nothing
    // and come out silent until the next switch, so this is worth waiting for.
    bool playback_linked = false;
    if (effects_playback_id != SPA_ID_INVALID)
        playback_linked = wait_for([this]() { return has_outgoing_link(effects_playback_id); }, 600);
    qDebug("replace_virtual_surround_module: New effects playback node link: %d", playback_linked);

    const uint32_t new_capture_id = effects_capture_id;
    bool linked = false;

    if (new_capture_id != SPA_ID_INVALID && playback_linked && is_valid() &&
        module_bindings.contains(new_effects)) {
        // Re-target the bridge playback stream: the application streams stay linked
        // to the bridge sink, so they never touch the physical device and no sink
        // they are linked to is removed (which is what makes WirePlumber pause
        // MPRIS players and what made the output jump in level).
        // Retry the write once in case WirePlumber had not picked up the new sink yet.
        for (int attempt = 0; attempt < 2 && !linked && is_valid() && metadata; ++attempt) {
            pw_metadata_set_property(metadata, bridge_playback_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", effects_capture_node_name.c_str());
            qDebug("replace_virtual_surround_module: Bridge node %u re-targeted to effects sink '%s' (attempt %d)",
                   bridge_playback_id, effects_capture_node_name.c_str(), attempt + 1);
            sync_with_core(1);
            linked = wait_for([this, new_capture_id]() { return has_link(bridge_playback_id, new_capture_id); }, 600);
        }
    }

    if (!linked) {
        qWarning("replace_virtual_surround_module: New effects sink was not linked in time (sink id %u)", new_capture_id);

        // Put the bridge back on the old sink before discarding the new graph. If
        // that is confirmed, the old graph keeps playing and the setting is reverted.
        bool restored = false;
        if (is_valid() && metadata && module_bindings.contains(old_effects) &&
            old_capture_id != SPA_ID_INVALID && !old_capture_name.empty()) {
            pw_metadata_set_property(metadata, bridge_playback_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", old_capture_name.c_str());
            sync_with_core(1);
            restored = wait_for([this, old_capture_id]() { return has_link(bridge_playback_id, old_capture_id); }, 500);
        }

        restore_state();
        pw_thread_loop_unlock(thread_loop);
        // Even a failed candidate may have briefly acquired the bridge link.
        // Use the same deferred teardown as a successful replacement.
        schedule_effects_destroy(new_effects, new_capture_id);
        Q_EMIT error_occurred(restored
            ? QStringLiteral("无法切换虚拟环绕声配置。上一配置仍在运行。")
            : QStringLiteral("效果图连接失败，未能确认恢复上一配置。"));
        if (!restored)
            Q_EMIT connection_lost();
        return false;
    }

    qDebug("replace_virtual_surround_module: After bridge move, effects playback link: %d, bridge link: %d",
           has_outgoing_link(effects_playback_id), has_link(bridge_playback_id, new_capture_id));

    // WirePlumber updates its link bookkeeping asynchronously. Do not destroy the
    // old sink while the bridge is still linked to it: mpris-pause.lua follows the
    // upstream link from the removed sink and pauses the application's player.
    if (old_capture_id != SPA_ID_INVALID) {
        const bool old_released = wait_for([this, old_capture_id]() { return !has_input_link(old_capture_id); }, 1200);
        qDebug("replace_virtual_surround_module: Old effects sink input link cleared: %d", old_released);
    }

    if (!is_valid() || !module_bindings.contains(new_effects)) {
        restore_state();
        pw_thread_loop_unlock(thread_loop);
        schedule_effects_destroy(new_effects, new_capture_id);
        return false;
    }
    effects_module = new_effects;
    graph_upmix_enabled.store(upmix_enabled, std::memory_order_relaxed);
    replacing_effects = false;
    bind_effects_control_locked();
    pw_thread_loop_unlock(thread_loop);

    // The old sink stays around briefly: WirePlumber updates its link bookkeeping
    // asynchronously, and removing a sink it still considers linked would pause
    // MPRIS media players. By now it has no input link anymore, so it is silent.
    schedule_effects_destroy(old_effects, old_capture_id);

    qInfo("replace_virtual_surround_module: Successfully replaced effects graph in %lld ms",
          static_cast<long long>(switch_timer.elapsed()));

    return true;
}

void PipeWireManager::schedule_effects_destroy(pw_impl_module *old_module, uint32_t old_sink_id, int attempts_left) {
    if (!old_module || !thread_loop)
        return;
    pw_thread_loop_lock(thread_loop);
    auto binding = module_bindings.constFind(old_module);
    if (binding == module_bindings.cend()) {
        pw_thread_loop_unlock(thread_loop);
        return;
    }
    const uint64_t generation = binding.value()->generation;
    if (!pending_effects_destroy.contains(old_module))
        pending_effects_destroy.append(old_module);
    pw_thread_loop_unlock(thread_loop);

    // Keep the silent old graph around long enough for WirePlumber's MPRIS link
    // bookkeeping to process the link removal (it syncs after the link event).
    // 250 ms was too short once graph creation started taking ~1 s: the old sink
    // was destroyed while links_in still remembered the bridge link, and
    // mpris-pause.lua paused the player even though the actual link was gone.
    QTimer::singleShot(1200, this, [this, old_module, old_sink_id, attempts_left, generation]() {
        if (!thread_loop)
            return;
        pw_thread_loop_lock(thread_loop);
        const auto binding = module_bindings.constFind(old_module);
        if (binding == module_bindings.cend() || binding.value()->generation != generation ||
            !pending_effects_destroy.contains(old_module)) {
            pw_thread_loop_unlock(thread_loop);
            return;
        }
        const bool still_linked = old_sink_id != SPA_ID_INVALID && has_input_link(old_sink_id);
        if (still_linked && attempts_left > 0) {
            pw_thread_loop_unlock(thread_loop);
            qDebug("schedule_effects_destroy: Old effects sink is still linked, retrying");
            schedule_effects_destroy(old_module, old_sink_id, attempts_left - 1);
            return;
        }
        if (still_linked) {
            // Removing it now would pause the player; leave it alive until the
            // final teardown instead
            pw_thread_loop_unlock(thread_loop);
            qWarning("schedule_effects_destroy: Old effects sink stayed linked, keeping it alive");
            return;
        }

        pending_effects_destroy.removeOne(old_module);
        destroy_module_locked(old_module);
        sync_with_core(1);
        pw_thread_loop_unlock(thread_loop);

        qDebug("schedule_effects_destroy: Destroyed replaced effects graph");
    });
}

void PipeWireManager::track_module_locked(pw_impl_module *module) {
    if (!module)
        return;
    auto *binding = new ModuleBinding{this, module, {}, ++module_generation};
    module_bindings.insert(module, binding);
    pw_impl_module_add_listener(module, &binding->listener, &module_events, binding);
}

void PipeWireManager::destroy_module_locked(pw_impl_module *module) {
    const auto it = module_bindings.find(module);
    if (it == module_bindings.end())
        return;
    it.value()->expected_destroy = true;
    pw_impl_module_destroy(module);
}

void PipeWireManager::on_module_destroy(void *data) {
    auto *binding = static_cast<ModuleBinding *>(data);
    auto *manager = binding->manager;
    auto *module = binding->module;
    const bool active = manager->bridge_module == module || manager->effects_module == module;
    const bool unexpected = active && !binding->expected_destroy && !manager->shutting_down;
    if (manager->bridge_module == module)
        manager->bridge_module = nullptr;
    if (manager->effects_module == module)
        manager->effects_module = nullptr;
    manager->pending_effects_destroy.removeAll(module);
    manager->module_bindings.remove(module);
    spa_hook_remove(&binding->listener);
    delete binding;
    if (unexpected) {
        manager->audio_playing.store(false, std::memory_order_relaxed);
        ++manager->output_gate_generation;
        Q_EMIT manager->error_occurred(QStringLiteral("音频效果模块已断开，虚拟环绕声已停止。"));
        Q_EMIT manager->connection_lost();
    }
    pw_thread_loop_signal(manager->thread_loop, false);
}

void PipeWireManager::destroy_pending_effects_locked() {
    const auto pending = pending_effects_destroy;
    for (pw_impl_module *module : pending)
        destroy_module_locked(module);
}

pw_impl_module *PipeWireManager::load_effects_module_locked(const string &hrir_wav_path, bool upmix_enabled,
                                                            double output_trim_db, bool output_limiter) {
    const uint32_t new_generation = effects_generation + 1;
    const string generation = std::to_string(new_generation);
    const string capture_name = string(capture_node_name_base) + ".effects." + generation;
    const string playback_name = string(playback_node_name_base) + ".effects." + generation;

    const string args = build_effects_module_args(hrir_wav_path, upmix_enabled, capture_name, playback_name,
                                                  output_trim_db, output_limiter);
    if (args.empty())
        return nullptr;

    // Registry callbacks compare against the member names; switch them before the
    // module's nodes are announced
    effects_capture_node_name = capture_name;
    effects_playback_node_name = playback_name;
    effects_capture_id = SPA_ID_INVALID;
    effects_playback_id = SPA_ID_INVALID;

    pw_impl_module *new_module = pw_context_load_module(context,
                                                        "libpipewire-module-filter-chain",
                                                        args.c_str(),
                                                        nullptr);
    if (!new_module) {
        qWarning("load_effects_module_locked: Failed to load module into PipeWire context");
        return nullptr;
    }

    track_module_locked(new_module);
    effects_generation = new_generation;
    return new_module;
}

bool PipeWireManager::load_bridge_module_locked() {
    const string args = build_bridge_module_args(effects_capture_node_name);
    if (args.empty())
        return false;

    bridge_module = pw_context_load_module(context,
                                          "libpipewire-module-filter-chain",
                                          args.c_str(),
                                          nullptr);
    track_module_locked(bridge_module);
    return bridge_module != nullptr;
}

string PipeWireManager::build_bridge_module_args(const string &effects_sink_name) {
    // A plain 8-channel pass-through. The capture side is the stable sink that
    // applications are routed to; the playback side feeds the current effects
    // sink. Keeping this module alive across profile switches is what makes the
    // switch inaudible to applications: their stream never moves, and the sink
    // they target never disappears.
    const string capture_props = R"(
{
    node.name = )" + string(capture_node_name_base) + R"(
    node.description = "虚拟环绕声"
    media.class = "Audio/Sink"
    priority.session = 0
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 8
    audio.position = [ FL FR FC LFE RL RR SL SR ]
}
        )";

    // node.linger + node.dont-fallback: when the named effects sink is momentarily
    // absent (e.g. during a WirePlumber restart) the bridge waits instead of
    // following the default device. A fallback would put unattenuated 7.1 on the
    // physical output, which is the loud jump this design exists to prevent.
    const string playback_props = R"(
{
    node.name = )" + string(playback_node_name_base) + R"(
    node.passive = true
    node.latency = "1024/48000"
    node.dont-fallback = true
    node.linger = true
    audio.rate = 48000
    audio.channels = 8
    audio.position = [ FL FR FC LFE RL RR SL SR ]
    target.object = ")" + json_escape(effects_sink_name) + R"("
}
        )";

    const string filter_graph = R"(
{
    nodes = [
        { type = builtin label = copy name = copyFL }
        { type = builtin label = copy name = copyFR }
        { type = builtin label = copy name = copyFC }
        { type = builtin label = copy name = copyLFE }
        { type = builtin label = copy name = copyRL }
        { type = builtin label = copy name = copyRR }
        { type = builtin label = copy name = copySL }
        { type = builtin label = copy name = copySR }
    ]
    links = [ ]
    inputs  = [ "copyFL:In" "copyFR:In" "copyFC:In" "copyLFE:In" "copyRL:In" "copyRR:In" "copySL:In" "copySR:In" ]
    outputs = [ "copyFL:Out" "copyFR:Out" "copyFC:Out" "copyLFE:Out" "copyRL:Out" "copyRR:Out" "copySL:Out" "copySR:Out" ]
}
        )";

    return R"(
{
    node.description = "虚拟环绕声"
    media.name = "虚拟环绕声"
    filter.graph = )" +
        filter_graph + R"(,
    capture.props = )" + capture_props + R"(,
    playback.props = )" + playback_props + R"(
}
        )";
}

bool PipeWireManager::has_link(uint32_t output_node, uint32_t input_node) const {
    for (const LinkInfo &link : links) {
        if (link.output_node == output_node && link.input_node == input_node)
            return true;
    }
    return false;
}

bool PipeWireManager::has_outgoing_link(uint32_t output_node) const {
    for (const LinkInfo &link : links) {
        if (link.output_node == output_node)
            return true;
    }
    return false;
}

bool PipeWireManager::has_input_link(uint32_t input_node) const {
    for (const LinkInfo &link : links) {
        if (link.input_node == input_node)
            return true;
    }
    return false;
}

bool PipeWireManager::wait_for(const std::function<bool()> &predicate, int timeout_ms) {
    if (predicate())
        return true;
    if (!thread_loop)
        return false;

    link_wait_active.store(true, std::memory_order_relaxed);
    QElapsedTimer elapsed;
    elapsed.start();
    bool satisfied = false;

    // Wait in slices so registry events are dispatched even if no explicit signal
    // arrives (the link/global callbacks signal the loop, but a missed wakeup must
    // not turn into a full-timeout stall)
    while (is_valid() && !(satisfied = predicate())) {
        const int remaining = timeout_ms - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0)
            break;

        struct timespec abstime;
        pw_thread_loop_get_time(thread_loop, &abstime,
                                static_cast<int64_t>(std::min(remaining, 50)) * SPA_NSEC_PER_MSEC);
        pw_thread_loop_timed_wait_full(thread_loop, &abstime);
    }

    link_wait_active.store(false, std::memory_order_relaxed);
    return satisfied;
}

bool PipeWireManager::is_audio_playing() const {
    return audio_playing.load(std::memory_order_relaxed);
}

void PipeWireManager::update_playing_state() {
    bool playing = false;
    for (auto it = node_bindings.cbegin(); it != node_bindings.cend() && !playing; ++it) {
        if (routed_node_ids.contains(it.key()) && it.value()->state == PW_NODE_STATE_RUNNING)
            playing = true;
    }
    audio_playing.store(playing, std::memory_order_relaxed);
}

void PipeWireManager::remove_virtual_surround_module() {
    if (!thread_loop)
        return;

    pw_thread_loop_lock(thread_loop);

    // Removing a sink while streams are still linked to it makes WirePlumber pause
    // their MPRIS players. Callers clear the routing first; give WirePlumber a
    // moment to actually move the streams before the bridge sink disappears.
    if (is_registry_listener_added && bridge_capture_id != SPA_ID_INVALID)
        wait_for([this]() { return !has_input_link(bridge_capture_id); }, 500);

    clear_effects_control_locked();
    destroy_module_locked(bridge_module);
    destroy_module_locked(effects_module);
    // Any graph whose teardown was deferred must also go away now
    destroy_pending_effects_locked();

    bridge_capture_id = SPA_ID_INVALID;
    bridge_playback_id = SPA_ID_INVALID;
    effects_capture_id = SPA_ID_INVALID;
    effects_playback_id = SPA_ID_INVALID;
    internal_playback_ids.clear();
    internal_capture_ids.clear();
    effects_capture_node_name.clear();
    effects_playback_node_name.clear();
    effects_generation = 0;
    audio_playing.store(false, std::memory_order_relaxed);

    // Wait until the server has actually destroyed the module's nodes,
    // recreating it too fast otherwise makes the audio stop completely
    sync_with_core(2);

    pw_thread_loop_unlock(thread_loop);

    qInfo("Removed virtual surround device");
}

void PipeWireManager::enable_routing() {
    // Do nothing if registry listener already exists
    if (is_registry_listener_added)
        return;
    if (!is_valid()) {
        qWarning("enable_routing: Not connected to PipeWire");
        return;
    }

    pw_thread_loop_lock(thread_loop);

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    routing_stopping = false;
    is_registry_listener_added = true;
    spa_zero(registry_listener);
    pw_registry_add_listener(registry, &registry_listener, &registry_events, this);

    pw_thread_loop_unlock(thread_loop);

    qInfo("Enabled routing");
}

void PipeWireManager::disable_routing() {
    // Do nothing if registry listener does not exist
    if (!is_registry_listener_added)
        return;
    if (!thread_loop)
        return;

    pw_thread_loop_lock(thread_loop);
    routing_stopping = true;

    // Take the streams out of the routed set *before* clearing their target. The
    // metadata listener below re-routes a routed stream whose target is removed,
    // which would otherwise undo this very write and keep the stream on the bridge
    // sink until it is destroyed (WirePlumber then pauses MPRIS players).
    const QList<uint32_t> cleared_ids = routed_node_ids.values();
    routed_node_ids.clear();

    for (uint32_t id : cleared_ids) {
        qDebug("disable_routing: clearing target.object for node %u", id);
        if (metadata) {
            // Remove the target object property from the node
            int ret = pw_metadata_set_property(metadata, id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", nullptr);
            qDebug("disable_routing: pw_metadata_set_property returned %d for node %u", ret, id);
        } else {
            qDebug("disable_routing: metadata is nullptr, skipping node %u", id);
        }
    }

    // Keep the registry listener alive until WirePlumber has moved the streams off
    // the bridge sink. Removing the sink while they are still linked makes
    // WirePlumber pause their MPRIS players.
    if (bridge_capture_id != SPA_ID_INVALID) {
        sync_with_core(1);
        // Long enough for WirePlumber's link bookkeeping under load; exiting a
        // little later is much better than removing the sink while a stream is
        // still linked, which makes mpris-pause.lua pause the player.
        if (!wait_for([this]() { return !has_input_link(bridge_capture_id); }, 1500))
            qWarning("disable_routing: Streams did not leave the bridge sink in time");
    }

    spa_hook_remove(&registry_listener);
    clear_effects_control_locked();
    if (metadata) {
        spa_hook_remove(&metadata_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata));
        metadata = nullptr;
    }
    metadata_global_id = SPA_ID_INVALID;

    for (auto it = node_bindings.begin(); it != node_bindings.end(); ++it)
        destroy_binding(it.value());
    node_bindings.clear();
    for (auto it = sink_bindings.begin(); it != sink_bindings.end(); ++it) {
        spa_hook_remove(&it.value()->listener);
        pw_proxy_destroy(it.value()->proxy);
        delete it.value();
    }
    sink_bindings.clear();
    metadata_targets.clear();
    effects_redirect_ms.clear();
    sinks.clear();
    decided_streams.clear();
    links.clear();
    internal_playback_ids.clear();
    internal_capture_ids.clear();
    bridge_capture_id = SPA_ID_INVALID;
    bridge_playback_id = SPA_ID_INVALID;
    effects_capture_id = SPA_ID_INVALID;
    effects_playback_id = SPA_ID_INVALID;
    audio_playing.store(false, std::memory_order_relaxed);

    // The registry is re-created on the next enable_routing()
    if (registry) {
        pw_proxy_destroy((pw_proxy *)registry);
        registry = nullptr;
    }

    is_registry_listener_added = false;

    pw_thread_loop_unlock(thread_loop);

    qDebug("Disabled routing");
}

bool PipeWireManager::is_valid() const {
    return connected.load(std::memory_order_acquire);
}

void PipeWireManager::set_lfe_mode(LfeMode mode) {
    lfe_mode = mode;
}

void PipeWireManager::set_lfe_options(LfeLowpass lowpass, LfeTimbre timbre, bool per_ear_alignment) {
    lfe_lowpass = lowpass;
    lfe_timbre = timbre;
    lfe_per_ear_alignment = per_ear_alignment;
}

int PipeWireManager::upmix_auto_state() const {
    if (!upmix_state)
        return -1;
    return upmix_state->load(std::memory_order_relaxed);
}

PipeWireManager::HrirAnalysis PipeWireManager::analyze_hrir(const string &path) const {
    HrirAnalysis result;
    if (path.empty())
        return result;

    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(path), wav)) return result;
    result.valid = true;
    result.source_rate = wav.rate;
    result.resampled = wav.rate != 48000;
    result.timbre = lfe_timbre;
    result.reference_gain = lfe_reference_gain(path);
    result.reference_gain_db = result.reference_gain > 0 ? 20.0 * std::log10(result.reference_gain) : 0.0;
    const LfeAlignment alignment =
        lfe_alignment_configured(path, 48000, lfe_lowpass, lfe_timbre, lfe_per_ear_alignment);
    // The analysis reports only the alignment delays actually applied by the
    // mainAlign/nativeDelay nodes.
    const double shared_delay_ms = 0.0;
    result.main_delay_ms = shared_delay_ms + (alignment.valid ? alignment.main_delay_seconds * 1000.0 : 0.0);
    result.alignment_ms = result.main_delay_ms;
    result.per_ear_alignment = lfe_per_ear_alignment;
    result.per_ear_values_valid = alignment.valid;
    result.lfe_delay_l_ms = shared_delay_ms + (alignment.valid ? alignment.lfe_delay_l_seconds * 1000.0 : 0.0);
    result.lfe_delay_r_ms = shared_delay_ms + (alignment.valid ? alignment.lfe_delay_r_seconds * 1000.0 : 0.0);
    result.average_residual_l_degrees = alignment.average_residual_l_degrees;
    result.average_residual_r_degrees = alignment.average_residual_r_degrees;
    result.worst_channel_residual_l_degrees = alignment.worst_channel_residual_l_degrees;
    result.worst_channel_residual_r_degrees = alignment.worst_channel_residual_r_degrees;
    result.allpass_l_valid = alignment.allpass_l_valid;
    result.allpass_l_freq = alignment.allpass_l_freq;
    result.allpass_l_q = alignment.allpass_l_q;
    result.allpass_l_residual_degrees = alignment.allpass_l_residual_degrees;
    result.allpass_l_worst_channel_degrees = alignment.allpass_l_worst_channel_degrees;
    result.allpass_r_valid = alignment.allpass_r_valid;
    result.allpass_r_freq = alignment.allpass_r_freq;
    result.allpass_r_q = alignment.allpass_r_q;
    result.allpass_r_residual_degrees = alignment.allpass_r_residual_degrees;
    result.allpass_r_worst_channel_degrees = alignment.allpass_r_worst_channel_degrees;
    result.level_db = hrir_level_db(path);
    return result;
}

double PipeWireManager::hrir_level_db(const string &path) const {
    HrirWav wav;
    if (!HrirWav::read(QString::fromStdString(path), wav))
        return -1000.0;
    const auto rate = wav.rate;

    // Reference model: seven mutually uncorrelated, equal-power pink-noise
    // sources, averaged across ears. Sum powers, not complex responses: the
    // latter measures an arbitrary coherent mix and changes with polarity/ITD.
    // Log-frequency integration is the 1/f PSD of this reference, not LUFS.
    const double upper = std::min(20000.0, rate * 0.5);
    if (upper <= 20.0)
        return -1000.0;
    constexpr int bands = 240; // ~24 points/octave over 20 Hz..20 kHz
    const double step = std::log(upper / 20.0) / bands;
    double energy = 0.0, weight = 0.0;
    for (int band = 0; band < bands; ++band) {
        const double frequency = 20.0 * std::exp((band + 0.5) * step);
        const auto response = hrir_response(wav, frequency);
        double power = 0.0;
        for (const auto channel : response)
            power += std::norm(channel) / response.size();
        const double a = Acoustics::a_weight_power(frequency);
        energy += power * a;
        weight += a;
    }
    if (!std::isfinite(energy) || energy <= 0.0 || weight <= 0.0)
        return -1000.0;
    return 10.0 * std::log10(energy / weight);
}

uint64_t PipeWireManager::upmix_block_count() const {
    if (!upmix_blocks)
        return 0;
    return upmix_blocks->load(std::memory_order_relaxed);
}

bool PipeWireManager::is_speaker_bypass() const {
    return speaker_bypass.load(std::memory_order_relaxed);
}

void PipeWireManager::on_core_done(void *data, [[maybe_unused]] uint32_t id, int seq) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    if (seq == manager->sync_seq) {
        manager->sync_done = true;
        pw_thread_loop_signal(manager->thread_loop, false);
    }
}

void PipeWireManager::on_core_error(void *data, uint32_t id, [[maybe_unused]] int seq, int res, const char *message) {
    PipeWireManager *manager = static_cast<PipeWireManager *>(data);

    qWarning("on_core_error: id %u, res %d (%s)", id, res, message ? message : "?");

    // Only surface core-level failures (e.g. a lost connection); errors for
    // individual objects are usually benign races with disappearing nodes
    if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET || res == -ENOTCONN ||
                             res == -ECONNABORTED || res == -ESHUTDOWN)) {
        const bool was_connected = manager->connected.exchange(false, std::memory_order_acq_rel);
        manager->audio_playing.store(false, std::memory_order_relaxed);
        ++manager->output_gate_generation;
        pw_thread_loop_signal(manager->thread_loop, false);
        Q_EMIT manager->error_occurred(QString::asprintf("PipeWire error: %s", message && message[0] ? message : strerror(-res)));
        if (was_connected)
            Q_EMIT manager->connection_lost();
    }
}

bool PipeWireManager::sync_with_core(int timeout_sec) {
    if (!core || !thread_loop || !is_valid())
        return false;

    sync_done = false;
    sync_seq = pw_core_sync(core, PW_ID_CORE, sync_seq + 1);
    if (sync_seq < 0)
        return false;

    struct timespec abstime;
    pw_thread_loop_get_time(thread_loop, &abstime, static_cast<int64_t>(timeout_sec) * SPA_NSEC_PER_SEC);
    while (!sync_done && is_valid()) {
        if (pw_thread_loop_timed_wait_full(thread_loop, &abstime) != 0) {
            qWarning("sync_with_core: Timed out after %d seconds", timeout_sec);
            return false;
        }
    }
    return sync_done && is_valid();
}

bool PipeWireManager::ensure_connection() {
    if (is_valid())
        return true;
    if (!thread_loop || !context || shutting_down.load())
        return false;
    disable_routing();
    remove_virtual_surround_module();
    pw_thread_loop_lock(thread_loop);
    if (core) {
        spa_hook_remove(&core_listener);
        pw_core_disconnect(core);
        core = nullptr;
    }
    core = pw_context_connect(context, nullptr, 0);
    if (core) {
        spa_zero(core_listener);
        pw_core_add_listener(core, &core_listener, &core_events, this);
        if (!loop_started)
            loop_started = pw_thread_loop_start(thread_loop) >= 0;
        connected.store(loop_started, std::memory_order_release);
        if (loop_started && !sync_with_core(1))
            connected.store(false, std::memory_order_release);
    }
    const bool ready = is_valid();
    pw_thread_loop_unlock(thread_loop);
    return ready;
}

PipeWireManager::PipeWireManager() {
    // Initialize PipeWire library
    pw_init(nullptr, nullptr);

    // Debug: Output current version of PipeWire library
    qInfo("Compiled with libpipewire %s\n"
          "Linked with libpipewire %s\n",
          pw_get_headers_version(), pw_get_library_version());

    // Errors emitted as a signal here are lost: no slots are connected yet during
    // construction. The frontend queries is_valid() instead and shows its own message.
    const auto fail = [](const char *message) {
        qWarning("%s", message);
    };

    // Create some initial stuff for PipeWire to work
    thread_loop = pw_thread_loop_new("main", nullptr);
    if (!thread_loop) {
        fail("Failed to create PipeWire thread loop");
        return;
    }

    pw_properties *props = pw_properties_new(nullptr, nullptr);
    if (!props) {
        fail("Failed to create PipeWire properties");
        return;
    }
    pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Music");

    // Required for Flatpak, otherwise it cannot write PipeWire metadata! Also nice to have for native packaging
    pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Manager");

    context = pw_context_new(pw_thread_loop_get_loop(thread_loop), props, 0);
    if (!context) {
        fail("Failed to create PipeWire context");
        return;
    }

    // Always lock before touching loop objects from outside the thread, then unlock!
    pw_thread_loop_lock(thread_loop);

    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        pw_thread_loop_unlock(thread_loop);
        fail("Failed to connect to PipeWire daemon");
        return;
    }

    // Listen for core roundtrip acknowledgements (see sync_with_core)
    spa_zero(core_listener);
    pw_core_add_listener(core, &core_listener, &core_events, this);

    if (pw_thread_loop_start(thread_loop) < 0) {
        pw_thread_loop_unlock(thread_loop);
        fail("Failed to start the PipeWire thread loop");
        return;
    }

    loop_started = true;
    connected.store(true, std::memory_order_release);
    if (!sync_with_core(1))
        connected.store(false, std::memory_order_release);
    pw_thread_loop_unlock(thread_loop);
}

PipeWireManager::~PipeWireManager() {
    shutting_down = true;
    // Release the routed streams first (disable_routing waits until they have left
    // the bridge sink), then tear the device down
    const bool was_playing = is_audio_playing();
    disable_routing();
    if (was_playing) {
        // WirePlumber's MPRIS link bookkeeping is asynchronous. Removing the
        // bridge immediately after its last input link disappears can still make
        // mpris-pause.lua pause the player, so give the server a moment to settle.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
    remove_virtual_surround_module();

    // Drop the extra reference taken for reading the detector state
    if (upmix_state_handle) {
        dlclose(upmix_state_handle);
        upmix_state_handle = nullptr;
        upmix_state = nullptr;
        upmix_blocks = nullptr;
    }

    if (thread_loop) {
        pw_thread_loop_lock(thread_loop);

        // Destroy the metadata proxy while holding the lock: its listener can
        // still be dispatched on the PipeWire thread until the loop is stopped
        if (metadata) {
            spa_hook_remove(&metadata_listener);
            pw_proxy_destroy((pw_proxy *)metadata);
            metadata = nullptr;
        }

        // Make sure the server has processed the target clears above before the
        // connection goes away
        sync_with_core(2);

        pw_thread_loop_unlock(thread_loop);
    }

    // Clean up PipeWire stuff, tolerating a failed construction
    if (thread_loop)
        pw_thread_loop_stop(thread_loop);
    if (core)
        pw_core_disconnect(core);
    if (context)
        pw_context_destroy(context);
    if (thread_loop)
        pw_thread_loop_destroy(thread_loop);
    pw_deinit();
}
