#include "MainAudioProcessor.h"
#include "AnalysisWorker.h"
#include <thread>

#include <functional>
#include <algorithm>
#include <cmath>

// constructor : init APVTS , set up sterio buses in/out
MainAudioProcessor::MainAudioProcessor()
 : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
 ),
   apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{}

MainAudioProcessor::~MainAudioProcessor() = default;

// Allocates RAM and sets up all your digital tools,
// High pass filter, EQ, comp, limiter, 5ms look-ahead buffer
// right before audio processing happens
void MainAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numChannels = (int) getTotalNumOutputChannels();

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (numChannels);

    // Tonal chain (HP + EQ), compressor, lookahead limiter, loudness analyzer.
    eqChain.prepare (sampleRate, samplesPerBlock, numChannels);

    compressor.reset();
    compressor.prepare (spec);

    limiter.prepare (sampleRate, samplesPerBlock, numChannels);

    tempBuffer.setSize (numChannels, samplesPerBlock);

    loudnessAnalyzer.prepare (sampleRate, samplesPerBlock, numChannels);
}

// Clean up RAM, resources ..
void MainAudioProcessor::releaseResources() {}

// Guard against unsupported channel conf such mono
bool MainAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto in = layouts.getChannelSet (true, 0);
    auto out = layouts.getChannelSet (false, 0);
    return in == out && (in == juce::AudioChannelSet::stereo());
}

// process the audio block by block, apply the DSP chain : HPF, EQ, Comp, Limiter
void MainAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    if (numChannels <= 0 || numSamples <= 0)
        return;

    // Real-time safety: NO allocations, NO file I/O, NO locks, NO logging here.
    // Use juce::dsp::ProcessContextReplacing with preallocated buffers.

    // Compute input RMS (pre-processing) for VU meter
    {
        double sumSquaresIn = 0.0;
        int totalSamplesIn = 0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = d[i];
                if (std::isnan (v) || std::isinf (v)) v = 0.0f;
                sumSquaresIn += (double) v * (double) v;
            }
            totalSamplesIn += numSamples;
        }
        double meanIn = sumSquaresIn / std::max(1, totalSamplesIn);
        float rmsIn = (float) std::sqrt (meanIn);
        inputRmsDb.store (juce::Decibels::gainToDecibels (rmsIn, -120.0f));
    }

    // Update DSP parameter values from APVTS (safe to read in audio thread)
    auto* eqLoGainParam = apvts.getRawParameterValue ("EQ_LO_GAIN");
    auto* eqMidGainParam = apvts.getRawParameterValue ("EQ_MID_GAIN");
    auto* eqHiGainParam = apvts.getRawParameterValue ("EQ_HI_GAIN");

    auto* compThreshold = apvts.getRawParameterValue ("COMP_THRESHOLD");
    auto* compRatio     = apvts.getRawParameterValue ("COMP_RATIO");
    auto* compAttack    = apvts.getRawParameterValue ("COMP_ATTACK");
    auto* compRelease   = apvts.getRawParameterValue ("COMP_RELEASE");

    auto* limiterDrive  = apvts.getRawParameterValue ("LIMITER_DRIVE");

    // EQ gains (linear) — update coefficients per block (cheap)
    float loGain  = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqLoGainParam->load()));
    float midGain = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqMidGainParam->load()));
    float hiGain  = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqHiGainParam->load()));

    auto sampleRate = getSampleRate();
    if (sampleRate > 0.0)
        eqChain.updateCoefficients (sampleRate, loGain, midGain, hiGain);

    // Compressor parameters
    compressor.setThreshold (juce::jlimit (-60.0f, 0.0f, compThreshold->load()));
    compressor.setRatio (juce::jlimit (1.0f, 20.0f, compRatio->load()));
    compressor.setAttack (juce::jlimit (0.1f, 500.0f, compAttack->load()));
    compressor.setRelease (juce::jlimit (1.0f, 5000.0f, compRelease->load()));

    // Pre-gain for limiter drive (linear)
    const float driveGain = juce::Decibels::decibelsToGain (juce::jlimit (-12.0f, 24.0f, limiterDrive->load()));
    limiter.setDriveGain (driveGain);

    // Process chain: HP + EQ (EqChain), compressor, limiter.
    // Order is identical to the former inline chain
    // (hp1 -> hp2 -> eqLow -> eqMid -> eqHigh -> compressor -> limiter).
    eqChain.process (buffer);

    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);
    compressor.process (context);

    limiter.process (buffer);

    // Compute output RMS (post-processing) for VU meter
    {
        double sumSquaresOut = 0.0;
        int totalSamplesOut = 0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = d[i];
                if (std::isnan (v) || std::isinf (v)) v = 0.0f;
                sumSquaresOut += (double) v * (double) v;
            }
            totalSamplesOut += numSamples;
        }
        double meanOut = sumSquaresOut / std::max(1, totalSamplesOut);
        float rmsOut = (float) std::sqrt (meanOut);
        outputRmsDb.store (juce::Decibels::gainToDecibels (rmsOut, -120.0f));
    }

    loudnessAnalyzer.process (buffer);
}



