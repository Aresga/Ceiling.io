#include "dsp/EqChain.h"

#include <algorithm>
#include <cmath>

namespace ceilingIO::dsp
{
    void EqChain::prepare (double sampleRate, int samplesPerBlock, int numChannels)
    {
        sampleRate_  = sampleRate;
        numChannels_ = numChannels;

        chains_.clear();
        chains_.resize ((size_t) numChannels);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
        spec.numChannels      = 1;

        // HP: two identical 2nd-order high-pass sections at 30 Hz, Q = sqrt(2)
        // (4th-order Butterworth). Never parameterized — set once here.
        auto hpCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 30.0, std::sqrt (2.0f));

        // EQ defaults (gain = 1.0). Overwritten each block via updateCoefficients.
        auto loCoeffs  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sampleRate, 100.0f,  0.7071f, 1.0f);
        auto midCoeffs  = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f,   1.0f);
        auto hiCoeffs  = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 10000.0f, 0.7071f, 1.0f);

        for (auto& chain : chains_)
        {
            chain.prepare (spec);
            chain.get<0>().coefficients = hpCoeffs;
            chain.get<1>().coefficients = hpCoeffs;
            chain.get<2>().coefficients = loCoeffs;
            chain.get<3>().coefficients = midCoeffs;
            chain.get<4>().coefficients = hiCoeffs;
            chain.reset();
        }
    }

    void EqChain::updateCoefficients (double sampleRate, float loGain, float midGain, float hiGain)
    {
        if (sampleRate <= 0.0)
            return;

        auto newLow  = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sampleRate, 100.0f,   0.7071f, loGain);
        auto newMid  = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f,    midGain);
        auto newHigh = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 10000.0f, 0.7071f, hiGain);

        // Identical coefficients on every channel — equivalent to the former
        // ProcessorDuplicator sharing one .state across channels.
        for (auto& chain : chains_)
        {
            chain.get<2>().coefficients = newLow;
            chain.get<3>().coefficients = newMid;
            chain.get<4>().coefficients = newHigh;
        }
    }

    void EqChain::process (juce::AudioBuffer<float>& buffer)
    {
        const int numChannels = std::min (buffer.getNumChannels(), (int) chains_.size());
        const int numSamples  = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return;

        // Per-channel: hp1 -> hp2 -> eqLow -> eqMid -> eqHigh (ProcessorChain
        // runs stages in index order). Order matches the former processBlock.
        juce::dsp::AudioBlock<float> block (buffer);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto channelBlock = block.getSingleChannelBlock ((size_t) ch);
            juce::dsp::ProcessContextReplacing<float> ctx (channelBlock);
            chains_[(size_t) ch].process (ctx);
        }
    }
} // namespace ceilingIO::dsp