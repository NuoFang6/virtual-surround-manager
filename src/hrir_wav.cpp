#include "hrir_wav.h"

#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

bool HrirWav::read(const QString &path, HrirWav &wav, QString *error) {
    wav = {};
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    const QByteArray header = file.read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.sliced(8, 4) != "WAVE")
        return fail(QStringLiteral("不是 RIFF/WAVE 文件"));
    const qint64 end = 8 + static_cast<qint64>(qFromLittleEndian<uint32_t>(header.constData() + 4));
    if (end < 12 || end > file.size())
        return fail(QStringLiteral("WAV 文件已截断"));

    HrirWav result;
    qint64 data_size = -1;
    bool have_format = false;
    for (qint64 offset = 12; offset < end;) {
        if (end - offset < 8 || !file.seek(offset))
            return fail(QStringLiteral("WAV 区块头不完整"));
        const QByteArray chunk = file.read(8);
        if (chunk.size() != 8)
            return fail(QStringLiteral("无法读取 WAV 区块"));
        const qint64 size = qFromLittleEndian<uint32_t>(chunk.constData() + 4);
        if (size > end - offset - 8)
            return fail(QStringLiteral("WAV 区块已截断"));
        const auto id = chunk.first(4);
        if (id == "fmt ") {
            if (have_format || size < 16)
                return fail(QStringLiteral("WAV 格式区块无效"));
            const QByteArray format = file.read(std::min<qint64>(size, 40));
            if (format.size() < 16)
                return fail(QStringLiteral("WAV 格式区块已截断"));
            result.format = qFromLittleEndian<uint16_t>(format.constData());
            result.channels = qFromLittleEndian<uint16_t>(format.constData() + 2);
            result.rate = qFromLittleEndian<uint32_t>(format.constData() + 4);
            result.block_align = qFromLittleEndian<uint16_t>(format.constData() + 12);
            result.bits = qFromLittleEndian<uint16_t>(format.constData() + 14);
            if (result.format == 0xfffe) {
                if (format.size() < 40)
                    return fail(QStringLiteral("WAV 扩展格式不完整"));
                const uint16_t extension = qFromLittleEndian<uint16_t>(format.constData() + 16);
                const uint16_t valid_bits = qFromLittleEndian<uint16_t>(format.constData() + 18);
                const uint32_t subformat = qFromLittleEndian<uint32_t>(format.constData() + 24);
                const QByteArray guid_tail = QByteArray::fromHex("00001000800000aa00389b71");
                if (extension < 22 || 18 + extension > size || valid_bits == 0 || valid_bits > result.bits ||
                    format.sliced(28, 12) != guid_tail || (subformat != 1 && subformat != 3) ||
                    (subformat == 3 && valid_bits != 32))
                    return fail(QStringLiteral("不支持的 WAV 扩展格式"));
                result.format = static_cast<uint16_t>(subformat);
            }
            const bool supported = (result.format == 3 && result.bits == 32) ||
                (result.format == 1 && (result.bits == 16 || result.bits == 24 || result.bits == 32));
            if (!supported || result.channels != 14 || result.rate == 0 ||
                result.block_align != result.channels * (result.bits / 8))
                return fail(QStringLiteral("必须是有效的 14 声道 PCM16/24/32 或 float32 WAV"));
            const uint64_t byte_rate = static_cast<uint64_t>(result.rate) * result.block_align;
            if (byte_rate != qFromLittleEndian<uint32_t>(format.constData() + 8))
                return fail(QStringLiteral("WAV 字节率与帧布局不一致"));
            have_format = true;
        } else if (id == "data") {
            if (data_size >= 0)
                return fail(QStringLiteral("WAV 包含多个音频数据区块"));
            result.data_offset = offset + 8;
            data_size = size;
        }
        offset += 8 + size;
        if ((size & 1) && offset < end)
            ++offset;
    }
    if (!have_format || data_size <= 0 || data_size % result.block_align != 0)
        return fail(QStringLiteral("WAV 音频数据不包含完整帧"));
    result.frames = static_cast<uint32_t>(data_size / result.block_align);
    // Preserve the existing ten-second HRIR budget, but reject instead of
    // silently truncating only the LFE branch. Bound allocation for corrupt input.
    if (result.frames > static_cast<uint64_t>(result.rate) * 10 || data_size > 256 * 1024 * 1024)
        return fail(QStringLiteral("HRIR 超出 10 秒或 256 MiB 的处理上限"));
    if (!file.seek(result.data_offset))
        return fail(QStringLiteral("无法定位 WAV 音频数据"));
    result.samples = file.read(data_size);
    if (result.samples.size() != data_size)
        return fail(QStringLiteral("WAV 音频数据已截断"));
    if (result.format == 3) {
        for (uint32_t frame = 0; frame < result.frames; ++frame)
            for (uint32_t channel = 0; channel < result.channels; ++channel)
                if (!std::isfinite(result.sample(frame, channel)))
                    return fail(QStringLiteral("HRIR 包含 NaN 或无穷大样本"));
    }
    wav = std::move(result);
    if (error)
        error->clear();
    return true;
}

double HrirWav::sample(uint32_t frame, uint32_t channel) const {
    if (frame >= frames || channel >= channels)
        return 0.0;
    const char *p = samples.constData() + static_cast<qsizetype>(frame) * block_align + channel * (bits / 8);
    if (format == 3)
        return std::bit_cast<float>(qFromLittleEndian<uint32_t>(p));
    if (bits == 16)
        return qFromLittleEndian<int16_t>(p) / 32768.0;
    if (bits == 32)
        return qFromLittleEndian<int32_t>(p) / 2147483648.0;
    int32_t value = static_cast<uint8_t>(p[0]) | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16);
    if (value & 0x800000)
        value -= 0x1000000;
    return value / 8388608.0;
}
