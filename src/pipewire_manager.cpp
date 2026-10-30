#include "pipewire_manager.h"

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
#include <cstdlib>
#include <chrono>
#include <complex>
#include <cstring>
#include <dlfcn.h>
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
    string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

//
// Parses a RIFF/WAVE header far enough to learn the frame count and format of
// the impulse response. Returns false on anything this minimal parser does not
// understand (only plain PCM / IEEE float files are accepted, which covers all
// HeSuvi-style HRIRs).
//
static bool parse_wav_format(const string &path, uint32_t &channels, uint32_t &rate,
                             uint32_t &block_align, uint16_t &bits, uint16_t &format_tag,
                             uint32_t &frames, size_t &data_offset) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();

    if (data.size() < 12 || memcmp(data.constData(), "RIFF", 4) != 0 ||
        memcmp(data.constData() + 8, "WAVE", 4) != 0)
        return false;

    bool have_fmt = false;
    format_tag = 0;
    size_t offset = 12;
    while (offset + 8 <= static_cast<size_t>(data.size())) {
        const char *id = data.constData() + offset;
        const uint32_t size = qFromLittleEndian<uint32_t>(data.constData() + offset + 4);
        const QByteArray body = data.mid(offset + 8, size);

        if (memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            format_tag = qFromLittleEndian<uint16_t>(body.constData() + 0);
            channels = qFromLittleEndian<uint16_t>(body.constData() + 2);
            rate = qFromLittleEndian<uint32_t>(body.constData() + 4);
            block_align = qFromLittleEndian<uint16_t>(body.constData() + 12);
            bits = qFromLittleEndian<uint16_t>(body.constData() + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format and bits per sample live in
            // the extension (the sub-format GUID starts with the plain format tag)
            if (format_tag == 0xFFFE && size >= 40) {
                bits = qFromLittleEndian<uint16_t>(body.constData() + 18);
                format_tag = qFromLittleEndian<uint16_t>(body.constData() + 24);
            }
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0 && have_fmt) {
            if (block_align == 0 || channels == 0)
                return false;
            frames = size / block_align;
            data_offset = offset + 8;
            // Only plain PCM or float (the formats the convolver accepts anyway)
            return (format_tag == 1 || format_tag == 3) && rate > 0 && frames > 0;
        }
        offset += 8 + size + (size % 2);
    }
    return false;
}

