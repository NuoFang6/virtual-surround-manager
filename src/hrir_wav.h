#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>

// One validated HeSuVi impulse response. Container width determines sample
// addressing; WAVE_FORMAT_EXTENSIBLE valid bits do not change the frame layout.
struct HrirWav {
    QByteArray samples;
    uint32_t rate = 0;
    uint32_t frames = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t format = 0;
    uint16_t block_align = 0;
    qint64 data_offset = 0;

    static bool read(const QString &path, HrirWav &wav, QString *error = nullptr);
    double sample(uint32_t frame, uint32_t channel) const;
};
