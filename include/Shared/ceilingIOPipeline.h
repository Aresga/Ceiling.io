#pragma once

#include <JuceHeader.h>
#include "MainAudioProcessor.h"

#include <array>

namespace ceilingIO
{
    struct GenrePreset {
        juce::String id;
        juce::String displayName;
        float baseLowShelfDb;
        float baseMidPeakDb;
        float baseHighShelfDb;
        float compRatio;
        float compAttackMs;
        float compReleaseMs;
    };

    struct PlatformPreset
    {
        juce::String name;
        juce::String label;
        float targetLufs = -14.0f;
        float maxTruePeakDbtp = -1.0f;
    };

    struct AnalysisResult
    {
        float lufsDb = -120.0f;
        float rmsDb = -120.0f;
        float peakDb = -120.0f;
        float suggestedThresholdDb = -18.0f;
        float suggestedDriveDb = 0.0f;
    };

    const std::array<PlatformPreset, 5>& getPlatformPresets() noexcept;
    const PlatformPreset* findPlatformPreset (const juce::String& name) noexcept;

    const GenrePreset* findGenrePreset (const juce::String& name) noexcept;

    AnalysisResult analyseReader (juce::AudioFormatReader& reader, int intensity);
    AnalysisResult analyseFile (const juce::File& audioFile, int intensity);

    float computeNormalizationGainDb (const AnalysisResult& analysis, const PlatformPreset& preset) noexcept;
    void applyAnalysisToProcessor (MainAudioProcessor& processor,
                                   const AnalysisResult& analysis,
                                   const PlatformPreset* preset,
                                   const GenrePreset* genre,
                                   float renderGainDb);
                
    bool encodeWavToMp3 (const juce::MemoryBlock& wavData, juce::MemoryBlock& mp3Data);

    bool renderReaderToMemory (juce::AudioFormatReader& reader,
                                MainAudioProcessor& processor,
                                juce::MemoryBlock& outputData,
                                juce::String& errorMessage,
                                AnalysisResult* finalAnalysis = nullptr
                            );

    bool renderFile (const juce::File& inputFile,
                     const juce::File& outputFile,
                     MainAudioProcessor& processor,
                     juce::String& errorMessage);
}