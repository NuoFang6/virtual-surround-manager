#include "output_equalizer.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

QString write_file(QTemporaryDir &dir, const QString &name, const QString &contents) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    file.write(contents.toUtf8());
    file.close();
    return path;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    expect(dir.isValid(), "temporary directory is available");

    OutputEqConfig config;
    QString error;
    const QString mixed = write_file(dir, QStringLiteral("mixed.txt"), QStringLiteral(
        "Preamp: -6.00 dB\n"
        "# AutoEq-compatible comment\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 3.00 dB Q 0.700\n"
        "Filter 2: ON PK Fc 1000 Hz Gain 6.00 dB Q 1.000\n"
        "Filter 3: OFF HSC Fc 10000 Hz Gain -2.00 dB Q 0.700\n"));
    expect(OutputEqualizer::read(mixed, config, &error), "valid Equalizer APO file parses");
    expect(config.filters.size() == 2, "disabled filters are omitted");
    expect(std::abs(config.file_preamp_db + 6.0) < 1e-9, "file preamp is preserved for status");
    expect(std::abs(config.reference_level_db + config.static_gain_db) < 1e-9,
           "fixed EQ gain preserves the A-weighted reference level");
    expect(std::abs(config.file_preamp_db + config.automatic_gain_db - config.static_gain_db) < 1e-9,
           "reported adjustment combines file preamp with fixed calibration");
    expect(std::abs(config.maximum_output_gain_db -
                    (config.maximum_filter_gain_db + config.static_gain_db)) < 1e-9,
           "reported output peak includes fixed calibration");

    const QString attenuation = write_file(dir, QStringLiteral("attenuation.txt"), QStringLiteral(
        "Preamp: -2 dB\n"
        "Filter 1: ON LSC Fc 12000 Hz Gain -4 dB Q 0.7\n"
        "Filter 2: ON HSC Fc 20 Hz Gain -4 dB Q 0.7\n"));
    expect(OutputEqualizer::read(attenuation, config, &error), "attenuation-only EQ parses");
    expect(config.static_gain_db > 3.5, "attenuation-only EQ receives positive fixed makeup");
    expect(config.maximum_output_gain_db > 0.0,
           "reference matching does not flatten the attenuation-only response");

    const QString boost = write_file(dir, QStringLiteral("boost.txt"), QStringLiteral(
        "Preamp: 0 dB\n"
        "Filter 1: ON PK Fc 1000 Hz Gain 8 dB Q 1\n"));
    expect(OutputEqualizer::read(boost, config, &error), "boost EQ parses");
    expect(config.maximum_output_gain_db > 3.0,
           "narrow boost remains available and is left to the mandatory limiter");

    const QString unknown = write_file(dir, QStringLiteral("unknown.txt"), QStringLiteral(
        "Preamp: 0 dB\nFilter 1: ON LP Fc 100 Hz Gain 0 dB Q 0.7\n"));
    expect(!OutputEqualizer::read(unknown, config, &error), "unknown enabled filter is rejected");

    const QString nan = write_file(dir, QStringLiteral("nan.txt"), QStringLiteral(
        "Preamp: nan dB\nFilter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n"));
    expect(!OutputEqualizer::read(nan, config, &error), "NaN is rejected");

    const QString too_low = write_file(dir, QStringLiteral("too-low.txt"), QStringLiteral(
        "Preamp: 0 dB\nFilter 1: ON PK Fc 1 Hz Gain 60 dB Q 100\n"));
    expect(!OutputEqualizer::read(too_low, config, &error), "filters below the response scan floor are rejected");

    const QString missing = write_file(dir, QStringLiteral("missing.txt"), QStringLiteral(
        "Filter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n"));
    expect(!OutputEqualizer::read(missing, config, &error), "missing preamp is rejected");

    QString too_many = QStringLiteral("Preamp: 0 dB\n");
    for (int i = 1; i <= 65; ++i)
        too_many += QStringLiteral("Filter %1: ON PK Fc %2 Hz Gain 0 dB Q 1\n").arg(i).arg(100 + i);
    const QString too_many_path = write_file(dir, QStringLiteral("too-many.txt"), too_many);
    expect(!OutputEqualizer::read(too_many_path, config, &error), "more than 64 enabled filters are rejected");

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            OutputEqConfig external;
            QString external_error;
            const bool ok = OutputEqualizer::read(QString::fromLocal8Bit(argv[i]), external, &external_error);
            if (!ok)
                std::fprintf(stderr, "FAIL: %s: %s\n", argv[i], qPrintable(external_error));
            failures += ok ? 0 : 1;
        }
    }

    if (failures == 0)
        std::puts("OK");
    return failures == 0 ? 0 : 1;
}
