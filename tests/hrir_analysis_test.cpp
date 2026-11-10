#include "pipewire_manager.h"
#include "acoustics.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <bit>
#include <cstdio>
#include <array>
#include <cmath>

static bool write_impulses(const QString &path, const std::array<float, 14> &gains,
                           uint32_t rate = 48000, bool stagger = false, uint32_t global_delay = 0) {
    const uint32_t frames = 96 + global_delay;
    QByteArray bytes;
    const auto le16 = [&](uint16_t value) { char b[2]; qToLittleEndian(value, b); bytes.append(b, 2); };
    const auto le32 = [&](uint32_t value) { char b[4]; qToLittleEndian(value, b); bytes.append(b, 4); };
    bytes.append("RIFF", 4); le32(36 + frames * 56); bytes.append("WAVEfmt ", 8);
    le32(16); le16(3); le16(14); le32(rate); le32(rate * 56); le16(56); le16(32);
    bytes.append("data", 4); le32(frames * 56);
    for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t channel = 0; channel < 14; ++channel)
            le32(std::bit_cast<uint32_t>(frame == global_delay + (stagger ? channel * 5 : 0) ? gains[channel] : 0.0f));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir runtime;
    // Analysis needs no daemon; point every connection attempt at an empty,
    // private directory instead of the user's running audio session.
    qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8());
    qputenv("PIPEWIRE_REMOTE", "absent-audit-server");
    PipeWireManager manager;
    int failures = 0;
    if (argc != 2) return 2;
    const QString directory = QString::fromLocal8Bit(argv[1]);
    const QStringList files = {QStringLiteral("27_95_142.wav"), QStringLiteral("AppleSpatial.wav"),
                              QStringLiteral("AtmosRenderer-Near-Generic.wav")};
    for (const QString &file : files) {
        const auto result = manager.analyze_hrir(QString(directory + u'/' + file).toStdString());
        // The analysis reports only the alignment delays applied by the graph;
        // the old 512-sample bass-shelf delay no longer exists.
        const bool ok = result.valid && result.per_ear_values_valid &&
            std::isfinite(result.average_residual_l_degrees) &&
            std::isfinite(result.average_residual_r_degrees) &&
            result.main_delay_ms >= 0.0 && result.main_delay_ms < 20.0 &&
            result.lfe_delay_l_ms >= 0.0 && result.lfe_delay_l_ms < 20.0 &&
            result.lfe_delay_r_ms >= 0.0 && result.lfe_delay_r_ms < 20.0;
        std::printf("[%s] %s gain=%.6f delay=%.6f LFE=%.6f/%.6f residual=%.2f/%.2f\n",
                    ok ? "PASS" : "FAIL", qPrintable(file), result.reference_gain,
                    result.main_delay_ms, result.lfe_delay_l_ms, result.lfe_delay_r_ms,
                    result.average_residual_l_degrees, result.average_residual_r_degrees);
        if (!ok) ++failures;
    }
    const auto invalid = manager.analyze_hrir(QString(directory + QStringLiteral("/oal_dflt.wav")).toStdString());
    if (invalid.valid) ++failures;
    std::printf("[%s] seven-channel file rejected\n", invalid.valid ? "FAIL" : "PASS");
    const auto check = [&](const char *name, bool ok) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check("phase distance is periodic", std::abs(Acoustics::phase_error(359.0 * std::numbers::pi / 180.0) +
        std::numbers::pi / 180.0) < 1e-12);
    check("A weighting is normalized at 1 kHz", std::abs(Acoustics::a_weight_power(1000.0) - 1.0) < 1e-12);
    std::array<float, 14> gains;
    gains.fill(1.0f);
    const auto flat = runtime.filePath(QStringLiteral("flat.wav"));
    check("write flat reference", write_impulses(flat, gains));
    const double flat_level = manager.hrir_level_db(flat.toStdString());
    check("flat reference level is 0 dB", std::abs(flat_level) < 1e-8);
    gains[0] = gains[1] = gains[7] = gains[8] = -1.0f;
    const auto reversed = runtime.filePath(QStringLiteral("reversed.wav"));
    check("write polarity reference", write_impulses(reversed, gains));
    check("independent-channel power ignores relative polarity", std::abs(manager.hrir_level_db(reversed.toStdString()) - flat_level) < 1e-8);
    const auto delayed = runtime.filePath(QStringLiteral("delayed.wav"));
    check("write delay reference", write_impulses(delayed, gains, 48000, true));
    check("independent-channel power ignores relative delays", std::abs(manager.hrir_level_db(delayed.toStdString()) - flat_level) < 1e-8);
    gains.fill(0.5f);
    const auto scaled = runtime.filePath(QStringLiteral("scaled.wav"));
    check("write scaled reference", write_impulses(scaled, gains));
    check("half-amplitude level is -6.0206 dB", std::abs(manager.hrir_level_db(scaled.toStdString()) + 6.020599913) < 1e-7);
    const auto low_rate = runtime.filePath(QStringLiteral("low-rate.wav"));
    check("write low-rate reference", write_impulses(low_rate, gains, 22050));
    check("level integration respects Nyquist", std::abs(manager.hrir_level_db(low_rate.toStdString()) + 6.020599913) < 1e-7);
    gains.fill(1e-12f);
    const auto quiet = runtime.filePath(QStringLiteral("quiet.wav"));
    check("write quiet phase reference", write_impulses(quiet, gains));
    const auto quiet_phase = manager.analyze_hrir(quiet.toStdString());
    const auto scaled_phase = manager.analyze_hrir(scaled.toStdString());
    check("global attenuation preserves valid phase", quiet_phase.valid && quiet_phase.per_ear_values_valid &&
        std::abs(quiet_phase.main_delay_ms - scaled_phase.main_delay_ms) < 1e-6 &&
        std::abs(quiet_phase.lfe_delay_l_ms - scaled_phase.lfe_delay_l_ms) < 1e-6 &&
        std::isfinite(quiet_phase.main_delay_ms) &&
        quiet_phase.main_delay_ms >= 0.0 && quiet_phase.main_delay_ms < 10.0);
    return failures ? 1 : 0;
}
