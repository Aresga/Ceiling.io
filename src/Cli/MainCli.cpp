#include <JuceHeader.h>
#include "MainAudioProcessor.h"
#include "ceilingIOPipeline.h"

#include <iostream>
#include <algorithm>

namespace
{
    struct CliOptions
    {
        juce::File inputFile;
        juce::File outputFile;
        bool wantsHelp = false;
        bool wantsVersion = false;
        bool wantsListPresets = false;
        juce::String presetName;
        juce::String genreName;
        double customTargetLoudness = -14.0; // Server fallback default
        int intensity = 2;
    };

    void printPresetList()
    {
        std::cout << "Available Platform Presets:\n";
        for (const auto& preset : ceilingIO::getPlatformPresets())
        {
            if (preset.name == "applemusic") // Skip duplicates
                continue;

            std::cout << "  " << preset.name.paddedRight(' ', 15) 
                      << " (" << preset.label << ", Target: "
                      << juce::String (preset.targetLufs, 1) << " LUFS, Max Peak: "
                      << juce::String (preset.maxTruePeakDbtp, 1) << " dBTP)\n";
        }

        std::cout << "\nAvailable Genre Presets:\n"
                  << "  electronic       (Electronic / Club)\n"
                  << "  acoustic         (Acoustic / Transparent)\n"
                  << "  pop              (Modern Pop)\n"
                  << "  rock             (Rock / Aggressive)\n"
                  << "  hiphop           (Hip-Hop / Bass-Heavy)\n"
                  << "\nUse --preset <name> and/or --genre <name> with --output to process files.\n";
    }

