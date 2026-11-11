#!/usr/bin/env python3
"""File-domain acoustic checks. Requires NumPy; never opens an audio device.

python3 -I tools/analyze_acoustics.py src/assets/hrir_wav > measurements.json
The seven-direction average below reproduces the legacy `diffuse` timbre;
it is deliberately not called a physical diffuse-field response.
"""
import argparse
import json
import math
from pathlib import Path
import struct

import numpy as np

LEFT = [0, 8, 6, 4, 12, 2, 10]
RIGHT = [1, 7, 13, 5, 11, 3, 9]
NATIVE_LFE_GAIN = 2.0


def read_wav(path):
    blob = path.read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not RIFF/WAVE")
    offset, fmt, data = 12, None, None
    while offset + 8 <= len(blob):
        label, size = struct.unpack_from("<4sI", blob, offset)
        body = blob[offset + 8:offset + 8 + size]
        if len(body) != size:
            raise ValueError("truncated chunk")
        if label == b"fmt ":
            fmt = body
        elif label == b"data":
            data = body
        offset += 8 + size + size % 2
    if fmt is None or data is None:
        raise ValueError("missing format/data")
    tag, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 0xfffe:
        tag = struct.unpack_from("<I", fmt, 24)[0]
    if align != channels * (bits // 8):
        raise ValueError("invalid frame layout")
    if tag == 3 and bits == 32:
        samples = np.frombuffer(data, dtype="<f4").astype(float)
    elif tag == 1 and bits in (16, 32):
        samples = np.frombuffer(data, dtype=f"<i{bits // 8}").astype(float) / 2 ** (bits - 1)
    elif tag == 1 and bits == 24:
        v = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        samples = v[:, 0] + (v[:, 1] << 8) + (v[:, 2] << 16)
        samples = np.where(samples & 0x800000, samples - 0x1000000, samples) / 8388608.0
    else:
        raise ValueError("unsupported encoding")
    if not np.isfinite(samples).all():
        raise ValueError("non-finite samples")
    return rate, samples.reshape(-1, channels)


def response(samples, rate, frequencies):
    result = np.zeros((len(frequencies), samples.shape[1]), complex)
    for start in range(0, len(samples), 2048):
        n = np.arange(start, min(start + 2048, len(samples)))
        result += np.exp(-2j * np.pi * frequencies[:, None] * n / rate) @ samples[start:start + len(n)]
    return result


def biquad(frequencies, cutoff, q, allpass=False):
    rate = 48000
    w = 2 * np.pi * cutoff / rate
    z = np.exp(-2j * np.pi * frequencies / rate)
    alpha = np.sin(w) / (2 * q)
    a = (1 + alpha) - 2 * np.cos(w) * z + (1 - alpha) * z * z
    b = ((1 - alpha) - 2 * np.cos(w) * z + (1 + alpha) * z * z if allpass else
         (1 - np.cos(w)) / 2 * (1 + 2 * z + z * z))
    return b / a


def lowpass(frequencies):
    return biquad(frequencies, 120, .541196100146197) * biquad(frequencies, 120, 1.306562964876377)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hrir_directory", type=Path)
    args = parser.parse_args()
    f = np.arange(20., 121.)
    ap = biquad(f, 5790.059175648786, .14251026703029981, allpass=True)
    corrected = lowpass(f) * ap * np.exp(2j * np.pi * f * 206 / 48000)
    assert np.max(np.abs(np.abs(ap) - 1)) < 1e-10
    assert abs(20 * np.log10(abs(lowpass(np.array([120.]))[0])) + 3.01029995664) < 1e-8
    assert abs(np.angle(np.exp(1j * math.radians(359))) - math.radians(-1)) < 1e-12

    rows = []
    for path in sorted(args.hrir_directory.glob("*.wav")):
        rate, samples = read_wav(path)
        if samples.shape[1] != 14:
            continue
        h = response(samples, rate, f)
        h_ear = np.stack([h[:, LEFT], h[:, RIGHT]], axis=1)
        mean = h_ear.mean(axis=2)
        reference = np.abs(h[::10][2:7][:, [6, 13]]).mean()
        timbre = mean * reference / np.abs(mean[::10][2:7]).mean()
        row = {"file": path.name, "rate": rate, "frames": len(samples), "reference_gain": float(reference)}
        row["native_lfe_vs_center_power_db_at_60"] = float(10 * np.log10(
            np.sum(np.abs(NATIVE_LFE_GAIN * lowpass(np.array([60.])) * timbre[40]) ** 2) /
            np.sum(np.abs(h[40, [6, 13]]) ** 2)))
        reference_indices = np.arange(20, 61, 10)
        row["native_lfe_vs_center_power_db_40_80"] = float(10 * np.log10(
            np.sum(np.abs(NATIVE_LFE_GAIN * lowpass(f[reference_indices])[:, None] * timbre[reference_indices]) ** 2) /
            np.sum(np.abs(h[reference_indices][:, [6, 13]]) ** 2)))
        row["coherent_vs_directional_power_db_at_60"] = float(10 * np.log10(
            np.mean(np.abs(mean[40]) ** 2) / np.mean(np.abs(h[40]) ** 2)))
        for send, name in [(10 ** (-6 / 20), "balanced"), (1., "impact")]:
            # Basis-channel diagnostic, not a claim that FreeSurround emits all
            # possible basis vectors or that arbitrary mixtures are covered.
            ratio = np.abs(h_ear + send * corrected[:, None, None] * timbre[:, :, None]) / np.maximum(np.abs(h_ear), 1e-20)
            row[name + "_min_directional_ear_ratio_db"] = float(20 * np.log10(np.min(ratio)))
        rows.append(row)
    n = np.arange(4096)
    x = .84 * np.cos(np.pi / 2 * n + np.pi / 4) / np.sqrt(.5)
    y = np.fft.irfft(np.fft.rfft(x), n=len(x) * 16) * 16
    result = {
        "model": "48k BW4; coherent-seven IR; 206 sample main delay; measured production all-pass",
        "bw4_db": {str(freq): float(20 * np.log10(abs(lowpass(np.array([freq]))[0]))) for freq in [40, 60, 80, 100, 120, 240]},
        "allpass_max_magnitude_error": float(np.max(np.abs(np.abs(ap) - 1))),
        "reference_phase_rms_deg_dense": float(np.sqrt(np.mean(np.degrees(np.angle(corrected)) ** 2))),
        "allpass_group_delay_ms_range": [float(v) for v in [np.min(-np.diff(np.unwrap(np.angle(ap))) / (2 * np.pi) * 1000), np.max(-np.diff(np.unwrap(np.angle(ap))) / (2 * np.pi) * 1000)]],
        "sample_vs_reconstructed_peak": [float(np.max(np.abs(x))), float(np.max(np.abs(y)))],
        "profiles": rows,
    }
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
