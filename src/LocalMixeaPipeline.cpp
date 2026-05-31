#include "ceilingIOPipeline.h"

#include <algorithm>
#include <cmath>

namespace ceilingIO
{
    namespace
    {
        const std::array<PlatformPreset, 5> presets
        {{
            { "spotify", "Spotify", -14.0f, -1.0f },
            { "apple-music", "Apple Music", -16.0f, -1.0f },
            { "applemusic", "Apple Music", -16.0f, -1.0f },
            { "youtube", "YouTube", -14.0f, -1.0f },
            { "tidal", "Tidal", -14.0f, -1.0f }
        }};
    }

    const std::array<PlatformPreset, 5>& getPlatformPresets() noexcept
    {
        return presets;
    }

    const PlatformPreset* findPreset (const juce::String& name) noexcept
    {
        const auto normalized = name.trim().toLowerCase();

        for (const auto& preset : presets)
            if (preset.name == normalized)
                return &preset;

        return nullptr;
    }

    AnalysisResult analyseReader (juce::AudioFormatReader& reader, int intensity)
    {
        AnalysisResult result;

        const int numChannels = (int) reader.numChannels;
        const juce::int64 totalSamples = reader.lengthInSamples;
        const int blockSize = 4096;

        juce::AudioBuffer<float> buffer (numChannels, blockSize);

        juce::int64 samplesRead = 0;
        double sumSquares = 0.0;
        float peak = 0.0f;

        while (samplesRead < totalSamples)
        {
            const int samplesThisIter = (int) std::min<juce::int64> (blockSize, totalSamples - samplesRead);
            reader.read (&buffer, 0, samplesThisIter, samplesRead, true, true);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const float* data = buffer.getReadPointer (channel);

                for (int sample = 0; sample < samplesThisIter; ++sample)
                {
                    const float value = data[sample];
                    sumSquares += (double) value * (double) value;
                    peak = std::max (peak, std::abs (value));
                }
            }

            samplesRead += samplesThisIter;
        }

        const double meanSquare = sumSquares / (double) (std::max<juce::int64> (1, totalSamples) * std::max (1, numChannels));
        const double rms = std::sqrt (meanSquare);

