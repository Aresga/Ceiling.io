#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelWeights.h"

#include <atomic>
#include <memory>
#include <vector>

namespace ceilingIO::dsp
{
    // EBU R128 / ITU-R BS.1770 loudness + true-peak analyzer.
    //
    // Extracted verbatim from the former MainAudioProcessor::LoudnessAnalyzer
    // nested struct. The only behavioural addition is BS.1770-4 channel
    // weighting in process(): for ChannelLayout::Stereo the weights are
    // {1.0, 1.0}, so `w * v * v == v * v` exactly — the stereo output is
    // bit-for-bit identical to the previous flat summation.
    //
    // Thread safety: atomics are written on the audio thread (process) and
    // read from the message thread (getters). No allocations happen in
    // process(); all buffers are preallocated in prepare().
    class LoudnessAnalyzer
    {
    public:
        void prepare (double sampleRate, int samplesPerBlock, int numChannels, ChannelLayout layout = ChannelLayout::Stereo);
        void reset();
        void process (const juce::AudioBuffer<float>& buffer);

        std::atomic<float> integratedLufs { -120.0f };
        std::atomic<float> shortTermLufs   { -120.0f };
        std::atomic<float> momentaryLufs   { -120.0f };
        std::atomic<float> loudnessRange   { 0.0f };
        std::atomic<float> truePeakMaxDbtp { -120.0f };

        double sampleRate = 44100.0;
        int    numChannels = 2;

        int momentaryWindowSamples = 0;
        int shortTermWindowSamples = 0;
        int samplesSinceShortTermUpdate = 0;

        std::vector<float> momentaryRing;
        std::vector<float> shortTermRing;
        int momentaryIndex = 0;
        int shortTermIndex = 0;
        int momentarySamplesFilled = 0;
        int shortTermSamplesFilled = 0;
        double momentarySum = 0.0;
        double shortTermSum = 0.0;

        int historySize = 600;
        std::vector<float> shortTermHistoryEnergy;
        int historyIndex = 0;
        int historyCount = 0;
        std::vector<float> percentileWorkspace;

        juce::AudioBuffer<float> analysisBuffer;
        std::vector<juce::dsp::ProcessorChain<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Filter<float>>> kWeightChains;
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

        // BS.1770-4 channel weighting (set in prepare from the layout)
        ChannelLayout layout = ChannelLayout::Stereo;
        const float* channelWeights = nullptr;
    };
} // namespace ceilingIO::dsp