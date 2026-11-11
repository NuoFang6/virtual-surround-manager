#include "output_equalizer.h"
#include "acoustics.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <utility>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kMaximumFilters = 64;
constexpr int kResponsePoints = 32768;

void set_error(QString *error, const QString &message) {
    if (error)
        *error = message;
}

bool parse_number(const QString &text, double &value) {
    bool ok = false;
    value = text.toDouble(&ok);
    return ok && std::isfinite(value);
}

} // namespace

bool OutputEqualizer::read(const QString &path, OutputEqConfig &config, QString *error) {
    config = {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        set_error(error, QStringLiteral("无法读取 EQ 文件：%1").arg(path));
        return false;
    }
    if (file.size() > 1024 * 1024) {
        set_error(error, QStringLiteral("EQ 文件超过 1 MiB 上限。"));
        return false;
    }

    const QString number = QStringLiteral(R"([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)");
    const QRegularExpression preamp_re(
        QStringLiteral(R"(^\s*Preamp\s*:\s*(%1)\s*dB\s*(?:#.*)?$)").arg(number),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression filter_re(
        QStringLiteral(R"(^\s*Filter\s+\d+\s*:\s*(ON|OFF)\s+(PK|LSC|HSC)\s+Fc\s+(%1)\s*Hz\s+Gain\s+(%1)\s*dB\s+Q\s+(%1)\s*(?:#.*)?$)").arg(number),
        QRegularExpression::CaseInsensitiveOption);

    bool preamp_seen = false;
    int line_number = 0;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        ++line_number;
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(u'#'))
            continue;

        const QRegularExpressionMatch preamp_match = preamp_re.match(line);
        if (preamp_match.hasMatch()) {
            if (preamp_seen) {
                set_error(error, QStringLiteral("EQ 文件第 %1 行重复定义 Preamp。").arg(line_number));
                return false;
            }
            if (!parse_number(preamp_match.captured(1), config.file_preamp_db)) {
                set_error(error, QStringLiteral("EQ 文件第 %1 行的 Preamp 无效。").arg(line_number));
                return false;
            }
            preamp_seen = true;
            continue;
        }

        const QRegularExpressionMatch filter_match = filter_re.match(line);
        if (!filter_match.hasMatch()) {
            set_error(error, QStringLiteral("EQ 文件第 %1 行不是支持的 Equalizer APO 参数。仅接受 Preamp、PK、LSC 和 HSC。").arg(line_number));
            return false;
        }
        if (filter_match.captured(1).compare(QStringLiteral("OFF"), Qt::CaseInsensitive) == 0)
            continue;
        if (config.filters.size() >= kMaximumFilters) {
            set_error(error, QStringLiteral("EQ 文件启用的滤波器超过 %1 段上限。").arg(kMaximumFilters));
            return false;
        }

        OutputEqFilter filter;
        const QString type = filter_match.captured(2).toUpper();
        filter.type = type == QStringLiteral("LSC") ? OutputEqFilter::Type::LowShelf
                    : type == QStringLiteral("HSC") ? OutputEqFilter::Type::HighShelf
                                                      : OutputEqFilter::Type::Peaking;
        if (!parse_number(filter_match.captured(3), filter.frequency) ||
            !parse_number(filter_match.captured(4), filter.gain_db) ||
            !parse_number(filter_match.captured(5), filter.q)) {
            set_error(error, QStringLiteral("EQ 文件第 %1 行包含无效数字。").arg(line_number));
            return false;
        }
        if (filter.frequency < 5.0 || filter.frequency >= kSampleRate / 2.0) {
            set_error(error, QStringLiteral("EQ 文件第 %1 行的频率必须在 5–24000 Hz 之间。").arg(line_number));
            return false;
        }
        if (filter.q <= 0.0 || filter.q > 100.0) {
            set_error(error, QStringLiteral("EQ 文件第 %1 行的 Q 必须在 0–100 之间。").arg(line_number));
            return false;
        }
        if (std::abs(filter.gain_db) > 60.0) {
            set_error(error, QStringLiteral("EQ 文件第 %1 行的增益超出 ±60 dB 上限。").arg(line_number));
            return false;
        }
        config.filters.append(filter);
    }

    if (!preamp_seen) {
        set_error(error, QStringLiteral("EQ 文件缺少 Preamp 行。"));
        return false;
    }
    if (config.filters.isEmpty()) {
        set_error(error, QStringLiteral("EQ 文件没有启用的 PK、LSC 或 HSC 滤波器。"));
        return false;
    }

    double maximum = -std::numeric_limits<double>::infinity();
    double reference_energy = 0.0;
    double reference_weight = 0.0;
    const auto combined_response_db = [&](double frequency) {
        double response = 0.0;
        for (const OutputEqFilter &filter : std::as_const(config.filters))
            response += response_db(filter, frequency, kSampleRate);
        return response;
    };
    maximum = std::max(combined_response_db(0.0), combined_response_db(kSampleRate * 0.5));
    for (int index = 0; index < kResponsePoints; ++index) {
        const double ratio = static_cast<double>(index) / (kResponsePoints - 1);
        const double frequency = 5.0 * std::pow((kSampleRate * 0.5) / 5.0, ratio);
        const double response = combined_response_db(frequency);
        maximum = std::max(maximum, response);
        if (frequency >= 20.0 && frequency <= 20000.0) {
            const double weight = Acoustics::a_weight_power(frequency);
            reference_energy += weight * std::pow(10.0, response / 10.0);
            reference_weight += weight;
        }
    }
    if (!std::isfinite(maximum) || !std::isfinite(reference_energy) ||
        reference_energy <= 0.0 || reference_weight <= 0.0) {
        set_error(error, QStringLiteral("无法计算 EQ 频率响应。"));
        return false;
    }

    config.enabled = true;
    config.source_path = path;
    config.maximum_filter_gain_db = maximum;
    config.reference_level_db = 10.0 * std::log10(reference_energy / reference_weight);
    config.static_gain_db = -config.reference_level_db;
    config.automatic_gain_db = config.static_gain_db - config.file_preamp_db;
    config.maximum_output_gain_db = maximum + config.static_gain_db;
    return true;
}

