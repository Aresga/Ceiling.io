#include "dsp/LoudnessAnalyzer.h"
#include "dsp/LufsMath.h"

#include <algorithm>
#include <cmath>

namespace ceilingIO::dsp
{
    void LoudnessAnalyzer::prepare (double newSampleRate, int samplesPerBlock, int channels, ChannelLayout layoutToUse)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        numChannels = std::max (1, channels);

        layout = layoutToUse;
        channelWeights = getChannelWeights (layout);

        momentaryWindowSamples = std::max (1, (int) std::ceil (0.4 * sampleRate));
        shortTermWindowSamples = std::max (1, (int) std::ceil (3.0 * sampleRate));
        samplesSinceShortTermUpdate = 0;

        momentaryRing.assign ((size_t) momentaryWindowSamples, 0.0f);
        shortTermRing.assign ((size_t) shortTermWindowSamples, 0.0f);
        momentaryIndex = 0;
        shortTermIndex = 0;
        momentarySamplesFilled = 0;
        shortTermSamplesFilled = 0;
        momentarySum = 0.0;
        shortTermSum = 0.0;

        historySize = 600;
        shortTermHistoryEnergy.assign ((size_t) historySize, 0.0f);
        historyIndex = 0;
        historyCount = 0;
        percentileWorkspace.assign ((size_t) historySize, 0.0f);

        analysisBuffer.setSize (numChannels, samplesPerBlock, false, false, true);

        kWeightChains.clear();
        kWeightChains.resize ((size_t) numChannels);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
        spec.numChannels = 1;

