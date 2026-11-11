#pragma once

#include <QString>
#include <QVector>

struct OutputEqFilter {
    enum class Type {
        Peaking,
        LowShelf,
        HighShelf,
    };

    Type type = Type::Peaking;
    double frequency = 0.0;
    double q = 1.0;
    double gain_db = 0.0;
};

struct OutputEqConfig {
    bool enabled = false;
    QString source_path;
    double file_preamp_db = 0.0;
    double reference_level_db = 0.0;
    double static_gain_db = 0.0;
    double automatic_gain_db = 0.0;
    double maximum_filter_gain_db = 0.0;
    double maximum_output_gain_db = 0.0;
    QVector<OutputEqFilter> filters;
};

class OutputEqualizer {
  public:
    static bool read(const QString &path, OutputEqConfig &config, QString *error = nullptr);
    static QString filter_type_name(OutputEqFilter::Type type);

  private:
    static double response_db(const OutputEqFilter &filter, double frequency, double sample_rate);
};