QString OutputEqualizer::filter_type_name(OutputEqFilter::Type type) {
    switch (type) {
    case OutputEqFilter::Type::LowShelf:
        return QStringLiteral("bq_lowshelf");
    case OutputEqFilter::Type::HighShelf:
        return QStringLiteral("bq_highshelf");
    case OutputEqFilter::Type::Peaking:
        return QStringLiteral("bq_peaking");
    }
    return {};
}

double OutputEqualizer::response_db(const OutputEqFilter &filter, double frequency, double sample_rate) {
    const double w0 = 2.0 * std::numbers::pi * filter.frequency / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * filter.q);
    const double cosine = std::cos(w0);
    const double a = std::pow(10.0, filter.gain_db / 40.0);
    const double k2 = 2.0 * std::sqrt(a) * alpha;
    const double plus = a + 1.0;
    const double minus = a - 1.0;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (filter.type) {
    case OutputEqFilter::Type::Peaking:
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosine;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha / a;
        break;
    case OutputEqFilter::Type::LowShelf:
        b0 = a * (plus - minus * cosine + k2);
        b1 = 2.0 * a * (minus - plus * cosine);
        b2 = a * (plus - minus * cosine - k2);
        a0 = plus + minus * cosine + k2;
        a1 = -2.0 * (minus + plus * cosine);
        a2 = plus + minus * cosine - k2;
        break;
    case OutputEqFilter::Type::HighShelf:
        b0 = a * (plus + minus * cosine + k2);
        b1 = -2.0 * a * (minus + plus * cosine);
        b2 = a * (plus + minus * cosine - k2);
        a0 = plus - minus * cosine + k2;
        a1 = 2.0 * (minus - plus * cosine);
        a2 = plus - minus * cosine - k2;
        break;
    }

    const std::complex<double> z = std::exp(std::complex<double>(0.0, -2.0 * std::numbers::pi * frequency / sample_rate));
    const std::complex<double> numerator = b0 + b1 * z + b2 * z * z;
    const std::complex<double> denominator = a0 + a1 * z + a2 * z * z;
    return 20.0 * std::log10(std::max(std::abs(numerator / denominator), 1e-30));
}
