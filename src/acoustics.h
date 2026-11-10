#pragma once

#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace Acoustics {

// Phase is a circular quantity: 359 degrees is a one-degree error, not 359.
inline double phase_error(double radians) {
    return std::remainder(radians, 2.0 * std::numbers::pi);
}

inline double relative_phase(std::complex<double> numerator, std::complex<double> denominator) {
    if (std::abs(numerator) == 0.0 || std::abs(denominator) == 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return phase_error(std::arg(numerator) - std::arg(denominator));
}

// IEC-style A-weighting magnitude squared, normalized to 0 dB at 1 kHz.
// This is a frequency weighting, not a programme-loudness (LUFS) algorithm.
inline double a_weight_power(double frequency) {
    const auto raw = [](double f) {
        const double f2 = f * f;
        return (12194.0 * 12194.0 * f2 * f2) /
            ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) *
             (f2 + 12194.0 * 12194.0));
    };
    const double amplitude = raw(frequency) / raw(1000.0);
    return amplitude * amplitude;
}

} // namespace Acoustics
