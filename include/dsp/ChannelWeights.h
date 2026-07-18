#pragma once

#include <array>

namespace ceilingIO::dsp
{
    // ITU-R BS.1770-4 channel weighting factors.
    // L = 1.0, R = 1.0, C = 1.0, LFE = 0.0 (excluded from loudness),
    // Ls/Rs = 1.41 (surround +3 dB), top layers = 1.41.
    //
    // For stereo, indices 0 and 1 both map to 1.0 — identical to the previous
    // flat summation, so this change is fully backward-compatible for the
    // existing stereo path.

    enum class ChannelLayout
    {
        Stereo,      // L, R
        Surround51,  // L, R, C, LFE, Ls, Rs
        Atmos712,    // L, R, C, LFE, Ls, Rs, Ltf, Rtf
        Atmos714     // L, R, C, LFE, Ls, Rs, Ltf1, Rtf1, Ltf2, Rtf2
    };

    // Stereo: {1.0, 1.0}
    inline constexpr float kStereoWeights[]    = { 1.0f, 1.0f };

    // 5.1: L, R, C, LFE, Ls, Rs
    inline constexpr float kSurround51Weights[] = { 1.0f, 1.0f, 1.0f, 0.0f, 1.41f, 1.41f };

    // 7.1.2: L, R, C, LFE, Ls, Rs, Ltf, Rtf
    inline constexpr float kAtmos712Weights[]   = { 1.0f, 1.0f, 1.0f, 0.0f, 1.41f, 1.41f, 1.41f, 1.41f };

    // 7.1.4: L, R, C, LFE, Ls, Rs, Ltf1, Rtf1, Ltf2, Rtf2
    inline constexpr float kAtmos714Weights[]   = { 1.0f, 1.0f, 1.0f, 0.0f, 1.41f, 1.41f, 1.41f, 1.41f, 1.41f, 1.41f };

    inline const float* getChannelWeights (ChannelLayout layout) noexcept
    {
        switch (layout)
        {
            case ChannelLayout::Stereo:    return kStereoWeights;
            case ChannelLayout::Surround51: return kSurround51Weights;
            case ChannelLayout::Atmos712:  return kAtmos712Weights;
            case ChannelLayout::Atmos714:  return kAtmos714Weights;
        }
        return kStereoWeights;
    }

    inline int getChannelCount (ChannelLayout layout) noexcept
    {
        switch (layout)
        {
            case ChannelLayout::Stereo:    return 2;
            case ChannelLayout::Surround51: return 6;
            case ChannelLayout::Atmos712:  return 8;
            case ChannelLayout::Atmos714:  return 10;
        }
        return 2;
    }
} // namespace ceilingIO::dsp