#pragma once

#include <JuceHeader.h>

#include <vector>

namespace ceilingIO::dsp
{
    // Tonal chain: high-pass (two cascaded 2nd-order sections, 30 Hz Butterworth)
    // plus three EQ bands (low shelf, mid peak, high shelf). Extracted from
    // MainAudioProcessor's hp1/hp2/eqLow/eqMid/eqHigh ProcessorDuplicators.
    //
    // Uses per-channel ProcessorChain instances instead of ProcessorDuplicator.
    // For stereo this is bit-for-bit equivalent: each channel's IIR filter has
    // independent state and identical coefficients, so the channel grouping
    // (per-channel vs duplicator) does not affect the result.
    class EqChain
    {
    public:
        void prepare (double sampleRate, int samplesPerBlock, int numChannels);
        // Rebuild the three EQ band coefficients and apply to every channel.
        // HP coefficients are fixed (set once in prepare). Gains are linear.
        void updateCoefficients (double sampleRate, float loGain, float midGain, float hiGain);
        void process (juce::AudioBuffer<float>& buffer);

    private:
        using Filter = juce::dsp::IIR::Filter<float>;
        // Stage order: 0=hp1, 1=hp2, 2=eqLow, 3=eqMid, 4=eqHigh
        using Chain  = juce::dsp::ProcessorChain<Filter, Filter, Filter, Filter, Filter>;

        std::vector<Chain> chains_;
        int    numChannels_ = 0;
        double sampleRate_  = 44100.0;
    };
} // namespace ceilingIO::dsp