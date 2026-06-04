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

        // Preset Control Matrix Blueprint:
        // { 
        //     "id", "Display Name", 
        //      LowGain(dB), MidGain(dB), HighGain(dB), 
        //      Ratio(:1), Attack(ms), Release(ms) 
        // }
       // 🌟 Fixed size from 3 to 5 to avoid compilation crash
        const std::array<GenrePreset, 5> genrePresets {{
            {
                "electronic", "Electronic / Club",
                1.5f, -0.5f, 0.8f, 
                3.5f, 15.0f, 80.0f 
            },
            { 
                "acoustic", "Acoustic / Transparent", 
                0.0f, 0.0f, 1.2f,    
                1.5f, 40.0f, 250.0f  
            },
            { 
                "pop", "Modern Pop", 
                0.8f, 1.0f, 0.5f,    
                2.2f, 25.0f, 140.0f  
            },
            {
                "rock", "Rock / Aggressive", 
                1.2f, 0.0f, 0.7f,    
                3.0f, 10.0f, 100.0f 
            },
            {
                "hip-hop", "Hip-Hop / Bass-Heavy",
                2.0f, -1.0f, 0.5f,   
                4.0f, 20.0f, 150.0f 
            }
        }};
    }

    const GenrePreset* findGenrePreset (const juce::String& name) noexcept {
        const auto normalized = name.trim().toLowerCase();
        for (const auto& preset : genrePresets)
            if (preset.id == normalized)
                return &preset;
        return nullptr;
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
                                   const PlatformPreset* platform,
                                   const GenrePreset* genre,
                                   float renderGainDb)
    {
        // 1. Calculate the Protection Factor (1.0 = Full Processing, 0.0 = Pure Bypass)
        float protectionFactor = 1.0f;

        // If the input file is already louder than -18 LUFS, it is heavily compressed.
        if (analysis.lufsDb > -18.0f)
        {
            // Smoothly scale down processing. At -12 LUFS, processing drops to absolute 0.
            protectionFactor = juce::jmap (analysis.lufsDb, -18.0f, -12.0f, 1.0f, 0.0f);
            protectionFactor = juce::jlimit (0.0f, 1.0f, protectionFactor);
        }

        // 2. Lock down True Peak safety boundaries from the platform rule
        const float ceiling = (platform != nullptr) ? platform->maxTruePeakDbtp : -1.0f;
        processor.setLimiterCeilingDbtp (ceiling);

        // 3. Apply Scaled Equalizer Curves
        if (genre != nullptr)
        {
            // The EQ choices are multiplied by our protection factor so loud tracks stay flat
            float finalLow = genre->baseLowShelfDb * protectionFactor;
            float finalMid = genre->baseMidPeakDb * protectionFactor;
            float finalHigh = genre->baseHighShelfDb * protectionFactor;

            if (auto* lo = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_LO_GAIN")))
                lo->setValueNotifyingHost (lo->convertTo0to1 (finalLow));

            if (auto* mid = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_MID_GAIN")))
                mid->setValueNotifyingHost (mid->convertTo0to1 (finalMid));

            if (auto* hi = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_HI_GAIN")))
                hi->setValueNotifyingHost (hi->convertTo0to1 (finalHigh));
        }

        // 4. Relax Compressor Glue Settings for Loud Tracks
        if (auto* ratio = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RATIO")))
        {
            // If protected, drop the compression ratio back down toward a transparent 1.0 (no effect)
            float baseRatio = (genre != nullptr) ? genre->compRatio : 2.0f;
            float finalRatio = juce::jmap (protectionFactor, 0.0f, 1.0f, 1.0f, baseRatio);
            ratio->setValueNotifyingHost (ratio->convertTo0to1 (finalRatio));
        }

        // Keep your existing timings setup
        if (auto* attack = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_ATTACK")))
            attack->setValueNotifyingHost (attack->convertTo0to1 (genre ? genre->compAttackMs : 10.0f));

        if (auto* release = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RELEASE")))
            release->setValueNotifyingHost (release->convertTo0to1 (genre ? genre->compReleaseMs : 100.0f));

        // 5. Intelligent Limiter Volume Adjustments
        if (auto* drive = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
        {
            float targetLufs = (platform != nullptr) ? platform->targetLufs : -14.0f;
            float platformGainDb = targetLufs - analysis.lufsDb;
            
            // If the track is already louder than the platform target, gain becomes 0.0dB.
            // It will not push the track any harder, acting only as a safety ceiling shield.

            float drivenGain = juce::jlimit(-12.0f, 12.0f, renderGainDb);
            drive->setValueNotifyingHost(drive->convertTo0to1(drivenGain));

            // float drivenGain = juce::jlimit (0.0f, 12.0f, platformGainDb);
            // drive->setValueNotifyingHost (drive->convertTo0to1 (drivenGain));
        }
    }

    bool renderReaderToMemory (juce::AudioFormatReader& reader,
                               MainAudioProcessor& processor,
                               juce::MemoryBlock& outputData,
                               juce::String& errorMessage,
                               AnalysisResult* finalAnalysis
                            )
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

        writer.reset();

        processor.releaseResources();

        if (finalAnalysis != nullptr) {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::MemoryInputStream> outStream (new juce::MemoryInputStream (outputData, false)); // false = zero copy
            std::unique_ptr<juce::AudioFormatReader> outputReader (wav.createReaderFor (outStream.release(), true));

            if (outputReader != nullptr)
            {
                *finalAnalysis = analyseReader (*outputReader, 2);
            }
        }


        // std::unique_ptr<juce::MemoryInputStream> outStream (new juce::MemoryInputStream (outputData, false));
        // std::unique_ptr<juce::AudioFormatReader> outputReader (wav.createReaderFor (outStream.release(), true));

        // if (outputReader != nullptr) {
        //     AnalysisResult finalStats = analyseReader (*outputReader, 2);
        // }
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