    void printUsage()
    {
        std::cout << "ceilingIO CLI\n"
                  << "Usage: ceilingIOCli --input <file> [--output <file>] [--preset <name>] [--genre <name>] [--target <lufs>]\n\n"
                  << "Options:\n"
                  << "  --help, -h          Show this help text\n"
                  << "  --version           Print version information\n"
                  << "  --input, -i         Input audio file to analyze\n"
                  << "  --output, -o        Output file to render (.wav or .mp3)\n"
                  << "  --preset            Streaming platform target preset (spotify, apple-music, youtube, tidal)\n"
                  << "  --genre             Sonic profile blueprint preset (electronic, acoustic, pop, rock, hiphop)\n"
                  << "  --target            Custom target loudness in LUFS (used if no --preset is provided)\n"
                  << "  --list-presets      Show all available platform and genre targets\n"
                  << "  --intensity, -t     Analysis intensity profile: 1=low, 2=medium, 3=high\n";
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
                options.inputFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[i]);
            }
            else if (arg == "--output" || arg == "-o")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --output";
                    return false;
                }
                options.outputFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[i]);
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
            else if (arg == "--genre")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --genre";
                    return false;
                }
                options.genreName = args[i].trim();
            }
            else if (arg == "--target")
            {
                if (++i >= args.size())
                {
                    errorMessage = "Missing value for --target";
                    return false;
                }
                options.customTargetLoudness = args[i].getDoubleValue();
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
        std::cerr << "Error: " << errorMessage << "\n";
        printUsage();
        return 1;
    }

    if (options.wantsHelp || (args.isEmpty() && !options.wantsListPresets && !options.wantsVersion))
    {
        printUsage();
        return 0;
    }

    if (options.wantsVersion)
    {
        std::cout << "ceilingIO CLI 0.1 (Local Standalone Pipeline)\n";
        return 0;
    }

    if (options.wantsListPresets)
    {
        printPresetList();
        return 0;
    }

    if (! options.inputFile.existsAsFile())
    {
        std::cerr << "Error: Input file does not exist: " << options.inputFile.getFullPathName() << "\n";
        return 1;
    }

    // ── STAGE 1: Audio File Loading & Reading ──────────────────────────────────
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (options.inputFile));
    if (! reader)
    {
        std::cerr << "Error: Unable to open input format channels for file: " << options.inputFile.getFullPathName() << "\n";
        return 1;
    }

    // ── STAGE 2: Analysis Pass ──────────────────────────────────────────────────
    std::cout << "[Info]: Analyzing transient vectors...\n";
    ceilingIO::AnalysisResult analysis = ceilingIO::analyseReader (*reader, options.intensity);

    const ceilingIO::PlatformPreset* platformPtr = nullptr;
    const ceilingIO::GenrePreset* genrePtr = nullptr;
    ceilingIO::PlatformPreset fallbackPreset;

    // Resolve Platform Matrix
    if (options.presetName.isNotEmpty())
    {
        platformPtr = ceilingIO::findPlatformPreset (options.presetName);
        if (platformPtr == nullptr)
        {
            std::cerr << "Error: Unknown target platform preset: " << options.presetName << "\n";
            return 1;
        }
    }

    // Server Fallback logic if explicit platform target parameter is absent
    if (platformPtr == nullptr)
    {
        fallbackPreset.name = "custom";
        fallbackPreset.label = "Custom Target";
        fallbackPreset.targetLufs = static_cast<float> (options.customTargetLoudness);
        fallbackPreset.maxTruePeakDbtp = -1.0f;
        platformPtr = &fallbackPreset;
    }

    // Resolve Genre Matrix
    if (options.genreName.isNotEmpty())
    {
        genrePtr = ceilingIO::findGenrePreset (options.genreName);
        if (genrePtr == nullptr)
        {
            std::cerr << "Error: Unknown genre profile preset: " << options.genreName << "\n";
            return 1;
        }
    }

    // ── STAGE 3: Server-Matched Normalization Engine Math ─────────────────────────
    const float effectiveLowBump   = (genrePtr != nullptr) ? genrePtr->baseLowShelfDb : 0.0f;
    const float dynamicHeadroomPad = (effectiveLowBump > 0.0f) ? -(effectiveLowBump * 0.5f) : 0.0f;

    float renderGainDb = (platformPtr->targetLufs - analysis.lufsDb) + dynamicHeadroomPad;
    
    if (renderGainDb < 0.0f)
        renderGainDb = 0.0f;

    // Report diagnostics mirroring the server state logs
    std::cout << "─────────────────────────────────────────────────────────\n"
              << "Input File:          " << options.inputFile.getFileName() << "\n"
              << "Integrated Loudness: " << juce::String (analysis.lufsDb, 2) << " LUFS\n"
              << "RMS Floor Level:     " << juce::String (analysis.rmsDb, 2) << " dB\n"
              << "Peak True Level:     " << juce::String (analysis.peakDb, 2) << " dB\n"
              << "Suggested Threshold: " << juce::String (analysis.suggestedThresholdDb, 2) << " dB\n"
              << "Suggested Pre-Drive: " << juce::String (analysis.suggestedDriveDb, 2) << " dB\n"
              << "─────────────────────────────────────────────────────────\n"
              << "Target Platform:     " << platformPtr->label << " (" << juce::String(platformPtr->targetLufs, 1) << " LUFS)\n"
              << "Target Genre Match:  " << (genrePtr ? genrePtr->displayName : "None / Flat Profile") << "\n"
              << "Calculated Processing Gain: " << juce::String (renderGainDb, 2) << " dB (Includes " << juce::String(dynamicHeadroomPad, 2) << " dB low bump pad)\n"
              << "─────────────────────────────────────────────────────────\n";

    // ── STAGE 4: Core Execution Render Pass ─────────────────────────────────────
    if (options.outputFile != juce::File())
    {
        std::cout << "[Info]: Executing DSP mastering matrices via shared memory pipes...\n";

        MainAudioProcessor processor;
        ceilingIO::applyAnalysisToProcessor (processor, analysis, platformPtr, genrePtr, renderGainDb);

        juce::MemoryBlock intermediateWavData;
        juce::String dspError;
        ceilingIO::AnalysisResult finalAnalysis;

        // Render directly out to memory block structures identical to the server layout
        if (! ceilingIO::renderReaderToMemory (*reader, processor, intermediateWavData, dspError, &finalAnalysis))
        {
            std::cerr << "Error: DSP math pipeline failure: " << (dspError.isNotEmpty() ? dspError : "Unknown Error") << "\n";
            return 1;
        }

        // Automatic Output Format Resolution Check
        const bool shouldTranscodeToMp3 = options.outputFile.getFileExtension().toLowerCase() == ".mp3";
        juce::MemoryBlock finalExportData;

        if (shouldTranscodeToMp3)
        {
            std::cout << "[Info]: Compiling frames through LAME MP3 encoder engine...\n";
            if (! ceilingIO::encodeWavToMp3 (intermediateWavData, finalExportData))
            {
                std::cerr << "Error: LAME framing structure compression layer failed.\n";
                return 1;
            }
        }
        else
        {
            finalExportData = std::move (intermediateWavData);
        }

        // Write internal output buffer structures back to filesystem
        if (options.outputFile.existsAsFile() && ! options.outputFile.deleteFile())
        {
            std::cerr << "Error: Target filesystem lock. Cannot replace output: " << options.outputFile.getFullPathName() << "\n";
            return 1;
        }

        if (auto parentDir = options.outputFile.getParentDirectory(); ! parentDir.exists())
            parentDir.createDirectory();

        std::unique_ptr<juce::FileOutputStream> fos (options.outputFile.createOutputStream());
        if (fos == nullptr)
        {
            std::cerr << "Error: Unable to open write permission stream to path: " << options.outputFile.getFullPathName() << "\n";
            return 1;
        }

        fos->write (finalExportData.getData(), finalExportData.getSize());
        fos.reset(); // Flush file buffers safely to disk

        std::cout << "─────────────────────────────────────────────────────────\n"
                  << "[Success]: Processed file generated successfully!\n"
                  << "Output Location:      " << options.outputFile.getFullPathName() << "\n"
                  << "Post-Master Loudness: " << juce::String (finalAnalysis.lufsDb, 2) << " LUFS\n"
                  << "Post-Master Peak:     " << juce::String (finalAnalysis.peakDb, 2) << " dB\n"
                  << "─────────────────────────────────────────────────────────\n";
    }
    else
    {
        std::cout << "[Notice]: No output path specified via (-o). Exiting without rendering a file pass.\n";
    }

    return 0;
}