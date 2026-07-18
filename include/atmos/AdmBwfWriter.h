#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelWeights.h"

#include <memory>

namespace ceilingIO::atmos
{
    // Minimal ADM BWF writer. Hand-writes a RIFF WAV with a WAVE_FORMAT_EXTENSIBLE
    // fmt chunk (correct channel mask for the bed layout), a PCM data chunk, and
    // the original axml chunk verbatim (metadata pass-through). Full ADM metadata
    // re-embedding (object position updates) is intentionally out of scope.
    class AdmBwfWriter
    {
    public:
        // Non-owning: caller must keep stream alive until close() returns.
        bool open (juce::OutputStream& stream, double sampleRate, int numChannels,
                   int bitsPerSample, dsp::ChannelLayout bedLayout, const juce::String& axml);

        // Interleave bed channels followed by object channels (each object is 1
        // channel) and write as PCM. numSamples samples from each channel.
        bool writeBlock (const juce::AudioBuffer<float>& bedBuffer,
                         const std::vector<juce::AudioBuffer<float>>& objectBuffers,
                         int numSamples);

        // Patches the RIFF and data chunk sizes and flushes.
        bool close();

    private:
        void writeU16LE (juce::uint16 v);
        void writeU32LE (juce::uint32 v);
        void writeS24LE (float sample);

        juce::OutputStream* out_ = nullptr;
        int    numChannels_   = 0;
        int    bitsPerSample_ = 24;
        int    bytesPerSample_ = 3;
        juce::uint64 dataBytesWritten_ = 0;
        juce::uint64 dataSizeFieldOffset_ = 0;
        juce::uint64 riffSizeFieldOffset_ = 4;
        juce::String axml_;
        bool ok_ = true; // tracks write failures (JUCE 8 OutputStream::write returns bool)
    };
} // namespace ceilingIO::atmos