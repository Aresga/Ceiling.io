#pragma once

#include <JuceHeader.h>
#include "dsp/LoudnessAnalyzer.h"
#include "dsp/LufsMath.h"

#include <memory>
#include <vector>

namespace ceilingIO::atmos
{
    struct AtmosLoudnessResult
    {
        float integratedLufsBed       = -120.0f; // bed channels only (BS.1770 weighted)
        float integratedLufsObjects   = -120.0f; // all objects summed with equal weight
        float integratedLufsComposite = -120.0f; // bed + objects combined
        float dialogueLufs            = -120.0f; // -120 in minimal parser (no dialogue tag)
        float truePeakBedDbtp         = -120.0f;
        float truePeakObjectsDbtp     = -120.0f;
        float loudnessRange           = 0.0f;
    };

    // Composite loudness/true-peak analyzer for an ADM bed + object split.
    // The bed is analyzed with BS.1770-4 channel weighting for its layout;
    // each object is analyzed as a mono stream. Results are combined in linear
    // mean-square domain.
    //
    // LoudnessAnalyzer holds std::atomic<float> members (non-copyable,
    // non-movable), so the per-object analyzers are stored behind unique_ptr
    // to keep this class movable/clearable.
    class AtmosLoudnessAnalyzer
    {
    public:
        void prepare (double sampleRate, int samplesPerBlock, int bedChannels, int numObjects);
        void processBedBlock (const juce::AudioBuffer<float>& bedBuffer);
        void processObjectBlock (const juce::AudioBuffer<float>& objectBuffer, int objectIndex);
        AtmosLoudnessResult getResult() const;
        void reset();

    private:
        ceilingIO::dsp::LoudnessAnalyzer bedAnalyzer_;
        std::vector<std::unique_ptr<ceilingIO::dsp::LoudnessAnalyzer>> objectAnalyzers_;
        int bedChannels_ = 0;
    };
} // namespace ceilingIO::atmos