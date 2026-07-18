#pragma once

#include <JuceHeader.h>
#include "AnalysisWorker.h"
#include "dsp/LoudnessAnalyzer.h"
#include "dsp/LookaheadLimiter.h"
#include "dsp/EqChain.h"

class MainAudioProcessor  : public juce::AudioProcessor
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    MainAudioProcessor();
    ~MainAudioProcessor() override;

    // AudioProcessor overrides
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    // Programs
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    // State
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static APVTS::ParameterLayout createParameterLayout();

    APVTS apvts;

    // DSP chain components. The tonal chain (HP + EQ) lives in EqChain, the
    // brickwall limiter in LookaheadLimiter, and the EBU R128 loudness/true-peak
    // meter in dsp::LoudnessAnalyzer. The compressor stays inline here.
    ceilingIO::dsp::EqChain eqChain;
    juce::dsp::Compressor<float> compressor;
    ceilingIO::dsp::LookaheadLimiter limiter;

    // Preallocated temp buffer for processing
    juce::AudioBuffer<float> tempBuffer;

    // Offline analysis worker (runs on background thread)
    AnalysisWorker analysisWorker;

    // Call from UI/main thread to request file analysis. This schedules
    // the analysis on a background thread and applies suggestions when done.
    void startFileAnalysis (const juce::File& file, int intensity, bool autoApply);

    // Apply analysis suggestions to APVTS parameters on the message thread
    void applyAnalysisSuggestions();

    // Analysis getters (thread-safe, read atomics)
    float getAnalysisSuggestedThresholdDb() const noexcept { return analysisWorker.getSuggestedThresholdDb(); }
    float getAnalysisSuggestedDriveDb() const noexcept { return analysisWorker.getSuggestedDriveDb(); }
    float getAnalysisLastRmsDb() const noexcept { return analysisWorker.getLastRmsDb(); }
    float getAnalysisLastPeakDb() const noexcept { return analysisWorker.getLastPeakDb(); }

    // VU meters (lock-free atomics updated on audio thread)
    std::atomic<float> inputRmsDb{ -120.0f };
    std::atomic<float> outputRmsDb{ -120.0f };
    float getInputRmsDb() const noexcept { return inputRmsDb.load(); }
    float getOutputRmsDb() const noexcept { return outputRmsDb.load(); }

    // Loudness/true-peak meters (EBU R128 style analysis) — delegated to the
    // extracted dsp::LoudnessAnalyzer.
    float getIntegratedLufs() const noexcept { return loudnessAnalyzer.integratedLufs.load(); }
    float getShortTermLufs() const noexcept { return loudnessAnalyzer.shortTermLufs.load(); }
    float getMomentaryLufs() const noexcept { return loudnessAnalyzer.momentaryLufs.load(); }
    float getLoudnessRange() const noexcept { return loudnessAnalyzer.loudnessRange.load(); }
    float getTruePeakMaxDbtp() const noexcept { return loudnessAnalyzer.truePeakMaxDbtp.load(); }

    void setLimiterCeilingDbtp (float ceilingDbtp) noexcept { limiter.setCeilingDbtp (ceilingDbtp); }

    ceilingIO::dsp::LoudnessAnalyzer loudnessAnalyzer;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainAudioProcessor)
};