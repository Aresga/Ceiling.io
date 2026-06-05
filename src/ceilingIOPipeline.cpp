#include "ceilingIOPipeline.h"
#include <lame/lame.h>

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
                "hiphop", "Hip-Hop / Bass-Heavy",
                2.0f, -1.0f, 0.5f,   
                4.0f, 20.0f, 150.0f 
            }
        }};
    } // namespace 

    namespace
    {
        constexpr float kLufsOffset = -0.691f;
        constexpr float kAbsoluteGateLufs = -70.0f;

        // Takes raw audio and convert it into a dicibel value in LUFS, EBU R128
        inline float meanSquareToLufs (double meanSquare)
        {
            if (! std::isfinite (meanSquare) || meanSquare <= 1.0e-12)
                return -120.0f;
            return kLufsOffset + 10.0f * std::log10 ((float) meanSquare);
        }
        // Does the opposite of meanSquareToLufs, converting a LUFS value back to a linear mean square value
        inline double lufsToMeanSquare (float lufs)
        {
            return std::pow (10.0, (lufs - kLufsOffset) / 10.0f);
        }
    } // namespace

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

    const PlatformPreset* findPlatformPreset (const juce::String& name) noexcept
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

    const int numChannels        = (int) reader.numChannels;
    const juce::int64 totalSamples = reader.lengthInSamples;
    const double sampleRate      = reader.sampleRate;
    const int blockSize          = 4096;

    // ── Build per-channel K-weighting filter chains (ITU-R BS.1770) ──────────
    // Stage 1: High-pass at 38 Hz, Q=0.7071 (removes DC/sub, Butterworth)
    // Stage 2: High-shelf at 1500 Hz, Q=0.7071, +4 dB (models acoustic head effect)
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (juce::uint32) blockSize;
    spec.numChannels      = 1; // applied per-channel

    using KWeightChain = juce::dsp::ProcessorChain<
        juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Filter<float>>;

    std::vector<KWeightChain> kChains ((size_t) numChannels);
    for (auto& chain : kChains)
    {
        chain.prepare (spec);
        chain.get<0>().coefficients =
            juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 38.0, 0.7071f);
        chain.get<1>().coefficients =
            juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                sampleRate, 1500.0, 0.7071f,
                juce::Decibels::decibelsToGain (4.0f));
    }

    // ── Accumulate flat (RMS/peak) and K-weighted (LUFS) separately ──────────
    juce::AudioBuffer<float> buffer (numChannels, blockSize);

    juce::int64 samplesRead      = 0;
    double flatSumSquares        = 0.0;   // unweighted → RMS
    double kWeightedSumSquares   = 0.0;   // K-weighted → LUFS
    float  peak                  = 0.0f;

    while (samplesRead < totalSamples)
    {
        const int samplesThisIter = (int) std::min<juce::int64> (blockSize, totalSamples - samplesRead);
        reader.read (&buffer, 0, samplesThisIter, samplesRead, true, true);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            // --- Flat pass: RMS + peak (no frequency weighting — correct for RMS) ---
            const float* data = buffer.getReadPointer (channel);
            for (int sample = 0; sample < samplesThisIter; ++sample)
            {
                const float value = data[sample];
                flatSumSquares += (double) value * (double) value;
                peak = std::max (peak, std::abs (value));
            }

            // --- K-weighted pass: copy channel → filter → accumulate ---
            juce::AudioBuffer<float> chBuf (1, samplesThisIter);
            chBuf.copyFrom (0, 0, buffer, channel, 0, samplesThisIter);

            juce::dsp::AudioBlock<float> block (chBuf);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            kChains[(size_t) channel].process (ctx);

            const float* kw = chBuf.getReadPointer (0);
            for (int sample = 0; sample < samplesThisIter; ++sample)
                kWeightedSumSquares += (double) kw[sample] * (double) kw[sample];
        }

        samplesRead += samplesThisIter;
    }

    // ── Compute final metrics ─────────────────────────────────────────────────
    const double totalSamplesCh = (double) (std::max<juce::int64> (1, totalSamples)
                                            * std::max (1, numChannels));

    const double kMeanSquare   = kWeightedSumSquares / totalSamplesCh;
    const double flatMeanSquare = flatSumSquares      / totalSamplesCh;
    const double rms            = std::sqrt (flatMeanSquare);

    // LUFS: K-weighted mean square fed into the BS.1770 formula
    result.lufsDb = meanSquareToLufs (kMeanSquare);

    // RMS: flat (unweighted) energy — correct, RMS is not frequency-weighted
    result.rmsDb  = juce::Decibels::gainToDecibels ((float) rms, -120.0f);
    result.peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);

    // ── Intensity-based threshold/drive suggestions (unchanged) ──────────────
    const int clampedIntensity = juce::jlimit (1, 3, intensity);
    float thresholdOffset = 0.0f;
    float driveOffset     = 0.0f;

    if (clampedIntensity == 1)
    {
        thresholdOffset = 1.0f;
        driveOffset     = 0.0f;
    }
    else if (clampedIntensity == 2)
    {
        thresholdOffset = 3.0f;
        driveOffset     = 0.5f;
    }
    else
    {
        thresholdOffset = 6.0f;
        driveOffset     = 1.5f;
    }

    result.suggestedThresholdDb = juce::jlimit (-60.0f, 0.0f, result.rmsDb - thresholdOffset);
    result.suggestedDriveDb     = juce::jlimit (0.0f, 12.0f, std::max (0.0f, -1.0f - result.peakDb + driveOffset));
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