//
// Reads one interleaved sample of the HRIR (14 channels) as a normalized float.
//
static double wav_sample(const QByteArray &data, size_t data_offset, uint32_t block_align,
                         uint16_t format_tag, uint16_t bits, uint32_t frame, uint32_t channel) {
    const uint32_t frame_size = bits / 8;
    const char *p = data.constData() + data_offset + static_cast<size_t>(frame) * block_align +
                    static_cast<size_t>(channel) * frame_size;
    if (format_tag == 3 && bits == 32) {
        float v = 0.0f;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    if (format_tag == 1) {
        if (bits == 16)
            return qFromLittleEndian<int16_t>(reinterpret_cast<const uint8_t *>(p)) / 32768.0;
        if (bits == 24) {
            int32_t v = static_cast<uint8_t>(p[0]) |
                        (static_cast<int32_t>(static_cast<uint8_t>(p[1])) << 8) |
                        (static_cast<int32_t>(static_cast<uint8_t>(p[2])) << 16);
            if (v & 0x800000)
                v -= 0x1000000;
            return v / 8388608.0;
        }
        if (bits == 32)
            return qFromLittleEndian<int32_t>(reinterpret_cast<const uint8_t *>(p)) / 2147483648.0;
    }
    return 0.0;
}

//
// The delta impulse is flat across the spectrum, while every HRIR has its own
// low-frequency magnitude. A full-scale delta therefore makes the LFE path's
// level depend on the selected HRIR: measured across the bundled profiles, the
// native LFE was +2.8 to +8.3 dB hotter than the Dolby binaural reference.
// Scale the delta to the center-channel HRIR's 40-80 Hz magnitude so the LFE
// path has the same low-frequency gain as the center speaker (the LFE's
// natural reference in HeSuVi), while keeping the delta itself flat/uncolored.
//
static double lfe_reference_gain(const string &hrir_path) {
    uint32_t channels = 0, rate = 0, block_align = 0, frames = 0;
    uint16_t bits = 0, format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(hrir_path, channels, rate, block_align, bits, format_tag, frames, data_offset))
        return -1.0;
    const bool supported = (format_tag == 3 && bits == 32) ||
                           (format_tag == 1 && (bits == 16 || bits == 24 || bits == 32));
    if (channels != 14 || frames == 0 || !supported)
        return -1.0;

    QFile file(QString::fromStdString(hrir_path));
    if (!file.open(QIODevice::ReadOnly))
        return -1.0;
    const QByteArray data = file.readAll();
    file.close();
    if (data_offset + static_cast<size_t>(frames) * block_align > static_cast<size_t>(data.size()))
        return -1.0;

    // Center HRIR pair (HeSuVi LFE uses these) over a 40-80 Hz grid. A plain DFT
    // is enough: at most ~48000 samples * 10 evaluations for a 1 s HRIR.
    static constexpr double kFreqs[] = {40.0, 50.0, 60.0, 70.0, 80.0};
    double magnitude_sum = 0.0;
    int magnitude_count = 0;
    for (const double f : kFreqs) {
        for (const uint32_t channel : {6u, 13u}) {
            double re = 0.0, im = 0.0;
            for (uint32_t n = 0; n < frames; ++n) {
                const double s = wav_sample(data, data_offset, block_align, format_tag, bits, n, channel);
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
    // Normalized HRIRs have a low-frequency magnitude below unity; the clamp
    // protects the integer delta formats and obviously broken files.
    const double gain = std::clamp(magnitude_sum / magnitude_count, 0.05, 1.0);
    qInfo("lfe_reference_gain: '%s' center HRIR 40-80 Hz gain %.4f (%.2f dB)",
          hrir_path.c_str(), gain, 20.0 * std::log10(gain));
    return gain;
}

struct LfeAlignment {
    bool valid = false;
    bool delay_main = false;
    bool delay_lfe = false;
    double seconds = 0.0;
    double residual_degrees = 0.0;        // linear slope-fit residual
    double average_residual_degrees = 0.0; // absolute average phase error after delay
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

static bool lfe_send_safe(const LfeAlignment &alignment) {
    return alignment.valid && lfe_effective_residual_degrees(alignment) <= 30.0;
}

static bool lfe_impact_allowed(const LfeAlignment &alignment) {
    return alignment.valid && lfe_effective_residual_degrees(alignment) <= 15.0;
}

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

// One ear's (or the common path's) phase fit. Shared by the production
// low-pass/timbre/per-ear path; lfe_alignment() below is retained only as the
// old reference implementation.
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
    for (size_t i = 0; i < kLfeFitCount; ++i)
        diff[i] = diff_in[i];
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
        const double r = diff[i] + 2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau;
        average_sq += r * r;
    }
    const double average_residual = std::sqrt(average_sq / kLfeFitCount);

    double worst = 0.0;
    for (int k = 0; k < n_channels; ++k) {
        double sum = 0.0;
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            double r = std::arg(lfe_resp[i] / channel_resp[channels[k]][i]) +
                       2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau;
            r = std::remainder(r, 2.0 * std::numbers::pi);
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
        double target[kLfeFitCount];
        bool can_lead = false;
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            const double r = diff[i] + 2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau;
            target[i] = -r;
            if (target[i] > 1e-9)
                can_lead = true;
        }
        if (!can_lead) {
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
                        const double err = allpass_phase_radians(kLfeFitFreqs[i], f0, q, graph_rate) - target[i];
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
                        double r = std::arg(lfe_resp[i] / channel_resp[channels[k]][i]) +
                                   2.0 * std::numbers::pi * kLfeFitFreqs[i] * tau +
                                   allpass_phase_radians(kLfeFitFreqs[i], best_freq, best_q, graph_rate);
                        r = std::remainder(r, 2.0 * std::numbers::pi);
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
// Retained as the reference implementation for the old LR4/delta/common path.
// The application now uses lfe_alignment_configured(), so this function is not
// called anymore.
[[maybe_unused]] static LfeAlignment lfe_alignment(const string &hrir_path, uint32_t graph_rate) {
    uint32_t channels = 0, samples = 0, block_align = 0;
    uint32_t source_rate = 0;
    uint16_t bits = 0, format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(hrir_path, channels, source_rate, block_align, bits, format_tag, samples, data_offset))
        return {};
    const bool supported = (format_tag == 3 && bits == 32) ||
                           (format_tag == 1 && (bits == 16 || bits == 24 || bits == 32));
    if (channels != 14 || samples == 0 || !supported || source_rate == 0 || graph_rate == 0)
        return {};

    QFile file(QString::fromStdString(hrir_path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    file.close();
    if (data_offset + static_cast<size_t>(samples) * block_align > static_cast<size_t>(data.size()))
        return {};

    // LR4 coefficients (same as the graph's two bq_lowpass sections). They must
    // use the graph's rate: the bq_lowpass nodes run at 48 kHz even when the IR
    // file is 44.1/96 kHz.
    const double w0 = 2.0 * std::numbers::pi * 120.0 / graph_rate;
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double b0 = (1.0 - cosw0) / 2.0;
    const double b1 = 1.0 - cosw0;
    const double b2 = b0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;

    // 20-120 Hz covers the LFE band that matters for the send. The previous
    // implementation started at 40 Hz; 20-40 Hz is included now so the deep-bass
    // phase is part of the fit, not an unmeasured extrapolation.
    constexpr double freqs[] = {20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 110.0, 120.0};
    constexpr size_t count = std::size(freqs);
    std::complex<double> channel_resp[14][count];
    for (uint32_t channel = 0; channel < 14; ++channel) {
        for (size_t i = 0; i < count; ++i) {
            const double f = freqs[i];
            double re = 0.0;
            double im = 0.0;
            for (uint32_t n = 0; n < samples; ++n) {
                const double sample = wav_sample(data, data_offset, block_align, format_tag, bits, n, channel);
                const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(n) / source_rate;
                re += sample * std::cos(phase);
                im -= sample * std::sin(phase);
            }
            channel_resp[channel][i] = std::complex<double>(re, im);
        }
    }

    double diff[count] = {};
    std::complex<double> lfe_resp[count];
    for (size_t i = 0; i < count; ++i) {
        const double f = freqs[i];
        std::complex<double> left_sum = 0.0;
        std::complex<double> right_sum = 0.0;
        for (uint32_t channel = 0; channel < 14; ++channel) {
            if (channel % 2 == 0)
                left_sum += channel_resp[channel][i];
            else
                right_sum += channel_resp[channel][i];
        }
        const std::complex<double> main_bass = (left_sum + right_sum) / 2.0;
        if (std::abs(main_bass) < 1e-9)
            return {};

        const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * std::numbers::pi * f / graph_rate));
        const std::complex<double> lp = (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
        lfe_resp[i] = lp * lp;
        diff[i] = std::arg(lfe_resp[i] / main_bass);
    }

    // Unwrap before the linear fit
    for (size_t i = 1; i < count; ++i) {
        while (diff[i] - diff[i - 1] > std::numbers::pi)
            diff[i] -= 2.0 * std::numbers::pi;
        while (diff[i] - diff[i - 1] < -std::numbers::pi)
            diff[i] += 2.0 * std::numbers::pi;
    }

    // Equal-weight least-squares slope. The absolute residual after applying the
    // delay is reported separately; slope residual alone was misleadingly small.
    const double mean_f = std::accumulate(std::begin(freqs), std::end(freqs), 0.0) / count;
    const double mean_d = std::accumulate(std::begin(diff), std::end(diff), 0.0) / count;
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < count; ++i) {
        num += (freqs[i] - mean_f) * (diff[i] - mean_d);
        den += (freqs[i] - mean_f) * (freqs[i] - mean_f);
    }
    const double slope = num / den;
    double slope_residual = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double predicted = mean_d + slope * (freqs[i] - mean_f);
        slope_residual += std::pow(diff[i] - predicted, 2);
    }
    slope_residual = std::sqrt(slope_residual / count);

    LfeAlignment result;
    result.valid = true;
    result.source_rate = source_rate;
    result.residual_degrees = slope_residual * 180.0 / std::numbers::pi;

    double tau = -slope / (2.0 * std::numbers::pi);
    if (slope_residual > 0.26) {
        // Phase is not a clean delay; leave the signal untouched rather than
        // inserting a wrong delay/correction.
        tau = 0.0;
    } else if (std::abs(tau) < 0.0002) {
        tau = 0.0;
    } else {
        tau = std::clamp(tau, -0.02, 0.02);
        // Quantize to whole graph samples so the delay node exactly represents
        // the fit (the delay runs at 48 kHz, not at the HRIR file rate).
        tau = std::round(tau * graph_rate) / graph_rate;
    }

    result.seconds = std::abs(tau);
    result.delay_main = tau > 0.0;
    result.delay_lfe = tau < 0.0;

    // Residual phase after the selected delay. For tau < 0 the LFE path is
    // delayed, so the same formula (+2*pi*f*tau) applies: delaying LFE by
    // |tau| subtracts 2*pi*f*|tau| from the LFE-main difference.
    double average_residual_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double r = diff[i] + 2.0 * std::numbers::pi * freqs[i] * tau;
        average_residual_sq += r * r;
    }
    const double average_residual = std::sqrt(average_residual_sq / count);
    result.average_residual_degrees = average_residual * 180.0 / std::numbers::pi;

    double worst_channel = 0.0;
    for (uint32_t channel = 0; channel < 14; ++channel) {
        double sum = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double r = std::arg(lfe_resp[i] / channel_resp[channel][i]) +
                       2.0 * std::numbers::pi * freqs[i] * tau;
            r = std::remainder(r, 2.0 * std::numbers::pi);
            sum += r * r;
        }
        worst_channel = std::max(worst_channel, std::sqrt(sum / count));
    }
    result.worst_channel_residual_degrees = worst_channel * 180.0 / std::numbers::pi;

    // Automatic all-pass correction on the LFE path. One second-order section is
    // enough to remove the residual hump; it has exactly unity magnitude, so the
    // LFE stays uncolored. Only use it when it measurably improves the absolute
    // residual and the target phase is always a lag (a causal all-pass cannot
    // lead).
    if (slope_residual <= 0.26) {
        double target[count];
        bool can_lead = false;
        for (size_t i = 0; i < count; ++i) {
            const double r = diff[i] + 2.0 * std::numbers::pi * freqs[i] * tau;
            target[i] = -r; // correction phase wanted from the all-pass
            if (target[i] > 1e-9)
                can_lead = true;
        }
        if (!can_lead) {
            double best_error = 1e9;
            double best_freq = 0.0;
            double best_q = 0.0;
            constexpr int kFreqSteps = 80;
            constexpr int kQSteps = 40;
            const double f_min = 20.0, f_max = 12000.0;
            const double q_min = 0.1, q_max = 10.0;
            for (int fi = 0; fi < kFreqSteps; ++fi) {
                const double f0 = f_min * std::pow(f_max / f_min, static_cast<double>(fi) / (kFreqSteps - 1));
                for (int qi = 0; qi < kQSteps; ++qi) {
                    const double q = q_min * std::pow(q_max / q_min, static_cast<double>(qi) / (kQSteps - 1));
                    double error = 0.0;
                    for (size_t i = 0; i < count; ++i) {
                        const double err = allpass_phase_radians(freqs[i], f0, q, graph_rate) - target[i];
                        error += err * err;
                    }
                    error = std::sqrt(error / count);
                    if (error < best_error) {
                        best_error = error;
                        best_freq = f0;
                        best_q = q;
                    }
                }
            }
            if (best_error < average_residual - 0.0087) { // at least 0.5 deg RMS
                result.allpass_valid = true;
                result.allpass_freq = best_freq;
                result.allpass_q = best_q;
                result.allpass_residual_degrees = best_error * 180.0 / std::numbers::pi;

                double worst_after = 0.0;
                for (uint32_t channel = 0; channel < 14; ++channel) {
                    double sum = 0.0;
                    for (size_t i = 0; i < count; ++i) {
                        double r = std::arg(lfe_resp[i] / channel_resp[channel][i]) +
                                   2.0 * std::numbers::pi * freqs[i] * tau +
                                   allpass_phase_radians(freqs[i], best_freq, best_q, graph_rate);
                        r = std::remainder(r, 2.0 * std::numbers::pi);
                        sum += r * r;
                    }
                    worst_after = std::max(worst_after, std::sqrt(sum / count));
                }
                result.allpass_worst_channel_degrees = worst_after * 180.0 / std::numbers::pi;
            }
        }
    }

    qInfo("lfe_alignment: '%s' source %u Hz, graph %u Hz, fit %.2f ms, slope-residual %.2f deg, absolute avg %.2f deg, worst channel %.2f deg%s",
          hrir_path.c_str(), source_rate, graph_rate, result.seconds * 1000.0, result.residual_degrees,
          result.average_residual_degrees, result.worst_channel_residual_degrees,
          result.allpass_valid ? " (+ all-pass)" : "");
    if (result.allpass_valid) {
        qInfo("lfe_alignment: all-pass %.1f Hz Q %.3f, avg %.2f deg, worst channel %.2f deg",
              result.allpass_freq, result.allpass_q, result.allpass_residual_degrees,
              result.allpass_worst_channel_degrees);
    }
    return result;
}

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
// Configured LFE alignment: same fit as lfe_alignment(), but with the selected
// low-pass/timbre and optionally separate left/right LFE delays and all-pass
// sections. The main path keeps one common delay (Dmain), while the LFE path can
// be delayed independently per ear (Dlfe = Dmain - tau_ear) so each ear gets its
// own best fit. This is the production path; the old lfe_alignment() below is
// retained only as a reference.
//
static LfeAlignment lfe_alignment_configured(const string &hrir_path, uint32_t graph_rate,
                                               LfeLowpass lowpass, LfeTimbre timbre,
                                               bool per_ear) {
    uint32_t channels = 0, samples = 0, block_align = 0, source_rate = 0;
    uint16_t bits = 0, format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(hrir_path, channels, source_rate, block_align, bits, format_tag, samples, data_offset))
        return {};
    const bool supported = (format_tag == 3 && bits == 32) ||
                           (format_tag == 1 && (bits == 16 || bits == 24 || bits == 32));
    if (channels != 14 || samples == 0 || !supported || source_rate == 0 || graph_rate == 0)
        return {};
    QFile file(QString::fromStdString(hrir_path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    file.close();
    if (data_offset + static_cast<size_t>(samples) * block_align > static_cast<size_t>(data.size()))
        return {};

    std::complex<double> channel_resp[14][kLfeFitCount];
    for (uint32_t channel = 0; channel < 14; ++channel) {
        for (size_t i = 0; i < kLfeFitCount; ++i) {
            const double f = kLfeFitFreqs[i];
            double re = 0.0, im = 0.0;
            for (uint32_t n = 0; n < samples; ++n) {
                const double sample = wav_sample(data, data_offset, block_align, format_tag, bits, n, channel);
                const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(n) / source_rate;
                re += sample * std::cos(phase);
                im -= sample * std::sin(phase);
            }
            channel_resp[channel][i] = std::complex<double>(re, im);
        }
    }

    std::complex<double> left[kLfeFitCount], right[kLfeFitCount];
    for (size_t i = 0; i < kLfeFitCount; ++i) {
        left[i] = 0.0;
        right[i] = 0.0;
        for (uint32_t channel = 0; channel < 14; ++channel)
            (channel % 2 == 0 ? left[i] : right[i]) += channel_resp[channel][i];
        if (std::abs(left[i]) < 1e-9 || std::abs(right[i]) < 1e-9)
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
        if (diffuse_count == 0 || diffuse_gain <= 1e-9)
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
            diff[i] = std::arg(lfe_avg[i] / main_avg);
        }
        const PhaseFit fit = fit_phase(diff, channel_resp, all_channels, 14, lfe_avg, graph_rate);
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
        diff_l[i] = std::arg(lfe_l[i] / left[i]);
        diff_r[i] = std::arg(lfe_r[i] / right[i]);
    }
    const PhaseFit fit_l = fit_phase(diff_l, channel_resp, even_channels, 7, lfe_l, graph_rate);
    const PhaseFit fit_r = fit_phase(diff_r, channel_resp, odd_channels, 7, lfe_r, graph_rate);

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
static bool write_lfe_bypass_delta(const string &hrir_path, const string &delta_path) {
    uint32_t channels = 0, rate = 0, block_align = 0, frames = 0;
    uint16_t bits = 0;
    uint16_t format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(hrir_path, channels, rate, block_align, bits, format_tag, frames, data_offset))
        return false;

    // The graph maps all 14 HeSuVi channels; a file with fewer channels would
    // silently produce a wrong spatial image (PipeWire tolerates out-of-range
    // channel indices), so reject it before the graph is built.
    if (channels != 14) {
        qWarning("write_lfe_bypass_delta: '%s' has %u channels, expected 14", hrir_path.c_str(), channels);
        return false;
    }

    const double delta_gain = lfe_reference_gain(hrir_path);
    if (delta_gain <= 0.0)
        return false;

    // Keep a sane cap for a broken header, but do not truncate a valid long HRIR:
    // the delta IR length has to match the main IR length for latency alignment.
    if (frames > rate * 10) {
        qWarning("write_lfe_bypass_delta: '%s' is longer than 10 s, truncating", hrir_path.c_str());
        frames = rate * 10;
    }

    // Mono file, same encoding as the source IR
    const uint32_t frame_size = bits / 8;
    const uint32_t data_size = frames * frame_size;

    QFile file(QString::fromStdString(delta_path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    auto le32 = [](uint32_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](uint16_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };

    // Same encoding as the source IR. Writing every file as IEEE float (the old
    // behaviour) made 16/24-bit PCM HRIRs produce a delta whose header lied about
    // its samples; PipeWire then failed to start the whole effects graph, leaving
    // the output silent until the next switch.
    const bool is_float = format_tag == 3;
    if (is_float && bits != 32)
        return false;

    QByteArray fmt;
    fmt += le16(format_tag); // 1 = PCM, 3 = IEEE float
    fmt += le16(1);          // mono
    fmt += le32(rate);
    fmt += le32(rate * frame_size);
    fmt += le16(frame_size);
    fmt += le16(bits);

    QByteArray header;
    header += QByteArray("RIFF", 4);
    header += le32(4 + 8 + 16 + 8 + data_size);
    header += QByteArray("WAVE", 4);
    header += QByteArray("fmt ", 4);
    header += le32(16);
    header += fmt;
    header += QByteArray("data", 4);
    header += le32(data_size);

    QByteArray samples(data_size, '\0');
    if (is_float) {
        const float gain = static_cast<float>(delta_gain);
        memcpy(samples.data(), &gain, 4);
    } else if (bits == 16) {
        const int16_t gain = static_cast<int16_t>(std::lround(32767.0 * delta_gain));
        memcpy(samples.data(), &gain, 2);
    } else if (bits == 24) {
        const int32_t gain = static_cast<int32_t>(std::lround(8388607.0 * delta_gain));
        const uint8_t bytes[3] = {static_cast<uint8_t>(gain & 0xFF),
                                  static_cast<uint8_t>((gain >> 8) & 0xFF),
                                  static_cast<uint8_t>((gain >> 16) & 0xFF)};
        memcpy(samples.data(), bytes, 3);
    } else if (bits == 32) {
        const int32_t gain = static_cast<int32_t>(std::lround(2147483647.0 * delta_gain));
        memcpy(samples.data(), &gain, 4);
    } else {
        return false;
    }

    const bool ok = file.write(header) == header.size() && file.write(samples) == samples.size();
    file.close();
    return ok;
}

//
// Experimental diffuse-field LFE IR: the average of the seven left/right HRIRs,
// normalized to the center pair's 40-80 Hz magnitude (the same reference as the
// delta IR). The result is a 2-channel WAV at the source rate/format/length; the
// LFE path convolves L with channel 0 and R with channel 1.
//
static bool write_lfe_diffuse_ir(const string &hrir_path, const string &diffuse_path) {
    uint32_t channels = 0, rate = 0, block_align = 0, frames = 0;
    uint16_t bits = 0, format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(hrir_path, channels, rate, block_align, bits, format_tag, frames, data_offset))
        return false;
    const bool supported = (format_tag == 3 && bits == 32) ||
                           (format_tag == 1 && (bits == 16 || bits == 24 || bits == 32));
    if (channels != 14 || frames == 0 || rate == 0 || !supported)
        return false;
    if (frames > rate * 10)
        frames = rate * 10;

    QFile file(QString::fromStdString(hrir_path));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();
    if (data_offset + static_cast<size_t>(frames) * block_align > static_cast<size_t>(data.size()))
        return false;

    std::vector<double> left(frames), right(frames);
    for (uint32_t n = 0; n < frames; ++n) {
        double l = 0.0, r = 0.0;
        for (uint32_t channel = 0; channel < 14; ++channel) {
            const double s = wav_sample(data, data_offset, block_align, format_tag, bits, n, channel);
            (channel % 2 == 0 ? l : r) += s;
        }
        left[n] = l / 7.0;
        right[n] = r / 7.0;
    }

    const double center_gain = lfe_reference_gain(hrir_path);
    if (center_gain <= 0.0)
        return false;
    double diffuse_gain = 0.0;
    int diffuse_count = 0;
    for (const double f : {40.0, 50.0, 60.0, 70.0, 80.0}) {
        double re_l = 0.0, im_l = 0.0, re_r = 0.0, im_r = 0.0;
        const double w = 2.0 * std::numbers::pi * f / static_cast<double>(rate);
        for (uint32_t n = 0; n < frames; ++n) {
            const double phase = w * static_cast<double>(n);
            const double c = std::cos(phase);
            const double s = std::sin(phase);
            re_l += left[n] * c;
            im_l -= left[n] * s;
            re_r += right[n] * c;
            im_r -= right[n] * s;
        }
        diffuse_gain += (std::sqrt(re_l * re_l + im_l * im_l) +
                         std::sqrt(re_r * re_r + im_r * im_r)) / 2.0;
        ++diffuse_count;
    }
    if (diffuse_count == 0 || diffuse_gain <= 1e-9)
        return false;
    const double scale = center_gain / (diffuse_gain / diffuse_count);

    const uint32_t frame_size = bits / 8;
    const uint32_t data_size = frames * 2 * frame_size;
    QFile out(QString::fromStdString(diffuse_path));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    auto le32 = [](uint32_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](uint16_t v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    const bool is_float = format_tag == 3;
    if (is_float && bits != 32)
        return false;

    QByteArray fmt;
    fmt += le16(format_tag);
    fmt += le16(2);
    fmt += le32(rate);
    fmt += le32(rate * 2 * frame_size);
    fmt += le16(2 * frame_size);
    fmt += le16(bits);

    QByteArray header;
    header += QByteArray("RIFF", 4);
    header += le32(4 + 8 + 16 + 8 + data_size);
    header += QByteArray("WAVE", 4);
    header += QByteArray("fmt ", 4);
    header += le32(16);
    header += fmt;
    header += QByteArray("data", 4);
    header += le32(data_size);

    QByteArray samples;
    samples.reserve(data_size);
    auto append_sample = [&](double v) {
        v = std::clamp(v, -1.0, 1.0);
        if (is_float) {
            const float f = static_cast<float>(v);
            samples.append(reinterpret_cast<const char *>(&f), 4);
        } else if (bits == 16) {
            const int16_t s = static_cast<int16_t>(std::lround(32767.0 * v));
            samples.append(reinterpret_cast<const char *>(&s), 2);
        } else if (bits == 24) {
            const int32_t s = static_cast<int32_t>(std::lround(8388607.0 * v));
            const uint8_t bytes[3] = {static_cast<uint8_t>(s & 0xFF),
                                      static_cast<uint8_t>((s >> 8) & 0xFF),
                                      static_cast<uint8_t>((s >> 16) & 0xFF)};
            samples.append(reinterpret_cast<const char *>(bytes), 3);
        } else if (bits == 32) {
            const int32_t s = static_cast<int32_t>(std::lround(2147483647.0 * v));
            samples.append(reinterpret_cast<const char *>(&s), 4);
        }
    };
    for (uint32_t n = 0; n < frames; ++n) {
        append_sample(left[n] * scale);
        append_sample(right[n] * scale);
    }

    const bool ok = out.write(header) == header.size() && out.write(samples) == samples.size();
    out.close();
    return ok;
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

    for (const QString &candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

string PipeWireManager::parse_default_sink_name(const char *value) {
    if (!value)
        return {};
    const string json(value);
    const size_t key = json.find("\"name\"");
    if (key == string::npos)
        return json;
    const size_t colon = json.find(':', key);
    if (colon == string::npos)
        return {};
    const size_t start = json.find('"', colon);
    if (start == string::npos)
        return {};
    const size_t end = json.find('"', start + 1);
    if (end == string::npos)
        return {};
    return json.substr(start + 1, end - start - 1);
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

void PipeWireManager::write_stream_mute(struct pw_node *node, bool mute) {
    if (!node)
        return;
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    // The value type comes from the vararg protocol as a *string* tag ("b" for
    // bool), not as a SPA_TYPE_* id (see AGENTS.md §5).
    const struct spa_pod *pod = reinterpret_cast<const struct spa_pod *>(
        spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                                   SPA_POD_Prop(SPA_PROP_mute, SPA_POD_Bool(mute))));
    pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
}

void PipeWireManager::write_stream_volume(struct pw_node *node, float volume) {
    if (!node)
        return;
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *pod = reinterpret_cast<const struct spa_pod *>(
        spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                                   SPA_POD_Prop(SPA_PROP_volume, SPA_POD_Float(volume))));
    pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
}

bool PipeWireManager::gate_effects_output_locked() {
    if (!effects_playback_control_proxy || !graph_upmix_enabled.load(std::memory_order_relaxed))
        return false;
    pw_proxy *proxy = effects_playback_control_proxy;
    write_stream_mute(reinterpret_cast<struct pw_node *>(proxy), true);
    const uint64_t generation = ++output_gate_generation;
    qInfo("gate_effects_output: muted for 150 ms (generation %llu)", static_cast<unsigned long long>(generation));
    QTimer::singleShot(150, this, [this, proxy, generation]() {
        if (!thread_loop)
            return;
        pw_thread_loop_lock(thread_loop);
        if (generation != output_gate_generation.load(std::memory_order_relaxed) ||
            effects_playback_control_proxy != proxy) {
            pw_thread_loop_unlock(thread_loop);
            return;
        }
        // Unmute at zero gain, then ramp up over ~100 ms. A hard unmute at a
        // non-zero sample is audible as a click (measured 0.135 step with a
        // continuous tone); the ramp keeps the track start smooth.
        write_stream_volume(reinterpret_cast<struct pw_node *>(proxy), 0.0f);
        write_stream_mute(reinterpret_cast<struct pw_node *>(proxy), false);
        qInfo("gate_effects_output: fade-in start (generation %llu)",
              static_cast<unsigned long long>(generation));
        pw_thread_loop_unlock(thread_loop);
        for (int step = 1; step <= 20; ++step) {
            QTimer::singleShot(step * 5, this, [this, proxy, generation, step]() {
                if (!thread_loop)
                    return;
                pw_thread_loop_lock(thread_loop);
                if (generation == output_gate_generation.load(std::memory_order_relaxed) &&
                    effects_playback_control_proxy == proxy) {
                    write_stream_volume(reinterpret_cast<struct pw_node *>(proxy),
                                        static_cast<float>(step) / 20.0f);
                }
                pw_thread_loop_unlock(thread_loop);
            });
        }
    });
    return true;
}

void PipeWireManager::fade_in_new_stream(uint32_t id) {
    auto it = node_bindings.find(id);
    if (it == node_bindings.end() || !it.value()->proxy)
        return;
    // While upmixing, the post-graph gate is the reliable place to suppress a
    // new stream: the first samples may already be inside the 128 ms FreeSurround
    // delay line before we discover the stream, so muting the source cannot
    // remove them (measured). The gate also covers the previous track's delay
    // line. Only fall back to the source mute when no gate is available.
    if (gate_effects_output_locked()) {
        qInfo("fade_in_new_stream: Node %u output-gated for 150 ms", id);
        return;
    }
    write_stream_mute(reinterpret_cast<struct pw_node *>(it.value()->proxy), true);
    qInfo("fade_in_new_stream: Node %u muted for 30 ms", id);
    QTimer::singleShot(30, this, [this, id]() {
        if (!thread_loop)
            return;
        pw_thread_loop_lock(thread_loop);
        auto binding = node_bindings.find(id);
        if (binding != node_bindings.end() && routed_node_ids.contains(id) && binding.value()->proxy)
            write_stream_mute(reinterpret_cast<struct pw_node *>(binding.value()->proxy), false);
        pw_thread_loop_unlock(thread_loop);
    });
}

void PipeWireManager::route_default_streams() {
    if (!metadata || speaker_bypass.load(std::memory_order_relaxed))
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
    if (!metadata || decided_streams.contains(id))
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

    if (!info || !info->props)
        return;

    // Track the state so the application knows whether audio is actually playing
    binding->state = info->state;

    const char *target = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT);
    if (!target)
        target = spa_dict_lookup(info->props, "target.node");
    binding->props_target = (target && strlen(target) > 0) ? target : "";
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

    if (!key)
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
            if (manager->effects_playback_control_proxy) {
                pw_proxy_destroy(manager->effects_playback_control_proxy);
                manager->effects_playback_control_proxy = nullptr;
            }
            manager->effects_playback_control_proxy =
                static_cast<pw_proxy *>(pw_registry_bind(manager->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
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
    if (manager->effects_playback_id == id) {
        if (manager->effects_playback_control_proxy) {
            pw_proxy_destroy(manager->effects_playback_control_proxy);
            manager->effects_playback_control_proxy = nullptr;
        }
    }
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

    // The path is interpolated into the JSON module arguments, so escape it
    const string hrir_path = json_escape(hrir_wav_path);

    // LFE impulse response. Default = the normalized diffuse-field pair; the
    // options are the uncolored delta or the center HRIR pair. All keep the same
    // IR length as the selected HRIR (see write_lfe_bypass_delta /
    // write_lfe_diffuse_ir). Only the delta is strictly uncolored and equal-L/R;
    // the diffuse and center variants deliberately apply HRIR-derived timbre.
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

    // Align the LFE/send and main HRIR bass per preset. The production path uses
    // the configured low-pass/timbre and per-ear LFE delays/all-pass; the old
    // lfe_alignment() is retained only as a reference implementation.
    const LfeAlignment alignment = lfe_alignment_configured(hrir_wav_path, 48000, lfe_lowpass,
                                                             lfe_timbre, lfe_per_ear_alignment);

    // The FreeSurround upmixer is only part of the graph while upmixing is enabled;
    // without it the graph inputs are the copy nodes directly and the bundled
    // LADSPA plugin is not needed at all.
    string upmix_nodes;
    string upmix_links;
    string graph_inputs = R"(  [ "copyFL:In" "copyFR:In" "copyFC:In" "copyLFE:In" "copyRL:In" "copyRR:In" "copySL:In" "copySR:In" ])";

    // Low-frequency handling (2026-10-28; defaults 2026-10-30). The LFE channel
    // is rendered through the selected LFE timbre IR (diffuse-field by default).
    // The 120 Hz BW4 in that path has several ms of group delay at 40-120 Hz,
    // while every HRIR has its own low-frequency delay; lfe_alignment_configured()
    // compensates the difference with a delay on the main or LFE path. Without it the send and main bass
    // partially cancel above 90 Hz (measured -95..-171 degrees), which is why a
    // louder send sounded worse instead of stronger.
    // - Balanced: mono sum of the seven full-range main channels, -6 dB into the
    //   LFE path while upmixing; native LFE direct. With alignment this adds
    //   density without the old cancellation.
    // - Reference: native LFE only, no send; most faithful.
    // - Impact: the same aligned send at 0 dB instead of -6 dB (6 dB more,
    //   i.e. 2x amplitude). It adds density and punch without the muddy broad
    //   shelf of the previous implementation.
    // A send is only safe when the post-alignment phase residual is small. The
    // old code gated only the impact level and always added -6 dB for balanced;
    // for presets without a usable delay fit that send can cancel the main bass
    // (measured -4.6 dB at 40 Hz for AtmosRenderer-Far and -3.5 dB at 100 Hz for
    // dtshx). Disable the send entirely in that case; impact additionally needs
    // the tighter 15 deg bound for its 0 dB level.
    const bool send_safe = lfe_send_safe(alignment);
    const bool bass_aligned = lfe_impact_allowed(alignment);
    const bool lfe_send = upmix_enabled && lfe_mode != LfeMode::Reference && send_safe;
    const string use_lfe = "0.0";
    const double send_db = (lfe_mode == LfeMode::Impact && bass_aligned) ? 0.0 : -6.0;
    const string send_gain_text = std::to_string(lfe_send ? std::pow(10.0, send_db / 20.0) : 0.0);
    if (upmix_enabled && lfe_mode != LfeMode::Reference && !send_safe) {
        qInfo("build_effects_module_args: LFE send disabled for this HRIR "
              "(post-alignment residual %.1f deg > 30 deg)",
              lfe_effective_residual_degrees(alignment));
    }
    // The LFE output of the auto plugin carries either the native LFE (bypass
    // branch) or the stereo upmix branch's mono main-sum send. There is no
    // separate graph mixer: adding a send outside the plugin would also add it
    // to real multichannel content, doubling that content's bass.
    // Low-pass topology: fixed 120 Hz Butterworth-4 (24 dB/oct). The enum keeps
    // the older LR4/8th-order shapes for the reference path.
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

    string alignment_nodes;
    string lfe_source_link = R"({ output = "copyLFE:Out" input=")" + lpf_first + R"(:In" })";
    string lfe_align_links;
    string lfe_phase_nodes;
    string lfe_phase_links;
    if (!alignment.per_ear) {
        if (alignment.delay_lfe) {
            lfe_source_link = R"({ output = "copyLFE:Out" input="delayLFE:In" })";
            lfe_align_links = R"(
        { output = "delayLFE:Out" input=")" + lpf_first + R"(:In" }
)";
            alignment_nodes += "\n        { type = builtin label = delay name = delayLFE config = { \"max-delay\" = 0.02 } control = { \"Delay (s)\" = " +
                               std::to_string(alignment.seconds) + " } }\n";
        }
        if (alignment.allpass_valid) {
            lfe_phase_nodes = R"(
        # Automatic all-pass phase correction for the LFE path (per HRIR).
        # Unity magnitude; only the low-frequency phase is rotated.
        { type = builtin label = bq_allpass name = apLFE control = { "Freq" = )" + std::to_string(alignment.allpass_freq) + R"( "Q" = )" + std::to_string(alignment.allpass_q) + R"( } }
)";
            lfe_phase_links = R"(
        { output = ")" + lpf_last + R"(:Out" input="apLFE:In" }
        { output = "apLFE:Out" input="gainLFE_L:In" }
        { output = "apLFE:Out" input="gainLFE_R:In" }
)";
        } else {
            lfe_phase_links = R"(
        { output = ")" + lpf_last + R"(:Out" input="gainLFE_L:In" }
        { output = ")" + lpf_last + R"(:Out" input="gainLFE_R:In" }
)";
        }
    } else {
        // Per-ear mode: one common main delay, independent LFE delays and
        // all-pass sections for L/R. The low-pass stays shared.
        const string delay_l = std::to_string(alignment.lfe_delay_l_seconds);
        const string delay_r = std::to_string(alignment.lfe_delay_r_seconds);
        if (alignment.lfe_delay_l_seconds > 0.0) {
            lfe_phase_nodes += R"json(
        { type = builtin label = delay name = delayLFE_L config = { "max-delay" = 0.02 } control = { "Delay (s)" = )json" +
                               delay_l + R"json( } }
)json";
        }
        if (alignment.lfe_delay_r_seconds > 0.0) {
            lfe_phase_nodes += R"json(
        { type = builtin label = delay name = delayLFE_R config = { "max-delay" = 0.02 } control = { "Delay (s)" = )json" +
                               delay_r + R"json( } }
)json";
        }
        if (alignment.allpass_l_valid) {
            lfe_phase_nodes += R"(
        { type = builtin label = bq_allpass name = apLFE_L control = { "Freq" = )" +
                               std::to_string(alignment.allpass_l_freq) + R"( "Q" = )" +
                               std::to_string(alignment.allpass_l_q) + R"( } }
)";
        }
        if (alignment.allpass_r_valid) {
            lfe_phase_nodes += R"(
        { type = builtin label = bq_allpass name = apLFE_R control = { "Freq" = )" +
                               std::to_string(alignment.allpass_r_freq) + R"( "Q" = )" +
                               std::to_string(alignment.allpass_r_q) + R"( } }
)";
        }

        auto per_ear_links = [&](const char *side, double delay_seconds, bool allpass_valid) {
            const string gain = "gainLFE_" + string(side);
            const string delay = "delayLFE_" + string(side);
            const string ap = "apLFE_" + string(side);
            string links;
            const string first = allpass_valid ? ap : gain;
            if (delay_seconds > 0.0) {
                links += "        { output = \"" + lpf_last + ":Out\" input=\"" + delay + ":In\" }\n";
                links += "        { output = \"" + delay + ":Out\" input=\"" + first + ":In\" }\n";
            } else {
                links += "        { output = \"" + lpf_last + ":Out\" input=\"" + first + ":In\" }\n";
            }
            if (allpass_valid)
                links += "        { output = \"" + ap + ":Out\" input=\"" + gain + ":In\" }\n";
            return links;
        };

        lfe_phase_links = "\n" + per_ear_links("L", alignment.lfe_delay_l_seconds, alignment.allpass_l_valid) +
                          per_ear_links("R", alignment.lfe_delay_r_seconds, alignment.allpass_r_valid);
    }

    string main_input_links = R"(
        { output = "copyFL:Out"  input="convFL_L:In"  }
        { output = "copyFL:Out"  input="convFL_R:In"  }
        { output = "copySL:Out"  input="convSL_L:In"  }
        { output = "copySL:Out"  input="convSL_R:In"  }
        { output = "copyRL:Out"  input="convRL_L:In"  }
        { output = "copyRL:Out"  input="convRL_R:In"  }
        { output = "copyFC:Out"  input="convFC_L:In"  }
        { output = "copyFR:Out"  input="convFR_R:In"  }
        { output = "copyFR:Out"  input="convFR_L:In"  }
        { output = "copySR:Out"  input="convSR_R:In"  }
        { output = "copySR:Out"  input="convSR_L:In"  }
        { output = "copyRR:Out"  input="convRR_R:In"  }
        { output = "copyRR:Out"  input="convRR_L:In"  }
        { output = "copyFC:Out"  input="convFC_R:In"  }
)";
    if (alignment.delay_main) {
        const string delay_s = std::to_string(alignment.seconds);
        const string control = R"ctl( config = { "max-delay" = 0.02 } control = { "Delay (s)" = )ctl" + delay_s + R"ctl( } })ctl";
        alignment_nodes += R"(
        # Per-HRIR bass alignment (low-pass vs. HRIR low-frequency group delay)
        { type = builtin label = delay name = delayFL )" + control + R"(
        { type = builtin label = delay name = delayFR )" + control + R"(
        { type = builtin label = delay name = delayFC )" + control + R"(
        { type = builtin label = delay name = delayRL )" + control + R"(
        { type = builtin label = delay name = delayRR )" + control + R"(
        { type = builtin label = delay name = delaySL )" + control + R"(
        { type = builtin label = delay name = delaySR )" + control + R"(
)";
        main_input_links = R"(
        { output = "copyFL:Out" input="delayFL:In" } { output = "delayFL:Out" input="convFL_L:In" } { output = "delayFL:Out" input="convFL_R:In" }
        { output = "copyFR:Out" input="delayFR:In" } { output = "delayFR:Out" input="convFR_R:In" } { output = "delayFR:Out" input="convFR_L:In" }
        { output = "copyFC:Out" input="delayFC:In" } { output = "delayFC:Out" input="convFC_L:In" } { output = "delayFC:Out" input="convFC_R:In" }
        { output = "copyRL:Out" input="delayRL:In" } { output = "delayRL:Out" input="convRL_L:In" } { output = "delayRL:Out" input="convRL_R:In" }
        { output = "copyRR:Out" input="delayRR:In" } { output = "delayRR:Out" input="convRR_R:In" } { output = "delayRR:Out" input="convRR_L:In" }
        { output = "copySL:Out" input="delaySL:In" } { output = "delaySL:Out" input="convSL_L:In" } { output = "delaySL:Out" input="convSL_R:In" }
        { output = "copySR:Out" input="delaySR:In" } { output = "delaySR:Out" input="convSR_R:In" } { output = "delaySR:Out" input="convSR_L:In" }
)";
    }
    if (alignment.delay_lfe) {
        alignment_nodes += "\n        { type = builtin label = delay name = delayLFE config = { \"max-delay\" = 0.02 } control = { \"Delay (s)\" = " +
                           std::to_string(alignment.seconds) + " } }\n";
    }

    const char *mode_name = lfe_mode == LfeMode::Balanced ? "balanced"
                            : lfe_mode == LfeMode::Reference ? "reference"
                                                             : "impact";
    qInfo("build_effects_module_args: LFE mode %s (upmix %s%s%s%s), output trim %+.2f dB (outGain %.4f)",
          mode_name, upmix_enabled ? "on" : "off", lfe_send ? ", bass send" : "",
          lfe_mode == LfeMode::Impact && !bass_aligned ? ", impact falls back to -6 dB" : "",
          alignment.delay_main ? ", main aligned" : alignment.delay_lfe ? ", LFE aligned" : "",
          output_trim_db, 0.30 * std::pow(10.0, output_trim_db / 20.0));

    QString plugin_path;
    if (upmix_enabled || output_limiter)
        plugin_path = find_ladspa_plugin();
    if (upmix_enabled && plugin_path.isEmpty()) {
        qWarning("build_effects_module_args: FreeSurround LADSPA plugin not found");
        Q_EMIT error_occurred(QStringLiteral("未找到 FreeSurround 上混插件（freesurround_ladspa.so）。请重新安装本应用，或把插件复制到 /usr/lib/ladspa 等 LADSPA 目录。"));
        return {};
    }
    const bool limiter_enabled = output_limiter && !plugin_path.isEmpty();
    if (output_limiter && !limiter_enabled)
        qWarning("build_effects_module_args: Output limiter requested but the LADSPA plugin is missing; limiter disabled");

    string limiter_nodes;
    string limiter_links;
    string graph_outputs = R"( [ "outGainL:Out" "outGainR:Out" ])";
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
        graph_outputs = R"( [ "limiter:Out L" "limiter:Out R" ])";
        qInfo("build_effects_module_args: Output limiter enabled (-1.5 dBFS, 5 ms lookahead)");
    }

    if (upmix_enabled) {
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
        upmix_nodes = R"(
        # FreeSurround stereo to 7.1 upmixer with automatic stereo detection:
        # only upmixes while all non-L/R input channels are silent
        { type = ladspa name = fs plugin = ")" + json_escape(plugin_path.toStdString()) + R"(" label = "freesurround71_auto"
          control = {
              "center_image" = 0.7
              "shift" = 0.0
              "depth" = 1.0
              "circular_wrap" = 90.0
              "focus" = 0.0
              "front_sep" = 1.0
              "rear_sep" = 1.0
              "bass_lo" = 70.0
              "bass_hi" = 90.0
              "use_lfe" = )" + use_lfe + R"(
              "upmix_enable" = 1.0
              "lfe_send" = )" + send_gain_text + R"(
          }
        }
)";
        upmix_links = R"(
        # upmixer outputs feed the copy nodes (which fan out to the HRIR convolvers)
        { output = "fs:Out FL"  input="copyFL:In"  }
        { output = "fs:Out FR"  input="copyFR:In"  }
        { output = "fs:Out FC"  input="copyFC:In"  }
        { output = "fs:Out RL"  input="copyRL:In"  }
        { output = "fs:Out RR"  input="copyRR:In"  }
        { output = "fs:Out SL"  input="copySL:In"  }
        { output = "fs:Out SR"  input="copySR:In"  }
)";
        upmix_links += R"(
        # The plugin's LFE output is the native LFE in bypass mode and the
        # mono main-sum send while upmixing, so the graph always takes it.
        { output = "fs:Out LFE" input="copyLFE:In" }
)";
        graph_inputs = R"(  [ "fs:In FL" "fs:In FR" "fs:In FC" "fs:In LFE" "fs:In RL" "fs:In RR" "fs:In SL" "fs:In SR" ])";
    }

    // LFE gain: native LFE content is recorded 10 dB low (70mm-film convention),
    // and HeSuVi's lfc.txt applies a 2x makeup. In binaural rendering the net
    // reference is +5.5 dB (10 dB speaker makeup minus 4.5 dB for the two-point-
    // source headphone rendering); 2x + the center-normalized timbre IR targets
    // it, and with BW4 measures +6.0 dB at 60 Hz (0.5 dB high). The graph uses
    // unity while upmix is enabled: the native LFE gets its 2x in the auto
    // plugin's bypass branch (and the balanced send is already at its intended
    // level); with upmix disabled only native LFE remains, so the graph applies
    // the 2x itself.
    const string lfe_gain = upmix_enabled ? "1.0" : "2.0";
    // Set 7.1 filter graph for virtual surround module, automatically creating playback and capture nodes
    const string filter_graph = R"(
{
    nodes = [)" + upmix_nodes + R"(
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
        { type = builtin label = convolver name = convFL_L config = { filename = ")" +
                                    hrir_path + R"(" channel =  0 resample_quality = 14 } }
        { type = builtin label = convolver name = convFL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  1 resample_quality = 14 } }
        { type = builtin label = convolver name = convSL_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  2 resample_quality = 14 } }
        { type = builtin label = convolver name = convSL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  3 resample_quality = 14 } }
        { type = builtin label = convolver name = convRL_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  4 resample_quality = 14 } }
        { type = builtin label = convolver name = convRL_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  5 resample_quality = 14 } }
        { type = builtin label = convolver name = convFC_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  6 resample_quality = 14 } }
        { type = builtin label = convolver name = convFR_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  7 resample_quality = 14 } }
        { type = builtin label = convolver name = convFR_L config = { filename = ")" +
                                        hrir_path + R"(" channel =  8 resample_quality = 14 } }
        { type = builtin label = convolver name = convSR_R config = { filename = ")" +
                                        hrir_path + R"(" channel =  9 resample_quality = 14 } }
        { type = builtin label = convolver name = convSR_L config = { filename = ")" +
                                        hrir_path + R"(" channel = 10 resample_quality = 14 } }
        { type = builtin label = convolver name = convRR_R config = { filename = ")" +
                                        hrir_path + R"(" channel = 11 resample_quality = 14 } }
        { type = builtin label = convolver name = convRR_L config = { filename = ")" +
                                        hrir_path + R"(" channel = 12 resample_quality = 14 } }
        { type = builtin label = convolver name = convFC_R config = { filename = ")" +
                                        hrir_path + R"(" channel = 13 resample_quality = 14 } }

        # LFE path: convolved with the selected LFE timbre IR, all of the same length
        # as the HRIR so both branches stay time-aligned. delta = unit impulse
        # (uncolored, equal L/R); diffuse = 7-pair average; center = center HRIR
        # pair. The last two deliberately apply HRIR timbre and may differ slightly
        # between ears; only delta is the strict conventional LFE render.
        { type = builtin label = convolver name = convLFE_L config = { filename = ")" +
                                        lfe_conv_path + R"(" channel = )" +
                                        lfe_channel_l + R"( resample_quality = 14 } }
        { type = builtin label = convolver name = convLFE_R config = { filename = ")" +
                                        lfe_conv_path + R"(" channel = )" +
                                        lfe_channel_r + R"( resample_quality = 14 } }

        # LFE path low-pass: fixed 120 Hz Butterworth-4 (Q=0.5412/1.3066, 24 dB/oct).
        # At 120 Hz this is -3 dB, so the LFE is 3 dB hotter at 100-120 Hz than the
        # earlier -6 dB LR4; that is the intended BW4 impact, not a level bug.
        # The IR is the selected diffuse/delta/center LFE timbre IR; gain is 2x
        # for native LFE channels when the plugin is not in the chain, and 1x
        # while the plugin's bypass applies the makeup.
)" + lpf_nodes + lfe_phase_nodes + R"(
        { type = builtin label = linear name = gainLFE_L control = { "Mult" = )" + lfe_gain + R"( } }
        { type = builtin label = linear name = gainLFE_R control = { "Mult" = )" + lfe_gain + R"( } }

        # stereo output
        { type = builtin label = mixer name = mixL }
        { type = builtin label = mixer name = mixR }
)" + alignment_nodes + R"(

        # Output headroom, matching HeSuVi's level budget: its upmix matrix attenuates the
        # main channels (L=0.5*L, R=0.5*R, ...) so the 8 binaural channels can sum up
        # without clipping. Users compensate with the system volume, applied after here.
        # The optional preset loudness match multiplies both channels by the same trim.
        { type = builtin label = linear name = outGainL control = { "Mult" = )" + std::to_string(0.30 * std::pow(10.0, output_trim_db / 20.0)) + R"( } }
        { type = builtin label = linear name = outGainR control = { "Mult" = )" + std::to_string(0.30 * std::pow(10.0, output_trim_db / 20.0)) + R"( } }
)" + limiter_nodes + R"(
    ]
    links = [)" + upmix_links + R"(
        # input
        )" + main_input_links + R"(
        )" + lfe_source_link + R"(
        )" + lfe_align_links + R"(
        )" + lpf_chain + lfe_phase_links + R"(
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
        { output = "convLFE_R:Out" input="mixR:In 8" }
        { output = "convLFE_L:Out" input="mixL:In 8" }
        { output = "mixL:Out" input="outGainL:In" }
        { output = "mixR:Out" input="outGainR:In" }
)" + limiter_links + R"(
    ]
    inputs  = )" + graph_inputs + R"(
    outputs = )" + graph_outputs + R"(
}
        )";

    // Set capture properties for virtual surround module
    // node.latency keeps the graph quantum at 1024 samples or more while this
    // device is active: without it, a low-latency app can drag the whole chain
    // down to tiny quanta where the 14 convolvers (plus the FFT upmixer) are
    // far more prone to xruns, and every quantum renegotiation is audible.
    // audio.rate pins both ends of the chain to 48000 Hz: the bundled HeSuVi
    // HRIR WAVs are 48000 Hz and while PipeWire's convolver would correctly
    // resample them to any other graph rate (which clock.allowed-rates can
    // switch to at runtime), running them at their native rate avoids that
    // resampling entirely. PipeWire converts between the chain and streams or
    // devices running at other rates.
    const string capture_props = R"(
{
    node.name = )" + capture_name +
                                         R"(
    node.description = "虚拟环绕声（内部效果）"
    media.class = "Audio/Sink"
    priority.session = 0
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 8
    audio.position = [ FL FR FC LFE RL RR SL SR ]
}
        )";

    // Set playback properties for virtual surround module
    const string playback_props = R"(
{
    node.name = )" + playback_name +
                                          R"(
    node.passive = true
    node.latency = "1024/48000"
    audio.rate = 48000
    audio.channels = 2
    audio.position = [ FL FR ]
}
        )";

    // Combine the properites to a single JSON string
    const string args = R"(
{
    node.description = "虚拟环绕声（内部效果）"
    media.name = "虚拟环绕声（内部效果）"
    filter.graph = )" +
               filter_graph +
               R"(,
    capture.props = )" +
               capture_props + R"(,
    playback.props = )" +
               playback_props + R"(
}
        )";

    return args;
}