        result.lufsDb = juce::jlimit (-120.0f, 12.0f, (float) (-0.691f + 10.0 * std::log10 (std::max (meanSquare, 1.0e-12))));
        result.rmsDb = juce::Decibels::gainToDecibels ((float) rms, -120.0f);
        result.peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);

        const int clampedIntensity = juce::jlimit (1, 3, intensity);
        float thresholdOffset = 0.0f;
        float driveOffset = 0.0f;

        if (clampedIntensity == 1)
        {
            thresholdOffset = 1.0f;
            driveOffset = 0.0f;
        }
        else if (clampedIntensity == 2)
        {
            thresholdOffset = 3.0f;
            driveOffset = 0.5f;
        }
        else
        {
            thresholdOffset = 6.0f;
            driveOffset = 1.5f;
        }

        result.suggestedThresholdDb = juce::jlimit (-60.0f, 0.0f, result.rmsDb - thresholdOffset);
        result.suggestedDriveDb = juce::jlimit (0.0f, 12.0f, std::max (0.0f, -1.0f - result.peakDb + driveOffset));
        return result;
    }

    AnalysisResult analyseFile (const juce::File& audioFile, int intensity)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));
        if (! reader)
            return {};

        return analyseReader (*reader, intensity);
    }

    float computeNormalizationGainDb (const AnalysisResult& analysis, const PlatformPreset& preset) noexcept
    {
        return preset.targetLufs - analysis.lufsDb;
    }

    void applyAnalysisToProcessor (MainAudioProcessor& processor,
                                   const AnalysisResult& analysis,
                                   const PlatformPreset* preset,
                                   float renderGainDb)
    {
        const bool presetActive = (preset != nullptr);

        processor.setLimiterCeilingDbtp (presetActive ? preset->maxTruePeakDbtp : -1.0f);

        if (auto* threshold = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_THRESHOLD")))
        {
            const float thresholdDb = presetActive ? -18.0f : analysis.suggestedThresholdDb;
            threshold->setValueNotifyingHost (threshold->convertTo0to1 (thresholdDb));
        }

        if (auto* drive = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
        {
            const float driveDb = presetActive ? renderGainDb : analysis.suggestedDriveDb;
            drive->setValueNotifyingHost (drive->convertTo0to1 (driveDb));
        }
    }

    bool renderReaderToMemory (juce::AudioFormatReader& reader,
                               MainAudioProcessor& processor,
                               juce::MemoryBlock& outputData,
                               juce::String& errorMessage)
    {
        outputData.reset();

        const int blockSize = 1024;
        juce::AudioBuffer<float> buffer ((int) reader.numChannels, blockSize);

        processor.prepareToPlay (reader.sampleRate, blockSize);

        juce::WavAudioFormat wav;
        const auto writerOptions = juce::AudioFormatWriterOptions{}
            .withSampleRate (reader.sampleRate)
            .withNumChannels ((int) reader.numChannels)
            .withBitsPerSample (24);

        std::unique_ptr<juce::OutputStream> outStream (new juce::MemoryOutputStream (outputData, false));
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (outStream, writerOptions));
        if (writer == nullptr)
        {
            errorMessage = "Unable to create in-memory WAV writer.";
            return false;
        }

        outStream.release();

        juce::int64 samplesRead = 0;
        while (samplesRead < reader.lengthInSamples)
        {
            const int samplesThisIter = (int) std::min<juce::int64> (blockSize, reader.lengthInSamples - samplesRead);
            reader.read (&buffer, 0, samplesThisIter, samplesRead, true, true);

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            writer->writeFromAudioSampleBuffer (buffer, 0, samplesThisIter);
            samplesRead += samplesThisIter;
        }

        processor.releaseResources();
        return true;
    }

    bool renderFile (const juce::File& inputFile,
                     const juce::File& outputFile,
                     MainAudioProcessor& processor,
                     juce::String& errorMessage)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (inputFile));
        if (! reader)
        {
            errorMessage = "Unable to open input file: " + inputFile.getFullPathName();
            return false;
        }

        if (outputFile.existsAsFile() && ! outputFile.deleteFile())
        {
            errorMessage = "Unable to replace output file: " + outputFile.getFullPathName();
            return false;
        }

        if (auto parent = outputFile.getParentDirectory(); parent.exists() && ! parent.createDirectory())
        {
            errorMessage = "Unable to create output directory: " + parent.getFullPathName();
            return false;
        }

        std::unique_ptr<juce::OutputStream> outStream (outputFile.createOutputStream());
        if (outStream == nullptr)
        {
            errorMessage = "Unable to create output stream: " + outputFile.getFullPathName();
            return false;
        }

        juce::WavAudioFormat wav;
        const auto writerOptions = juce::AudioFormatWriterOptions{}
            .withSampleRate (reader->sampleRate)
            .withNumChannels ((int) reader->numChannels)
            .withBitsPerSample (24);

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (outStream, writerOptions));
        if (writer == nullptr)
        {
            errorMessage = "Unable to create WAV writer for: " + outputFile.getFullPathName();
            return false;
        }

        outStream.release();

        const int blockSize = 1024;
        juce::AudioBuffer<float> buffer ((int) reader->numChannels, blockSize);

        processor.prepareToPlay (reader->sampleRate, blockSize);

        juce::int64 samplesRead = 0;
        while (samplesRead < reader->lengthInSamples)
        {
            const int samplesThisIter = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - samplesRead);
            reader->read (&buffer, 0, samplesThisIter, samplesRead, true, true);

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            writer->writeFromAudioSampleBuffer (buffer, 0, samplesThisIter);
            samplesRead += samplesThisIter;
        }

        processor.releaseResources();
        return true;
    }
}