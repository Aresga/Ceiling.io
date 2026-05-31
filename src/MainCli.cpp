#include <JuceHeader.h>
#include "MainAudioProcessor.h"

#include <iostream>
#include <algorithm>

namespace
{
    struct PlatformPreset
    {
        juce::String name;
        juce::String label;
        float targetLufs = -14.0f;
        float maxTruePeakDbtp = -1.0f;
    };

    struct CliOptions
    {
        juce::File inputFile;
        juce::File outputFile;
        bool wantsHelp = false;
        bool wantsVersion = false;
        bool wantsRender = false;
        bool wantsListPresets = false;
        juce::String presetName;
        int intensity = 2;
    };

    struct AnalysisResult
    {
        float lufsDb = -120.0f;
        float rmsDb = -120.0f; // 
        float peakDb = -120.0f;
        float suggestedThresholdDb = -18.0f;
        float suggestedDriveDb = 0.0f;
    };

    const std::array<PlatformPreset, 5> platformPresets
    {{
        { "spotify", "Spotify", -14.0f, -1.0f },
        { "apple-music", "Apple Music", -16.0f, -1.0f },
        { "applemusic", "Apple Music", -16.0f, -1.0f },
        { "youtube", "YouTube", -14.0f, -1.0f },
        { "tidal", "Tidal", -14.0f, -1.0f }
    }};

    const PlatformPreset* findPreset (const juce::String& name)
    {
        const auto normalized = name.trim().toLowerCase();

        for (const auto& preset : platformPresets)
            if (preset.name == normalized)
                return &preset;

        return nullptr;
    }

    void printPresetList()
    {
        std::cout << "Available presets:\n";

        for (const auto& preset : platformPresets)
        {
            if (preset.name == "applemusic")
                continue;

            std::cout << "  " << preset.name << "  (" << preset.label << ", target "
                      << juce::String (preset.targetLufs, 1) << " LUFS, max true peak "
                      << juce::String (preset.maxTruePeakDbtp, 1) << " dBTP)\n";
        }

        std::cout << "\nUse --preset <name> with --output to normalize the render to that platform target.\n";
    }

    void printUsage()
    {
        std::cout << "ceilingIO CLI\n"
                  << "Usage: ceilingIOCli --input <file> [--output <file>] [--preset <name>] [--intensity 1|2|3]\n"
                  << "Options:\n"
                  << "  --help, -h       Show this help text\n"
                  << "  --version        Print version information\n"
                  << "  --input, -i      Input audio file to analyze\n"
                  << "  --output, -o     Output WAV file to render\n"
                  << "  --preset         Streaming target preset: spotify, apple-music, youtube, tidal\n"
                  << "  --list-presets   Show preset targets\n"
                  << "  --intensity, -t  Analysis intensity: 1=low, 2=medium, 3=high\n";
    }

