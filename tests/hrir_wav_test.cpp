#include "hrir_wav.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

static void append16(QByteArray &data, uint16_t value) {
    char bytes[2]; qToLittleEndian(value, bytes); data.append(bytes, 2);
}
static void append32(QByteArray &data, uint32_t value) {
    char bytes[4]; qToLittleEndian(value, bytes); data.append(bytes, 4);
}
static QByteArray make_wav(uint16_t tag, uint16_t bits, bool extensible = false) {
    QByteArray fmt;
    append16(fmt, extensible ? 0xfffe : tag); append16(fmt, 14);
    append32(fmt, 48000); append32(fmt, 48000 * 14 * (bits / 8));
    append16(fmt, 14 * (bits / 8)); append16(fmt, bits);
    if (extensible) {
        append16(fmt, 22); append16(fmt, 24); append32(fmt, 0);
        append32(fmt, tag); fmt += QByteArray::fromHex("00001000800000aa00389b71");
    }
    QByteArray samples;
    for (int c = 0; c < 14; ++c) {
        if (tag == 3) append32(samples, std::bit_cast<uint32_t>(0.25f));
        else if (bits == 16) append16(samples, 8192);
        else if (bits == 24) samples += QByteArray::fromHex("000020");
        else append32(samples, 0x20000000);
    }
    QByteArray result("RIFF", 4);
    append32(result, 4 + 8 + fmt.size() + 8 + samples.size());
    result += "WAVEfmt "; append32(result, fmt.size()); result += fmt;
    result += "data"; append32(result, samples.size()); result += samples;
    return result;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    int failures = 0;
    const auto check = [&](const char *name, QByteArray bytes, bool expected, double sample = 0.25) {
        const QString path = dir.filePath(QString::fromLatin1(name));
        QFile file(path); file.open(QIODevice::WriteOnly); file.write(bytes); file.close();
        HrirWav wav; QString error;
        const bool accepted = HrirWav::read(path, wav, &error);
        bool ok = accepted == expected;
        if (accepted && expected)
            for (uint32_t c = 0; c < 14; ++c) ok &= std::abs(wav.sample(0, c) - sample) < 1e-7;
        std::printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, qPrintable(error));
        if (!ok) ++failures;
    };
    check("pcm16", make_wav(1,16), true);
    check("pcm24", make_wav(1,24), true);
    check("pcm32", make_wav(1,32), true);
    check("float32", make_wav(3,32), true);
    check("24-in-32", make_wav(1,32,true), true);
    QByteArray bytes = make_wav(3,32);
    bytes.truncate(22); qToLittleEndian<uint32_t>(14,bytes.data()+4);
    check("truncated-fmt",bytes,false);
    bytes=make_wav(3,32); qToLittleEndian<uint16_t>(1,bytes.data()+32);
    check("bad-align",bytes,false);
    bytes=make_wav(3,32); bytes.chop(1);
    check("truncated-data",bytes,false);
    bytes=make_wav(3,32); qToLittleEndian<uint16_t>(7,bytes.data()+22);
    check("seven-channels",bytes,false);
    bytes=make_wav(3,32); qToLittleEndian(std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()),bytes.data()+44);
    check("nan",bytes,false);
    bytes=make_wav(3,32); qToLittleEndian(std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()),bytes.data()+44);
    check("infinity",bytes,false);
    bytes=make_wav(1,32,true); bytes[52]='x';
    check("bad-guid",bytes,false);
    std::printf("%d failures\n",failures);
    return failures ? 1 : 0;
}
