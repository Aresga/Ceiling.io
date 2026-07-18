#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelWeights.h"

#include <memory>
#include <vector>

namespace ceilingIO::atmos
{
    // Minimal ADM BWF (ITU-R BS.2076) reader.
    //
    // Block-oriented: holds the AudioFormatReader and streams bed + object
    // audio in blocks rather than loading the whole multi-channel file into
    // memory (Constraint #5). Extracts the axml chunk for metadata pass-through
    // and parses just enough ADM XML to identify the bed channel layout and the
    // number/track-index of objects. This is NOT a full BS.2076 renderer.
    struct ObjectDefinition
    {
        int          trackIndex = 0;       // channel offset within the WAV
        float        x = 0.0f, y = 0.0f, z = 0.0f; // normalized position (-1..1); 0 in minimal parser
        float        size = 0.0f;          // 0 = point source; 0 in minimal parser
        juce::String programmeId;           // programme this object belongs to (empty in minimal parser)
    };

    class AdmBwfReader
    {
    public:
        // Open from a file or from a memory blob (non-owning view of data, which
        // must outlive the reader). Returns false if the input is not a valid
        // ADM BWF (no axml chunk / unrecognised layout) so callers can fall back
        // to the stereo pipeline.
        bool open (const juce::File& file);
        bool openFromMemory (const void* data, size_t size);

        int                         getNumBedChannels() const noexcept { return bedChannels_; }
        int                         getNumObjects() const noexcept    { return (int) objects_.size(); }
        dsp::ChannelLayout          getBedLayout() const noexcept    { return bedLayout_; }
        const std::vector<ObjectDefinition>& getObjects() const noexcept { return objects_; }
        const juce::String&         getAxmlChunk() const noexcept   { return axmlChunk_; }
        double                      getSampleRate() const noexcept  { return sampleRate_; }
        juce::int64                 getLengthInSamples() const noexcept { return lengthInSamples_; }

        // Stream a block of bed + object audio. Caller pre-sizes bedBuffer to
        // (numBedChannels x numSamples) and each objectBuffers[k] to (1 x numSamples).
        // Returns false on a read error.
        bool readBlock (juce::AudioBuffer<float>& bedBuffer,
                        std::vector<juce::AudioBuffer<float>>& objectBuffers,
                        juce::int64 startSample, int numSamples);

    private:
        bool parseAxml (const juce::String& axml);

        std::unique_ptr<juce::AudioFormatReader> reader_;
        juce::String   axmlChunk_;
        std::vector<ObjectDefinition> objects_;
        dsp::ChannelLayout bedLayout_ = dsp::ChannelLayout::Stereo;
        int     bedChannels_   = 0;
        double  sampleRate_     = 48000.0;
        juce::int64 lengthInSamples_ = 0;
        juce::AudioBuffer<float> scratch_; // interleaved scratch for block reads (preallocated)
    };
} // namespace ceilingIO::atmos