bool PipeWireManager::create_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                                     double output_trim_db, bool output_limiter) {
    if (!is_valid()) {
        Q_EMIT error_occurred(QStringLiteral("连接 PipeWire 音频服务失败。"));
        return false;
    }
    if (bridge_module) {
        qInfo("create_virtual_surround_module: Virtual surround device already exists");
        return true;
    }

    capture_node_name = capture_node_name_base;
    effects_generation = 0;

    qInfo("create_virtual_surround_module: Creating device with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    pw_thread_loop_lock(thread_loop);

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

    if (!load_bridge_module_locked()) {
        pw_impl_module_destroy(new_effects);
        sync_with_core(1);
        effects_capture_node_name.clear();
        effects_playback_node_name.clear();
        pw_thread_loop_unlock(thread_loop);
        qWarning("create_virtual_surround_module: Failed to load the bridge module");
        Q_EMIT error_occurred(QStringLiteral("创建虚拟环绕声设备失败。"));
        return false;
    }

    effects_module = new_effects;
    sync_with_core(1);
    pw_thread_loop_unlock(thread_loop);

    qInfo("create_virtual_surround_module: Successfully created device");

    return true;
}

bool PipeWireManager::replace_virtual_surround_module(const string &hrir_wav_path, bool upmix_enabled,
                                                      double output_trim_db, bool output_limiter) {
    if (!bridge_module)
        return create_virtual_surround_module(hrir_wav_path, upmix_enabled, output_trim_db, output_limiter);

    if (!is_valid()) {
        Q_EMIT error_occurred(QStringLiteral("连接 PipeWire 音频服务失败。"));
        return false;
    }

    if (!is_registry_listener_added || !metadata) {
        // Only called while routing is enabled; without the registry listener the
        // internal node IDs are unknown and the bridge cannot be re-targeted
        qWarning("replace_virtual_surround_module: Routing is not enabled");
        return false;
    }

    qInfo("replace_virtual_surround_module: Replacing effects graph with WAV path '%s', upmixing %s",
          hrir_wav_path.c_str(), upmix_enabled ? "enabled" : "disabled");

    QElapsedTimer switch_timer;
    switch_timer.start();

    pw_thread_loop_lock(thread_loop);

    // Pick up node globals that arrived since the last iteration
    sync_with_core(1);

    const string old_capture_name = effects_capture_node_name;
    const string old_playback_name = effects_playback_node_name;
    const uint32_t old_capture_id = effects_capture_id;
    const uint32_t old_playback_id = effects_playback_id;
    const uint32_t old_generation = effects_generation;

    if (bridge_playback_id == SPA_ID_INVALID || old_capture_id == SPA_ID_INVALID) {
        pw_thread_loop_unlock(thread_loop);
        qWarning("replace_virtual_surround_module: Bridge or effects nodes are not known yet");
        Q_EMIT error_occurred(QStringLiteral("虚拟环绕声设备尚未就绪，请重试。"));
        return false;
    }

    pw_impl_module *new_effects = load_effects_module_locked(hrir_wav_path, upmix_enabled, output_trim_db, output_limiter);
    if (!new_effects) {
        effects_capture_node_name = old_capture_name;
        effects_playback_node_name = old_playback_name;
        effects_capture_id = old_capture_id;
        effects_playback_id = old_playback_id;
        effects_generation = old_generation;
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

    if (new_capture_id != SPA_ID_INVALID) {
        // Re-target the bridge playback stream: the application streams stay linked
        // to the bridge sink, so they never touch the physical device and no sink
        // they are linked to is removed (which is what makes WirePlumber pause
        // MPRIS players and what made the output jump in level).
        // Retry the write once in case WirePlumber had not picked up the new sink yet.
        for (int attempt = 0; attempt < 2 && !linked; ++attempt) {
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
        if (old_capture_id != SPA_ID_INVALID && !old_capture_name.empty()) {
            pw_metadata_set_property(metadata, bridge_playback_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", old_capture_name.c_str());
            sync_with_core(1);
            restored = wait_for([this, old_capture_id]() { return has_link(bridge_playback_id, old_capture_id); }, 500);
        }

        if (restored) {
            pw_impl_module_destroy(new_effects);
            sync_with_core(1);
            effects_capture_node_name = old_capture_name;
            effects_playback_node_name = old_playback_name;
            effects_capture_id = old_capture_id;
            effects_playback_id = old_playback_id;
            effects_generation = old_generation;
            pw_thread_loop_unlock(thread_loop);
            Q_EMIT error_occurred(QStringLiteral("无法切换虚拟环绕声配置。上一配置仍在运行。"));
            return false;
        }

        // Neither confirmed: keep the new graph alive rather than leaving the bridge
        // without a target, and point the bridge at it once more
        qWarning("replace_virtual_surround_module: Could not restore the old sink, keeping the new graph");
        if (new_capture_id != SPA_ID_INVALID)
            pw_metadata_set_property(metadata, bridge_playback_id, PW_KEY_TARGET_OBJECT, "Spa:String:JSON", effects_capture_node_name.c_str());
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

    pw_impl_module *old_effects = effects_module;
    effects_module = new_effects;
    sync_with_core(1);

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
    if (!pending_effects_destroy.contains(old_module))
        pending_effects_destroy.append(old_module);

    // Keep the silent old graph around long enough for WirePlumber's MPRIS link
    // bookkeeping to process the link removal (it syncs after the link event).
    // 250 ms was too short once graph creation started taking ~1 s: the old sink
    // was destroyed while links_in still remembered the bridge link, and
    // mpris-pause.lua paused the player even though the actual link was gone.
    QTimer::singleShot(1200, this, [this, old_module, old_sink_id, attempts_left]() {
        // The shutdown path or a following replace may have collected it already
        if (!pending_effects_destroy.contains(old_module) || !thread_loop)
            return;

        pw_thread_loop_lock(thread_loop);
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
        pw_impl_module_destroy(old_module);
        sync_with_core(1);
        pw_thread_loop_unlock(thread_loop);

        qDebug("schedule_effects_destroy: Destroyed replaced effects graph");
    });
}

void PipeWireManager::destroy_pending_effects_locked() {
    for (pw_impl_module *m : std::as_const(pending_effects_destroy))
        pw_impl_module_destroy(m);
    pending_effects_destroy.clear();
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

    effects_generation = new_generation;
    graph_upmix_enabled.store(upmix_enabled, std::memory_order_relaxed);
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
    while (!(satisfied = predicate())) {
        const int remaining = timeout_ms - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0)
            break;

        struct timespec abstime;
        clock_gettime(CLOCK_REALTIME, &abstime);
        const int slice = std::min(remaining, 50);
        abstime.tv_nsec += static_cast<long>(slice) * 1000 * 1000;
        if (abstime.tv_nsec >= 1000000000) {
            abstime.tv_sec += 1;
            abstime.tv_nsec -= 1000000000;
        }
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

    if (bridge_module) {
        pw_impl_module_destroy(bridge_module);
        bridge_module = nullptr;
    }
    if (effects_module) {
        pw_impl_module_destroy(effects_module);
        effects_module = nullptr;
    }
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

    spa_zero(registry_listener);
    // Route all new audio output streams to the filter chain, only active while the loop runs
    pw_registry_add_listener(registry, &registry_listener, &registry_events, this);

    is_registry_listener_added = true;

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
    return thread_loop && context && core;
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

    const double gain = lfe_reference_gain(path);
    if (gain <= 0.0)
        return result;
    result.reference_gain = gain;
    result.reference_gain_db = 20.0 * std::log10(gain);

    const LfeAlignment alignment = lfe_alignment_configured(path, 48000, lfe_lowpass, lfe_timbre,
                                                              lfe_per_ear_alignment);
    if (!alignment.valid)
        return result;

    result.valid = true;
    result.alignment_ms = (alignment.per_ear ? alignment.main_delay_seconds : alignment.seconds) * 1000.0;
    result.delay_main = alignment.delay_main;
    result.delay_lfe = alignment.delay_lfe;
    result.residual_degrees = alignment.residual_degrees;
    result.average_residual_degrees = alignment.average_residual_degrees;
    result.worst_channel_residual_degrees = alignment.worst_channel_residual_degrees;
    result.allpass_valid = alignment.allpass_valid;
    result.allpass_freq = alignment.allpass_freq;
    result.allpass_q = alignment.allpass_q;
    result.allpass_residual_degrees = alignment.allpass_residual_degrees;
    result.allpass_worst_channel_degrees = alignment.allpass_worst_channel_degrees;
    result.source_rate = alignment.source_rate;
    result.resampled = alignment.source_rate != 48000;
    result.per_ear_alignment = alignment.per_ear;
    result.per_ear_values_valid = alignment.per_ear;
    result.timbre = alignment.timbre;
    result.main_delay_ms = alignment.main_delay_seconds * 1000.0;
    result.lfe_delay_l_ms = alignment.lfe_delay_l_seconds * 1000.0;
    result.lfe_delay_r_ms = alignment.lfe_delay_r_seconds * 1000.0;
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
    result.send_safe = lfe_send_safe(alignment);
    result.impact_allowed = lfe_impact_allowed(alignment);
    result.level_db = hrir_level_db(path);
    return result;
}

double PipeWireManager::hrir_level_db(const string &path) const {
    uint32_t channels = 0, rate = 0, block_align = 0, frames = 0;
    uint16_t bits = 0, format_tag = 0;
    size_t data_offset = 0;
    if (!parse_wav_format(path, channels, rate, block_align, bits, format_tag, frames, data_offset))
        return -1000.0;
    const bool supported = (format_tag == 3 && bits == 32) ||
                           (format_tag == 1 && (bits == 16 || bits == 24 || bits == 32));
    if (channels != 14 || frames == 0 || rate == 0 || !supported)
        return -1000.0;

    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly))
        return -1000.0;
    const QByteArray data = file.readAll();
    file.close();
    if (data_offset + static_cast<size_t>(frames) * block_align > static_cast<size_t>(data.size()))
        return -1000.0;

    // A center-weighted binaural impulse response: the C pair plus half of the
    // front pair. This follows how a centered source is distributed by the
    // upmixer and gives a stable relative level for every preset.
    std::vector<double> hl(frames), hr(frames);
    for (uint32_t n = 0; n < frames; ++n) {
        const double left = wav_sample(data, data_offset, block_align, format_tag, bits, n, 6);
        const double right = wav_sample(data, data_offset, block_align, format_tag, bits, n, 13);
        const double fl_l = wav_sample(data, data_offset, block_align, format_tag, bits, n, 0);
        const double fl_r = wav_sample(data, data_offset, block_align, format_tag, bits, n, 1);
        const double fr_l = wav_sample(data, data_offset, block_align, format_tag, bits, n, 8);
        const double fr_r = wav_sample(data, data_offset, block_align, format_tag, bits, n, 7);
        hl[n] = left + 0.5 * (fl_l + fr_l);
        hr[n] = right + 0.5 * (fl_r + fr_r);
    }

    // 1/3-octave A-weighted energy. The absolute value is arbitrary; only the
    // difference between presets is used.
    double energy_sum = 0.0;
    int bands = 0;
    for (double f = 20.0; f <= 20000.0; f *= 1.2599210498948732) {
        double re_l = 0.0, im_l = 0.0, re_r = 0.0, im_r = 0.0;
        const double w = 2.0 * std::numbers::pi * f / static_cast<double>(rate);
        for (uint32_t n = 0; n < frames; ++n) {
            const double phase = w * static_cast<double>(n);
            const double c = std::cos(phase);
            const double s = std::sin(phase);
            re_l += hl[n] * c;
            im_l -= hl[n] * s;
            re_r += hr[n] * c;
            im_r -= hr[n] * s;
        }
        const double f2 = f * f;
        const double ra = (12194.0 * 12194.0 * f2 * f2) /
                          ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) *
                           (f2 + 12194.0 * 12194.0));
        const double a_weight = std::pow(10.0, (20.0 * std::log10(ra) + 2.0) / 10.0);
        const double energy = (re_l * re_l + im_l * im_l + re_r * re_r + im_r * im_r) / 2.0;
        energy_sum += energy * a_weight;
        ++bands;
    }
    if (bands == 0 || energy_sum <= 0.0)
        return -1000.0;
    return 10.0 * std::log10(energy_sum / static_cast<double>(bands));
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
    if (id == PW_ID_CORE && res < 0) {
        // Emitted from the PipeWire thread: the connection to FrontendManager
        // is automatically queued across threads
        Q_EMIT manager->error_occurred(QString::asprintf("PipeWire error: %s", message && message[0] ? message : strerror(-res)));
    }
}

bool PipeWireManager::sync_with_core(int timeout_sec) {
    if (!core || !thread_loop)
        return false;

    sync_done = false;
    sync_seq = pw_core_sync(core, PW_ID_CORE, sync_seq + 1);

    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += timeout_sec;

    while (!sync_done) {
        if (pw_thread_loop_timed_wait_full(thread_loop, &abstime) != 0) {
            qWarning("sync_with_core: Timed out after %d seconds", timeout_sec);
            return false;
        }
    }
    return true;
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

    pw_thread_loop_unlock(thread_loop);
}

PipeWireManager::~PipeWireManager() {
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
