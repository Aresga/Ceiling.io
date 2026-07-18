#pragma once

#include <JuceHeader.h>
#include "dsp/LookaheadLimiter.h"
#include "dsp/EqChain.h"
#include "atmos/AdmBwfReader.h"
#include "atmos/AdmBwfWriter.h"
#include "atmos/AtmosLoudnessAnalyzer.h"

#include <vector>

namespace ceilingIO::atmos
{
    struct AtmosRenderConfig
    {
        float targetBedLufs       = -18.0f;
        float targetCompositeLufs = -18.0f;
        float maxTruePeakDbtp     = -1.0f;
        float dialogueHeadroomDb  = 0.0f; // extra headroom for dialogue objects (unused in minimal)
        bool  linkedLimiting      = true;
    };

    // Atmos mastering pipeline. Bed channels get gain + EQ + compression;
    // bed + all objects pass through a single (linked) lookahead limiter so the
    // spatial image is preserved. The original ADM axml metadata is re-embedded
    // on write. Stream-processed block-by-block (Constraint #5).
    class AtmosRenderer
    {
    public:
        void prepare (double sampleRate, int bedChannels, int numObjects, int samplesPerBlock);
        void setConfig (const AtmosRenderConfig& config);

        // Process one block in place. bedBuffer = (bedChannels x n); each
        // objectBuffers[k] = (1 x n). Linked limiting runs across all channels.
        void processBlock (juce::AudioBuffer<float>& bedBuffer,
                            std::vector<juce::AudioBuffer<float>>& objectBuffers);

        // Full offline render (server mode). Streams the input reader block by
        // block, analyzes, computes a uniform composite gain to hit the target,
        // processes, and writes an ADM BWF (axml pass-through) into outputData.
        bool renderToMemory (AdmBwfReader& input,
                              juce::MemoryBlock& outputData,
                              const AtmosRenderConfig& config,
                              juce::String& errorMessage,
                              AtmosLoudnessResult* finalAnalysis = nullptr);

    private:
        ceilingIO::dsp::LookaheadLimiter limiter_;
        ceilingIO::dsp::EqChain           eqChain_;
        juce::dsp::Compressor<float>      compressor_;
        AtmosLoudnessAnalyzer            analyzer_;     // input (gain computation)
        AtmosLoudnessAnalyzer            postAnalyzer_; // output (final analysis)
        std::vector<float> objectGainDb_;
        float bedGainDb_ = 0.0f;
        AtmosRenderConfig config_;
        double sampleRate_ = 48000.0;
        int bedChannels_ = 0;
        int numObjects_ = 0;
        int samplesPerBlock_ = 1024;
        juce::AudioBuffer<float> concatBuffer_; // bed + object channels for linked limiting
    };
} // namespace ceilingIO::atmos