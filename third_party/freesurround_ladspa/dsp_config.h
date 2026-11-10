#pragma once

namespace VsmDsp {
inline constexpr int fft_size = 4096;
inline constexpr int upmix_latency = fft_size + fft_size / 2;
// Historical LFE send levels (-6 dB / 0 dB) for the mono main-sum send. The
// send is rendered through the diffuse LFE path, not as a shelf on the main
// HRIRs, so it keeps the non-frontal LFE character.
inline constexpr float balanced_lfe_send = 0.501f;
inline constexpr float impact_lfe_send = 1.0f;
}