juce::AudioProcessorEditor* MainAudioProcessor::createEditor() { return nullptr; }

bool MainAudioProcessor::hasEditor() const { return false; }
const juce::String MainAudioProcessor::getName() const { return "ceilingIO"; }
double MainAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int MainAudioProcessor::getNumPrograms() { return 1; }
int MainAudioProcessor::getCurrentProgram() { return 0; }
void MainAudioProcessor::setCurrentProgram (int) {}
const juce::String MainAudioProcessor::getProgramName (int) { return {}; }
void MainAudioProcessor::changeProgramName (int, const juce::String&) {}

void MainAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (state.isValid())
    {
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        copyXmlToBinary (*xml, destData);
    }
}

void MainAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr)
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

void MainAudioProcessor::applyAnalysisSuggestions()
{
    // Read atomic suggestions from the AnalysisWorker and apply to APVTS on message thread
    const float suggestedTh = analysisWorker.getSuggestedThresholdDb();
    const float suggestedDrive = analysisWorker.getSuggestedDriveDb();

    juce::MessageManager::callAsync ([this, suggestedTh, suggestedDrive]() {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("COMP_THRESHOLD")))
        {
            float v = juce::jlimit (-60.0f, 0.0f, suggestedTh);
            p->setValueNotifyingHost (p->convertTo0to1 (v));
        }

        if (auto* p2 = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("LIMITER_DRIVE")))
        {
            float d = juce::jlimit (0.0f, 12.0f, suggestedDrive);
            p2->setValueNotifyingHost (p2->convertTo0to1 (d));
        }
    });
}

void MainAudioProcessor::startFileAnalysis (const juce::File& file, int intensity, bool autoApply)
{
    analysisWorker.analyzeFile (file, intensity, [this, autoApply]() {
        if (autoApply)
            applyAnalysisSuggestions();
    });
}

juce::AudioProcessorValueTreeState::ParameterLayout MainAudioProcessor::createParameterLayout()
{
    using Parameter = juce::AudioParameterFloat;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // EQ bands
    params.push_back (std::make_unique<Parameter> ("EQ_LO_GAIN",  "EQ Low Gain",  juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));
    params.push_back (std::make_unique<Parameter> ("EQ_MID_GAIN", "EQ Mid Gain", juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));
    params.push_back (std::make_unique<Parameter> ("EQ_HI_GAIN",  "EQ Hi Gain",   juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));

    // Compressor
    params.push_back (std::make_unique<Parameter> ("COMP_THRESHOLD", "Comp Threshold", juce::NormalisableRange<float>(-60.0f, 0.0f), -18.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_RATIO",     "Comp Ratio",     juce::NormalisableRange<float>(1.0f, 20.0f), 2.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_ATTACK",    "Comp Attack",    juce::NormalisableRange<float>(0.1f, 100.0f), 10.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_RELEASE",   "Comp Release",   juce::NormalisableRange<float>(10.0f, 1000.0f), 100.0f));

    // Limiter drive
    params.push_back (std::make_unique<Parameter> ("LIMITER_DRIVE",  "Limiter Drive",  juce::NormalisableRange<float>(0.0f, 18.0f), 0.0f));

    return { params.begin(), params.end() };
}