        auto hpCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 40.0, 0.5f);
        auto shelfCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
            sampleRate, 1500.0, 0.7071f, juce::Decibels::decibelsToGain (4.0f));

        for (auto& chain : kWeightChains)
        {
            chain.prepare (spec);
            chain.get<0>().coefficients = hpCoeffs;
            chain.get<1>().coefficients = shelfCoeffs;
            chain.reset();
        }

        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            numChannels, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false);
        oversampler->initProcessing ((juce::uint32) samplesPerBlock);
        oversampler->reset();

        integratedLufs.store (-120.0f);
        shortTermLufs.store (-120.0f);
        momentaryLufs.store (-120.0f);
        loudnessRange.store (0.0f);
        truePeakMaxDbtp.store (-120.0f);
    }

    void LoudnessAnalyzer::reset()
    {
        momentaryRing.assign (momentaryRing.size(), 0.0f);
        shortTermRing.assign (shortTermRing.size(), 0.0f);
        momentaryIndex = 0;
        shortTermIndex = 0;
        momentarySamplesFilled = 0;
        shortTermSamplesFilled = 0;
        momentarySum = 0.0;
        shortTermSum = 0.0;
        samplesSinceShortTermUpdate = 0;
        historyIndex = 0;
        historyCount = 0;
        std::fill (shortTermHistoryEnergy.begin(), shortTermHistoryEnergy.end(), 0.0f);
        integratedLufs.store (-120.0f);
        shortTermLufs.store (-120.0f);
        momentaryLufs.store (-120.0f);
        loudnessRange.store (0.0f);
        truePeakMaxDbtp.store (-120.0f);
        if (oversampler != nullptr)
            oversampler->reset();
        for (auto& chain : kWeightChains)
            chain.reset();
    }

    void LoudnessAnalyzer::process (const juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0)
            return;

        if (numSamples > analysisBuffer.getNumSamples())
            return;

        const int copyChannels = std::min (buffer.getNumChannels(), analysisBuffer.getNumChannels());
        for (int ch = 0; ch < copyChannels; ++ch)
            analysisBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        for (int ch = copyChannels; ch < analysisBuffer.getNumChannels(); ++ch)
            analysisBuffer.clear (ch, 0, numSamples);

        if (oversampler != nullptr)
        {
            juce::dsp::AudioBlock<float> baseBlock (analysisBuffer);
            auto upBlock = oversampler->processSamplesUp (baseBlock);
            float truePeak = 0.0f;
            for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
            {
                const float* data = upBlock.getChannelPointer (ch);
                for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
                    truePeak = std::max (truePeak, std::abs (data[i]));
            }
            oversampler->processSamplesDown (baseBlock);
            truePeakMaxDbtp.store (juce::Decibels::gainToDecibels (truePeak, -120.0f));
        }

        juce::dsp::AudioBlock<float> block (analysisBuffer);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto channelBlock = block.getSingleChannelBlock ((size_t) ch);
            juce::dsp::ProcessContextReplacing<float> ctx (channelBlock);
            kWeightChains[(size_t) ch].process (ctx);
        }

        const float* channelPtrs[64] = {};
        const int chCount = std::min (numChannels, 64);
        for (int ch = 0; ch < chCount; ++ch)
            channelPtrs[ch] = analysisBuffer.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            float sumSquares = 0.0f;
            for (int ch = 0; ch < chCount; ++ch)
            {
                const float v = channelPtrs[ch][i];
                // BS.1770-4 channel weighting. For Stereo the weights are
                // {1.0, 1.0}, so `w * v * v == v * v` exactly (IEEE multiply by
                // 1.0 is identity) — bit-for-bit identical to the old flat sum.
                const float w = (ch < numChannels && channelWeights != nullptr) ? channelWeights[ch] : 1.0f;
                sumSquares += w * v * v;
            }

            if (momentarySamplesFilled < momentaryWindowSamples)
                ++momentarySamplesFilled;
            momentarySum -= momentaryRing[(size_t) momentaryIndex];
            momentaryRing[(size_t) momentaryIndex] = sumSquares;
            momentarySum += sumSquares;
            ++momentaryIndex;
            if (momentaryIndex >= momentaryWindowSamples)
                momentaryIndex = 0;

            if (shortTermSamplesFilled < shortTermWindowSamples)
                ++shortTermSamplesFilled;
            shortTermSum -= shortTermRing[(size_t) shortTermIndex];
            shortTermRing[(size_t) shortTermIndex] = sumSquares;
            shortTermSum += sumSquares;
            ++shortTermIndex;
            if (shortTermIndex >= shortTermWindowSamples)
                shortTermIndex = 0;
        }

        const int momentaryDenom = std::max (1, momentarySamplesFilled * numChannels);
        const int shortTermDenom = std::max (1, shortTermSamplesFilled * numChannels);
        const double momentaryMean = momentarySum / (double) momentaryDenom;
        const double shortTermMean = shortTermSum / (double) shortTermDenom;

        momentaryLufs.store (meanSquareToLufs (momentaryMean));
        shortTermLufs.store (meanSquareToLufs (shortTermMean));

        samplesSinceShortTermUpdate += numSamples;
        const int updateInterval = (int) std::max (1.0, sampleRate);
        while (samplesSinceShortTermUpdate >= updateInterval)
        {
            samplesSinceShortTermUpdate -= updateInterval;
            shortTermHistoryEnergy[(size_t) historyIndex] = (float) shortTermMean;
            historyIndex = (historyIndex + 1) % historySize;
            historyCount = std::min (historyCount + 1, historySize);

            double absGateEnergy = lufsToMeanSquare (kAbsoluteGateLufs);
            double ungatedSum = 0.0;
            int ungatedCount = 0;
            for (int i = 0; i < historyCount; ++i)
            {
                const double e = shortTermHistoryEnergy[(size_t) i];
                if (e > absGateEnergy)
                {
                    ungatedSum += e;
                    ++ungatedCount;
                }
            }

            float integrated = -120.0f;
            if (ungatedCount > 0)
            {
                const double ungatedMean = ungatedSum / (double) ungatedCount;
                const float ungatedLufs = meanSquareToLufs (ungatedMean);
                const float relativeGateLufs = ungatedLufs - 10.0f;
                const double relativeGateEnergy = lufsToMeanSquare (relativeGateLufs);
                const double gateEnergy = std::max (absGateEnergy, relativeGateEnergy);

                double gatedSum = 0.0;
                int gatedCount = 0;
                for (int i = 0; i < historyCount; ++i)
                {
                    const double e = shortTermHistoryEnergy[(size_t) i];
                    if (e > gateEnergy)
                    {
                        gatedSum += e;
                        ++gatedCount;
                    }
                }

                if (gatedCount > 0)
                    integrated = meanSquareToLufs (gatedSum / (double) gatedCount);
            }

            integratedLufs.store (integrated);

            int lraCount = 0;
            for (int i = 0; i < historyCount; ++i)
            {
                const double e = shortTermHistoryEnergy[(size_t) i];
                if (e > absGateEnergy)
                {
                    percentileWorkspace[(size_t) lraCount] = meanSquareToLufs (e);
                    ++lraCount;
                }
            }

            if (lraCount >= 2)
            {
                const int idx10 = (int) std::floor (0.1f * (lraCount - 1));
                const int idx95 = (int) std::floor (0.95f * (lraCount - 1));
                std::nth_element (percentileWorkspace.begin(), percentileWorkspace.begin() + idx10,
                                  percentileWorkspace.begin() + lraCount);
                const float p10 = percentileWorkspace[(size_t) idx10];
                std::nth_element (percentileWorkspace.begin(), percentileWorkspace.begin() + idx95,
                                  percentileWorkspace.begin() + lraCount);
                const float p95 = percentileWorkspace[(size_t) idx95];
                loudnessRange.store (std::max (0.0f, p95 - p10));
            }
            else
            {
                loudnessRange.store (0.0f);
            }
        }
    }
} // namespace ceilingIO::dsp