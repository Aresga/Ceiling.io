#include "atmos/AtmosLoudnessAnalyzer.h"
#include "dsp/ChannelWeights.h"

#include <algorithm>

namespace ceilingIO::atmos
{
    namespace
    {
        dsp::ChannelLayout layoutForBedCount (int bedChannels) noexcept
        {
            switch (bedChannels)
            {
                case 2:  return dsp::ChannelLayout::Stereo;
                case 6:  return dsp::ChannelLayout::Surround51;
                case 8:  return dsp::ChannelLayout::Atmos712;
                case 10: return dsp::ChannelLayout::Atmos714;
                default: break;
            }
            return dsp::ChannelLayout::Stereo;
        }
    }

    void AtmosLoudnessAnalyzer::prepare (double sampleRate, int samplesPerBlock, int bedChannels, int numObjects)
    {
        bedChannels_ = bedChannels;

        const dsp::ChannelLayout bedLayout = layoutForBedCount (bedChannels);
        bedAnalyzer_.prepare (sampleRate, samplesPerBlock, bedChannels, bedLayout);

        // Defensive: if the layout's weight table is shorter than the bed channel
        // count (unexpected layout), fall back to flat (nullptr) weighting to
        // avoid an out-of-bounds read in LoudnessAnalyzer::process.
        if (dsp::getChannelCount (bedLayout) < bedChannels)
            bedAnalyzer_.channelWeights = nullptr;

        objectAnalyzers_.clear();
        objectAnalyzers_.reserve ((size_t) std::max (0, numObjects));
        for (int i = 0; i < numObjects; ++i)
        {
            auto a = std::make_unique<ceilingIO::dsp::LoudnessAnalyzer>();
            a->prepare (sampleRate, samplesPerBlock, 1, dsp::ChannelLayout::Stereo);
            objectAnalyzers_.push_back (std::move (a));
        }
    }

    void AtmosLoudnessAnalyzer::processBedBlock (const juce::AudioBuffer<float>& bedBuffer)
    {
        bedAnalyzer_.process (bedBuffer);
    }

    void AtmosLoudnessAnalyzer::processObjectBlock (const juce::AudioBuffer<float>& objectBuffer, int objectIndex)
    {
        if (objectIndex >= 0 && objectIndex < (int) objectAnalyzers_.size())
            objectAnalyzers_[(size_t) objectIndex]->process (objectBuffer);
    }

    AtmosLoudnessResult AtmosLoudnessAnalyzer::getResult() const
    {
        AtmosLoudnessResult r;

        r.integratedLufsBed = bedAnalyzer_.integratedLufs.load();
        r.truePeakBedDbtp   = bedAnalyzer_.truePeakMaxDbtp.load();
        r.loudnessRange     = bedAnalyzer_.loudnessRange.load();

        // Combine objects in the linear mean-square domain.
        double objectMeanSquareSum = 0.0;
        float  objectTruePeak = -120.0f;
        bool   anyObject = false;
        for (const auto& obj : objectAnalyzers_)
        {
            const float lufs = obj->integratedLufs.load();
            if (lufs > -120.0f)
            {
                objectMeanSquareSum += dsp::lufsToMeanSquare (lufs);
                anyObject = true;
            }
            const float tp = obj->truePeakMaxDbtp.load();
            if (tp > objectTruePeak)
                objectTruePeak = tp;
        }

        if (anyObject)
            r.integratedLufsObjects = dsp::meanSquareToLufs (objectMeanSquareSum);
        r.truePeakObjectsDbtp = objectTruePeak;

        // Composite: bed + object energies summed in linear domain.
        const double bedMeanSquare = dsp::lufsToMeanSquare (r.integratedLufsBed);
        const double compositeMeanSquare = bedMeanSquare + objectMeanSquareSum;
        r.integratedLufsComposite = dsp::meanSquareToLufs (compositeMeanSquare);

        r.dialogueLufs = -120.0f; // not identified in the minimal parser
        return r;
    }

    void AtmosLoudnessAnalyzer::reset()
    {
        bedAnalyzer_.reset();
        for (auto& obj : objectAnalyzers_)
            obj->reset();
    }
} // namespace ceilingIO::atmos