    bool parseOptions (juce::StringArray args, CliOptions& options, juce::String& errorMessage)
    {
        for (int i = 0; i < args.size(); ++i)
        {
            const auto arg = args[i];

            if (arg == "--help" || arg == "-h")
            {
                options.wantsHelp = true;
            }
            else if (arg == "--version")
            {
                options.wantsVersion = true;
            }
            else if (arg == "--render")
            {
                options.wantsRender = true;
            }
            else if (arg == "--list-presets")
            {
                options.wantsListPresets = true;
            }
            else if (arg == "--input" || arg == "-i")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --input";
                    return false;
                }

                juce::File inputFile = juce::File::getCurrentWorkingDirectory().getChildFile (args[i]);

                options.inputFile = inputFile;
            }
            else if (arg == "--output" || arg == "-o")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --output";
                    return false;
                }

                options.outputFile = juce::File (args[i]);
                options.wantsRender = true;
            }
            else if (arg == "--preset")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --preset";
                    return false;
                }

                options.presetName = args[i].trim();
            }
            else if (arg == "--intensity" || arg == "-t")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --intensity";
                    return false;
                }

                options.intensity = juce::jlimit (1, 3, args[i].getIntValue());
            }
            else
            {
                errorMessage = "Unknown argument: " + arg;
                return false;
            }
        }

        return true;
    }

    // float computeNormalizationGainDb (const AnalysisResult& analysis, const PlatformPreset& preset)
    // {
    //     const float targetLoudnessGainDb = preset.targetLufs - analysis.lufsDb;
    //     const float maxAllowedGainDb = preset.maxTruePeakDbtp - analysis.peakDb;
    //     return std::min (targetLoudnessGainDb, maxAllowedGainDb);
    // }

    float computeNormalizationGainDb (const AnalysisResult& analysis, const PlatformPreset& preset)
    {
        // Let the loudness gap drive the engine entirely.
        // The look-ahead limiter will handle clamping the peaks safely at the ceiling.
        return preset.targetLufs - analysis.lufsDb;
    }

    AnalysisResult analyseFile (const juce::File& audioFile, int intensity)
    {
        AnalysisResult result;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));
        if (! reader)
            return result;

        const int numChannels = (int) reader->numChannels;
        const juce::int64 totalSamples = reader->lengthInSamples;
        const int blockSize = 4096;

        juce::AudioBuffer<float> buffer (numChannels, blockSize);

        juce::int64 samplesRead = 0;
        double sumSquares = 0.0;
        float peak = 0.0f;

        while (samplesRead < totalSamples)
        {
            const int samplesThisIter = (int) std::min<juce::int64> (blockSize, totalSamples - samplesRead);
            reader->read (&buffer, 0, samplesThisIter, samplesRead, true, true);

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

    // void applyAnalysisToProcessor (MainAudioProcessor& processor, const AnalysisResult& analysis, bool presetActive, float renderGainDb)
    // {
    //     if (auto* threshold = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_THRESHOLD")))
    //     {
    //         const float thresholdDb = presetActive ? -18.0f : analysis.suggestedThresholdDb;
    //         threshold->setValueNotifyingHost (threshold->convertTo0to1 (thresholdDb));
    //     }

    //     if (auto* drive = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
    //     {
    //         // Pre-drive the limiter during preset renders so the output level is
    //         // set by the processor, not by a post-chain gain stage.
    //         const float driveDb = presetActive ? renderGainDb : analysis.suggestedDriveDb;
    //         drive->setValueNotifyingHost (drive->convertTo0to1 (driveDb));
    //     }
    // }
    void applyAnalysisToProcessor (MainAudioProcessor& processor, const AnalysisResult& analysis, const PlatformPreset* preset, float renderGainDb)
{
    const bool presetActive = (preset != nullptr);

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

    // if (auto* ceiling = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("CEILING")))
    // {
    //     // FIX: Now 'preset' is safely in scope and can be accessed if it's not null!
    //     const float ceilingDb = presetActive ? preset->maxTruePeakDbtp : -0.3f; 
    //     ceiling->setValueNotifyingHost (ceiling->convertTo0to1 (ceilingDb));
    // }
}

}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    CliOptions options;
    juce::String errorMessage;

    if (! parseOptions (args, options, errorMessage))
    {
        std::cerr << errorMessage << "\n";
        printUsage();
        return 1;
    }

    if (options.wantsListPresets)
    {
        printPresetList();
        return 0;
    }

    if (options.wantsHelp || args.isEmpty())
    {
        printUsage();
        return 0;
    }

    if (options.wantsVersion)
    {
        std::cout << "ceilingIO CLI 0.1\n";
        return 0;
    }

    if (! options.inputFile.existsAsFile())
    {
        std::cerr << "Input file does not exist: " << options.inputFile.getFullPathName() << "\n";
        return 1;
    }

    if (options.wantsRender && options.outputFile == juce::File())
    {
        std::cerr << "Render requested but no output file was provided.\n";
        return 1;
    }

    MainAudioProcessor processor;

    const auto analysis = analyseFile (options.inputFile, options.intensity);
    const PlatformPreset* preset = nullptr;
    if (options.presetName.isNotEmpty())
    {
        preset = findPreset (options.presetName);
        if (preset == nullptr)
        {
            std::cerr << "Unknown preset: " << options.presetName << "\n";
            return 1;
        }
    }

    float renderGainDb = 0.0f;
    if (preset != nullptr)
        renderGainDb = computeNormalizationGainDb (analysis, *preset);

    std::cout << "Input: " << options.inputFile.getFullPathName() << "\n"
              << "Integrated loudness: " << juce::String (analysis.lufsDb, 2) << " LUFS\n"
              << "RMS: " << juce::String (analysis.rmsDb, 2) << " dB\n"
              << "Peak: " << juce::String (analysis.peakDb, 2) << " dB\n"
              << "Suggested threshold: " << juce::String (analysis.suggestedThresholdDb, 2) << " dB\n"
              << "Suggested drive: " << juce::String (analysis.suggestedDriveDb, 2) << " dB\n";

    if (preset != nullptr)
    {
        std::cout << "Preset: " << preset->label << " (target "
                  << juce::String (preset->targetLufs, 1) << " LUFS, max true peak "
                  << juce::String (preset->maxTruePeakDbtp, 1) << " dBTP)\n"
                  << "Normalization gain: " << juce::String (renderGainDb, 2) << " dB\n";
    }

    if (options.outputFile != juce::File())
    {
        // applyAnalysisToProcessor (processor, analysis, preset != nullptr, renderGainDb);
        applyAnalysisToProcessor (processor, analysis, preset, renderGainDb);

        if (! renderFile (options.inputFile, options.outputFile, processor, errorMessage))
        {
            std::cerr << errorMessage << "\n";
            return 1;
        }

        std::cout << "Rendered: " << options.outputFile.getFullPathName() << "\n";
    }

    return 0;
}
