#pragma once

#include <JuceHeader.h>
#include "AnalysisWorker.h"

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
    
    // DSP chain components
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> hp1;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> hp2;

    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> eqLow;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> eqMid;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> eqHigh;

    juce::dsp::Compressor<float> compressor;

    // Limiter: simple lookahead buffer per channel (preallocated in prepareToPlay)
    int lookaheadSamples = 0;
    int limiterBufferLen = 0;
    std::vector<std::vector<float>> limiterDelayBuffers;
    std::vector<int> limiterWriteIndex;
    std::vector<int> limiterReadIndex;

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

    // Loudness/true-peak meters (EBU R128 style analysis)
    float getIntegratedLufs() const noexcept { return loudnessAnalyzer.integratedLufs.load(); }
    float getShortTermLufs() const noexcept { return loudnessAnalyzer.shortTermLufs.load(); }
    float getMomentaryLufs() const noexcept { return loudnessAnalyzer.momentaryLufs.load(); }
    float getLoudnessRange() const noexcept { return loudnessAnalyzer.loudnessRange.load(); }
    float getTruePeakMaxDbtp() const noexcept { return loudnessAnalyzer.truePeakMaxDbtp.load(); }

    void setLimiterCeilingDbtp (float ceilingDbtp) noexcept { limiterCeilingDbtp = ceilingDbtp; }

    // Limiter optimization: monotonic queue per channel (preallocated in prepareToPlay)
    struct MonotonicQueue
    {
        std::vector<float> vals;
        std::vector<long long> idxs;
        int head = 0;
        int size = 0;
        int capacity = 0;

        void init(int cap)
        {
            capacity = cap;
            vals.assign ((size_t) cap, 0.0f);
            idxs.assign ((size_t) cap, 0);
            head = 0;
            size = 0;
        }

        inline bool empty() const noexcept { return size == 0; }
        inline float back_val() const noexcept { return vals[(head + size - 1) % capacity]; }
        inline long long back_idx() const noexcept { return idxs[(head + size - 1) % capacity]; }
        inline float front_val() const noexcept { return vals[head]; }
        inline long long front_idx() const noexcept { return idxs[head]; }

        inline void pop_back() noexcept { if (size > 0) --size; }
        inline void pop_front() noexcept { if (size > 0) { head = (head + 1) % capacity; --size; } }
        inline void push_back(long long idx, float v) noexcept
        {
            vals[(head + size) % capacity] = v;
            idxs[(head + size) % capacity] = idx;
            ++size;
        }
    };

    std::vector<MonotonicQueue> limiterQueues;
    std::vector<long long> limiterSampleCounters;

    struct LoudnessAnalyzer
    {
        void prepare (double sampleRate, int samplesPerBlock, int numChannels);
        void reset();
        void process (const juce::AudioBuffer<float>& buffer);

        std::atomic<float> integratedLufs{ -120.0f };
        std::atomic<float> shortTermLufs{ -120.0f };
        std::atomic<float> momentaryLufs{ -120.0f };
        std::atomic<float> loudnessRange{ 0.0f };
        std::atomic<float> truePeakMaxDbtp{ -120.0f };

        double sampleRate = 44100.0;
        int numChannels = 2;

        int momentaryWindowSamples = 0;
        int shortTermWindowSamples = 0;
        int samplesSinceShortTermUpdate = 0;

        std::vector<float> momentaryRing;
        std::vector<float> shortTermRing;
        int momentaryIndex = 0;
        int shortTermIndex = 0;
        int momentarySamplesFilled = 0;
        int shortTermSamplesFilled = 0;
        double momentarySum = 0.0;
        double shortTermSum = 0.0;

        int historySize = 600;
        std::vector<float> shortTermHistoryEnergy;
        int historyIndex = 0;
        int historyCount = 0;
        std::vector<float> percentileWorkspace;

        juce::AudioBuffer<float> analysisBuffer;
        std::vector<juce::dsp::ProcessorChain<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Filter<float>>> kWeightChains;
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    };

    LoudnessAnalyzer loudnessAnalyzer;
    float limiterCeilingDbtp = -1.0f;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainAudioProcessor)
};