// float computeNormalizationGainDb (const AnalysisResult& analysis, const PlatformPreset& preset) noexcept
// {
//     return preset.targetLufs - analysis.lufsDb;
// }



    // AnalysisResult analyseFile (const juce::File& audioFile, int intensity)
    // {
    //     juce::AudioFormatManager formatManager;
    //     formatManager.registerBasicFormats();

    //     std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));
    //     if (! reader)
    //         return {};

    //     return analyseReader (*reader, intensity);
    // }

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
        // 1. Protection Factor
        // Quiet tracks (< -24 LUFS): boost processing slightly (up to 1.25x)
        // Normal tracks (<= -18 LUFS): full processing (1.0)
        // Loud tracks (-18 to -12 LUFS): scale down smoothly toward 0
        // Mastered tracks (>= -12 LUFS): bypass all DSP
        float protectionFactor = 1.0f;

        if (analysis.lufsDb > -18.0f)
        {
            protectionFactor = juce::jmap  (analysis.lufsDb, -18.0f, -12.0f, 1.0f, 0.0f);
            protectionFactor = juce::jlimit (0.0f, 1.0f, protectionFactor);
        }
        else if (analysis.lufsDb < -24.0f)
        {
            protectionFactor = juce::jmap  (analysis.lufsDb, -24.0f, -18.0f, 1.25f, 1.0f);
            protectionFactor = juce::jlimit (1.0f, 1.25f, protectionFactor);
        }


        // 2. Limiter ceiling from platform preset
        const float ceiling = (platform != nullptr) ? platform->maxTruePeakDbtp : -1.0f;
        juce::Logger::writeToLog ("[Ceiling pipeline] Setting limiter ceiling to " + juce::String (ceiling) + " dBTP");
        processor.setLimiterCeilingDbtp (ceiling);


        // 3. EQ — scaled by protectionFactor so loud tracks stay flat
        if (genre != nullptr)
        {
            const float finalLow  = genre->baseLowShelfDb  * protectionFactor;
            const float finalMid  = genre->baseMidPeakDb   * protectionFactor;
            const float finalHigh = genre->baseHighShelfDb * protectionFactor;

            if (auto* lo = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_LO_GAIN")))
                lo->setValueNotifyingHost (lo->convertTo0to1 (finalLow));

            if (auto* mid = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_MID_GAIN")))
                mid->setValueNotifyingHost (mid->convertTo0to1 (finalMid));

            if (auto* hi = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_HI_GAIN")))
                hi->setValueNotifyingHost (hi->convertTo0to1 (finalHigh));
        }


        // 4. Compressor ratio — boosted for high crest factor (very dynamic) tracks
        // so the compressor does more work before the limiter, reducing peak-to-LUFS gap
        if (auto* ratio = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RATIO")))
        {
            const float baseRatio   = (genre != nullptr) ? genre->compRatio : 2.0f;

            // crestFactor is negative — more negative = more dynamic = needs more compression
            const float crestFactor = analysis.lufsDb - analysis.peakDb;
            const float crestBoost  = juce::jlimit (0.0f, 2.0f, (-crestFactor - 12.0f) * 0.2f);

            const float finalRatio  = juce::jmap (protectionFactor, 0.0f, 1.0f, 1.0f, baseRatio + crestBoost);
            ratio->setValueNotifyingHost (ratio->convertTo0to1 (finalRatio));
        }


        // 5. Compressor timings — scaled by protectionFactor
        // At protectionFactor=0 (mastered track), use neutral values to avoid envelope artifacts
        if (auto* attack = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_ATTACK")))
        {
            const float finalAttack = genre
                ? juce::jmap (protectionFactor, 0.0f, 1.0f, 10.0f, genre->compAttackMs)
                : 10.0f;
            attack->setValueNotifyingHost (attack->convertTo0to1 (finalAttack));
        }

        if (auto* release = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RELEASE")))
        {
            const float finalRelease = genre
                ? juce::jmap (protectionFactor, 0.0f, 1.0f, 100.0f, genre->compReleaseMs)
                : 100.0f;
            release->setValueNotifyingHost (release->convertTo0to1 (finalRelease));
        }


        // 6. Limiter drive — peak-aware clamp
        // Don't push harder than what peaks allow under the ceiling, max +6 dB
        if (auto* drive = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
        {
            // renderGainDb already accounts for LUFS target — don't re-cap by peak here.
            // The brickwall limiter ceiling handles the actual peak control.
            const float finalDrive = juce::jlimit (0.0f, 18.0f, renderGainDb); // raise cap from 6→18
            drive->setValueNotifyingHost (drive->convertTo0to1 (finalDrive));
        }
    }
    
    bool encodeWavToMp3 (const juce::MemoryBlock& wavData, juce::MemoryBlock& mp3Data)
    {
        // Re-read the WAV from memory
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        auto stream = std::make_unique<juce::MemoryInputStream> (wavData.getData(), wavData.getSize(), false);
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (std::move (stream)));
        if (reader == nullptr) return false;

        const int numSamples  = (int) reader->lengthInSamples;
        const int numChannels = (int) reader->numChannels;
        const int sampleRate  = (int) reader->sampleRate;

        // Read PCM into interleaved short buffers
        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        reader->read (&buffer, 0, numSamples, 0, true, true);

        lame_t lame = lame_init();
        lame_set_in_samplerate  (lame, sampleRate);
        lame_set_num_channels   (lame, numChannels);
        lame_set_VBR            (lame, vbr_default);
        lame_set_VBR_quality    (lame, 2);   // V2 ~190kbps, transparent quality
        lame_init_params        (lame);

        const int mp3BufSize = (int) (1.25 * numSamples + 7200);
        mp3Data.setSize ((size_t) mp3BufSize);

        int encodedBytes = 0;
        if (numChannels == 2)
        {
            encodedBytes = lame_encode_buffer_ieee_float (
                lame,
                buffer.getReadPointer (0), buffer.getReadPointer (1),
                numSamples,
                (unsigned char*) mp3Data.getData(), mp3BufSize);
        }
        else
        {
            encodedBytes = lame_encode_buffer_ieee_float (
                lame,
                buffer.getReadPointer (0), buffer.getReadPointer (0),
                numSamples,
                (unsigned char*) mp3Data.getData(), mp3BufSize);
        }

        // Flush remaining frames
        encodedBytes += lame_encode_flush (lame, (unsigned char*) mp3Data.getData() + encodedBytes,
                                        mp3BufSize - encodedBytes);
        lame_close (lame);

        mp3Data.setSize ((size_t) encodedBytes);
        return encodedBytes > 0;
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
} // namespace ceilingIO