#pragma once

#include <cmath>

namespace ceilingIO::dsp
{
    // BS.1770-4 / EBU R128 loudness math.
    // Consolidated here from the former anonymous-namespace duplicates in
    // ceilingIOPipeline.cpp and MainAudioProcessor.cpp. The functions are
    // byte-for-byte identical to the originals so existing behaviour is
    // preserved exactly.
    constexpr float kLufsOffset      = -0.691f;
    constexpr float kAbsoluteGateLufs = -70.0f;

    // Takes raw audio and converts it into a decibel value in LUFS, EBU R128
    inline float meanSquareToLufs (double meanSquare)
    {
        if (! std::isfinite (meanSquare) || meanSquare <= 1.0e-12)
            return -120.0f;
        return kLufsOffset + 10.0f * std::log10 (static_cast<float> (meanSquare));
    }

    // Does the opposite of meanSquareToLufs, converting a LUFS value back to a
    // linear mean square value.
    inline double lufsToMeanSquare (float lufs)
    {
        return std::pow (10.0, (lufs - kLufsOffset) / 10.0f);
    }
} // namespace ceilingIO::dsp