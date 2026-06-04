#include "MainAudioProcessor.h"
#include "AnalysisWorker.h"
#include <thread>

#include <functional>
#include <algorithm>
#include <cmath>

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
}

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
    juce::ignoreUnused (sampleRate, samplesPerBlock);
    // Reserve and prepare DSP objects here (not allocated on audio thread)
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    // High-pass 4th order Butterworth at 30 Hz (cascade of two 2nd-order sections)
    auto hpCoeffs1 = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 30.0, std::sqrt(2.0f));
    auto hpCoeffs2 = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 30.0, std::sqrt(2.0f));
    hp1.state = *hpCoeffs1;
    hp2.state = *hpCoeffs2;
    hp1.prepare(spec);
    hp2.prepare(spec);

    // EQ initial coefficients (will be updated each block)
    eqLow.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(sampleRate, 100.0f, 0.7071f, 1.0f);
    eqMid.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate, 1000.0f, 1.0f, 1.0f);
    eqHigh.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(sampleRate, 10000.0f, 0.7071f, 1.0f);
    eqLow.prepare(spec);
    eqMid.prepare(spec);
    eqHigh.prepare(spec);

    compressor.reset();
    compressor.prepare(spec);

    // Limiter lookahead: 5 ms default
    lookaheadSamples = std::max(1, static_cast<int> (std::floor (0.005 * sampleRate)));
    limiterBufferLen = lookaheadSamples + samplesPerBlock + 4;

    limiterDelayBuffers.clear();
    limiterDelayBuffers.resize ((size_t) spec.numChannels);
    limiterWriteIndex.assign ((size_t) spec.numChannels, 0);
    limiterReadIndex.assign ((size_t) spec.numChannels, 0);

    for (size_t ch = 0; ch < limiterDelayBuffers.size(); ++ch)
    {
        limiterDelayBuffers[ch].assign ((size_t) limiterBufferLen, 0.0f);
        limiterWriteIndex[ch] = 0;
        limiterReadIndex[ch] = (limiterWriteIndex[ch] + limiterBufferLen - lookaheadSamples) % limiterBufferLen;
    }

    tempBuffer.setSize ((int) spec.numChannels, samplesPerBlock);

    // Initialize monotonic queues and counters for optimized sliding max
    limiterQueues.clear();
    limiterQueues.resize ((size_t) spec.numChannels);
    limiterSampleCounters.assign ((size_t) spec.numChannels, 0);
    int mqCap = lookaheadSamples + 4;
    for (size_t ch = 0; ch < limiterQueues.size(); ++ch)
        limiterQueues[ch].init (mqCap);

    loudnessAnalyzer.prepare (sampleRate, samplesPerBlock, (int) spec.numChannels);
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

    // Update EQ coefficients per block (cheap)
    float loGain = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqLoGainParam->load()));
    float midGain = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqMidGainParam->load()));
    float hiGain = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, eqHiGainParam->load()));

    // recreate coefficients with appropriate linear gain applied
    auto sampleRate = getSampleRate();
    if (sampleRate > 0.0)
    {
        auto newLow = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, 100.0f, 0.7071f, loGain);
        auto newMid = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f, midGain);
        auto newHigh = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 10000.0f, 0.7071f, hiGain);
        *eqLow.state = *newLow;
        *eqMid.state = *newMid;
        *eqHigh.state = *newHigh;
    }

    // Compressor parameters
    compressor.setThreshold (juce::jlimit (-60.0f, 0.0f, compThreshold->load()));
    compressor.setRatio (juce::jlimit (1.0f, 20.0f, compRatio->load()));
    compressor.setAttack (juce::jlimit (0.1f, 500.0f, compAttack->load()));
    compressor.setRelease (juce::jlimit (1.0f, 5000.0f, compRelease->load()));

    // Pre-gain for limiter drive (dB)
    const float driveGain = juce::Decibels::decibelsToGain (juce::jlimit (-12.0f, 24.0f, limiterDrive->load()));

    juce::dsp::AudioBlock<float> block (buffer);

    // Process chain: HP (two sections), EQ (low, mid, high), compressor
    juce::dsp::ProcessContextReplacing<float> context (block);
    hp1.process (context);
    hp2.process (context);
    eqLow.process (context);
    eqMid.process (context);
    eqHigh.process (context);
    compressor.process (context);

    // Limiter: simple look-ahead brickwall limiter fixed at -1.0 dBFS
    const float linearCeiling = juce::Decibels::decibelsToGain (limiterCeilingDbtp);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* channelData = buffer.getWritePointer (ch);
        auto& delayBuf = limiterDelayBuffers[(size_t) ch];
        int writeIdx = limiterWriteIndex[(size_t) ch];
        int readIdx = limiterReadIndex[(size_t) ch];
        auto& mq = limiterQueues[(size_t) ch];
        long long& counter = limiterSampleCounters[(size_t) ch];

        for (int i = 0; i < numSamples; ++i)
        {
            float x = channelData[i] * driveGain;
            if (std::isnan (x) || std::isinf (x))
                x = 0.0f;

            // write incoming sample into delay buffer
            delayBuf[(size_t) writeIdx] = x;

            // sliding max using monotonic queue (preallocated, no allocs)
            float absx = std::abs (x);
            long long idx = counter++;

            // pop back while last <= absx
            while (mq.size > 0 && mq.back_val() <= absx)
                mq.pop_back();

            // push new
            mq.push_back (idx, absx);

            // pop front if out of window
            while (mq.size > 0 && mq.front_idx() <= idx - lookaheadSamples)
                mq.pop_front();

            float peak = mq.empty() ? 0.0f : mq.front_val();

            float requiredGain = 1.0f;
            if (peak > 1e-12f)
                requiredGain = std::min (1.0f, linearCeiling / peak);

            // output delayed sample multiplied by required gain
            float out = delayBuf[(size_t) readIdx] * requiredGain;

            // final safety clamp
            if (std::isnan (out) || std::isinf (out))
                out = 0.0f;
            out = juce::jlimit (-1.0f, 1.0f, out);

            channelData[i] = out;

            // advance indices
            ++writeIdx; if (writeIdx >= limiterBufferLen) writeIdx = 0;
            ++readIdx;  if (readIdx  >= limiterBufferLen) readIdx  = 0;
        }

        limiterWriteIndex[(size_t) ch] = writeIdx;
        limiterReadIndex[(size_t) ch] = readIdx;
    }

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



void MainAudioProcessor::LoudnessAnalyzer::prepare (double newSampleRate, int samplesPerBlock, int channels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    numChannels = std::max (1, channels);

    momentaryWindowSamples = std::max (1, (int) std::ceil (0.4 * sampleRate));
    shortTermWindowSamples = std::max (1, (int) std::ceil (3.0 * sampleRate));
    samplesSinceShortTermUpdate = 0;

    momentaryRing.assign ((size_t) momentaryWindowSamples, 0.0f);
    shortTermRing.assign ((size_t) shortTermWindowSamples, 0.0f);
    momentaryIndex = 0;
    shortTermIndex = 0;
    momentarySamplesFilled = 0;
    shortTermSamplesFilled = 0;
    momentarySum = 0.0;
    shortTermSum = 0.0;

    historySize = 600;
    shortTermHistoryEnergy.assign ((size_t) historySize, 0.0f);
    historyIndex = 0;
    historyCount = 0;
    percentileWorkspace.assign ((size_t) historySize, 0.0f);

    analysisBuffer.setSize (numChannels, samplesPerBlock, false, false, true);

    kWeightChains.clear();
    kWeightChains.resize ((size_t) numChannels);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = 1;

    auto hpCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 40.0, 0.5f);
    auto shelfCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sampleRate, 1500.0, 0.7071f, juce::Decibels::decibelsToGain (4.0f));

    for (auto& chain : kWeightChains)
    {
        chain.prepare (spec);
        chain.get<0>().coefficients = hpCoeffs;
        chain.get<1>().coefficients = shelfCoeffs;
        chain.reset();
    }

    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        numChannels, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false);
    oversampler->initProcessing ((juce::uint32) samplesPerBlock);
    oversampler->reset();

    integratedLufs.store (-120.0f);
    shortTermLufs.store (-120.0f);
    momentaryLufs.store (-120.0f);
    loudnessRange.store (0.0f);
    truePeakMaxDbtp.store (-120.0f);
}

void MainAudioProcessor::LoudnessAnalyzer::reset()
{
    momentaryRing.assign (momentaryRing.size(), 0.0f);
    shortTermRing.assign (shortTermRing.size(), 0.0f);
    momentaryIndex = 0;
    shortTermIndex = 0;
    momentarySamplesFilled = 0;
    shortTermSamplesFilled = 0;
    momentarySum = 0.0;
    shortTermSum = 0.0;
    samplesSinceShortTermUpdate = 0;
    historyIndex = 0;
    historyCount = 0;
    std::fill (shortTermHistoryEnergy.begin(), shortTermHistoryEnergy.end(), 0.0f);
    integratedLufs.store (-120.0f);
    shortTermLufs.store (-120.0f);
    momentaryLufs.store (-120.0f);
    loudnessRange.store (0.0f);
    truePeakMaxDbtp.store (-120.0f);
    if (oversampler != nullptr)
        oversampler->reset();
    for (auto& chain : kWeightChains)
        chain.reset();
}

void MainAudioProcessor::LoudnessAnalyzer::process (const juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    if (numSamples > analysisBuffer.getNumSamples())
        return;

    const int copyChannels = std::min (buffer.getNumChannels(), analysisBuffer.getNumChannels());
    for (int ch = 0; ch < copyChannels; ++ch)
        analysisBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
    for (int ch = copyChannels; ch < analysisBuffer.getNumChannels(); ++ch)
        analysisBuffer.clear (ch, 0, numSamples);

    if (oversampler != nullptr)
    {
        juce::dsp::AudioBlock<float> baseBlock (analysisBuffer);
        auto upBlock = oversampler->processSamplesUp (baseBlock);
        float truePeak = 0.0f;
        for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
        {
            const float* data = upBlock.getChannelPointer (ch);
            for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
                truePeak = std::max (truePeak, std::abs (data[i]));
        }
        oversampler->processSamplesDown (baseBlock);
        truePeakMaxDbtp.store (juce::Decibels::gainToDecibels (truePeak, -120.0f));
    }

    juce::dsp::AudioBlock<float> block (analysisBuffer);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto channelBlock = block.getSingleChannelBlock ((size_t) ch);
        juce::dsp::ProcessContextReplacing<float> ctx (channelBlock);
        kWeightChains[(size_t) ch].process (ctx);
    }

    const float* channelPtrs[64] = {};
    const int chCount = std::min (numChannels, 64);
    for (int ch = 0; ch < chCount; ++ch)
        channelPtrs[ch] = analysisBuffer.getReadPointer (ch);

    for (int i = 0; i < numSamples; ++i)
    {
        float sumSquares = 0.0f;
        for (int ch = 0; ch < chCount; ++ch)
        {
            const float v = channelPtrs[ch][i];
            sumSquares += v * v;
        }

        if (momentarySamplesFilled < momentaryWindowSamples)
            ++momentarySamplesFilled;
        momentarySum -= momentaryRing[(size_t) momentaryIndex];
        momentaryRing[(size_t) momentaryIndex] = sumSquares;
        momentarySum += sumSquares;
        ++momentaryIndex;
        if (momentaryIndex >= momentaryWindowSamples)
            momentaryIndex = 0;

        if (shortTermSamplesFilled < shortTermWindowSamples)
            ++shortTermSamplesFilled;
        shortTermSum -= shortTermRing[(size_t) shortTermIndex];
        shortTermRing[(size_t) shortTermIndex] = sumSquares;
        shortTermSum += sumSquares;
        ++shortTermIndex;
        if (shortTermIndex >= shortTermWindowSamples)
            shortTermIndex = 0;
    }

    const int momentaryDenom = std::max (1, momentarySamplesFilled * numChannels);
    const int shortTermDenom = std::max (1, shortTermSamplesFilled * numChannels);
    const double momentaryMean = momentarySum / (double) momentaryDenom;
    const double shortTermMean = shortTermSum / (double) shortTermDenom;

    momentaryLufs.store (meanSquareToLufs (momentaryMean));
    shortTermLufs.store (meanSquareToLufs (shortTermMean));

    samplesSinceShortTermUpdate += numSamples;
    const int updateInterval = (int) std::max (1.0, sampleRate);
    while (samplesSinceShortTermUpdate >= updateInterval)
    {
        samplesSinceShortTermUpdate -= updateInterval;
        shortTermHistoryEnergy[(size_t) historyIndex] = (float) shortTermMean;
        historyIndex = (historyIndex + 1) % historySize;
        historyCount = std::min (historyCount + 1, historySize);

        double absGateEnergy = lufsToMeanSquare (kAbsoluteGateLufs);
        double ungatedSum = 0.0;
        int ungatedCount = 0;
        for (int i = 0; i < historyCount; ++i)
        {
            const double e = shortTermHistoryEnergy[(size_t) i];
            if (e > absGateEnergy)
            {
                ungatedSum += e;
                ++ungatedCount;
            }
        }

        float integrated = -120.0f;
        if (ungatedCount > 0)
        {
            const double ungatedMean = ungatedSum / (double) ungatedCount;
            const float ungatedLufs = meanSquareToLufs (ungatedMean);
            const float relativeGateLufs = ungatedLufs - 10.0f;
            const double relativeGateEnergy = lufsToMeanSquare (relativeGateLufs);
            const double gateEnergy = std::max (absGateEnergy, relativeGateEnergy);

            double gatedSum = 0.0;
            int gatedCount = 0;
            for (int i = 0; i < historyCount; ++i)
            {
                const double e = shortTermHistoryEnergy[(size_t) i];
                if (e > gateEnergy)
                {
                    gatedSum += e;
                    ++gatedCount;
                }
            }

            if (gatedCount > 0)
                integrated = meanSquareToLufs (gatedSum / (double) gatedCount);
        }

        integratedLufs.store (integrated);

        int lraCount = 0;
        for (int i = 0; i < historyCount; ++i)
        {
            const double e = shortTermHistoryEnergy[(size_t) i];
            if (e > absGateEnergy)
            {
                percentileWorkspace[(size_t) lraCount] = meanSquareToLufs (e);
                ++lraCount;
            }
        }

        if (lraCount >= 2)
        {
            const int idx10 = (int) std::floor (0.1f * (lraCount - 1));
            const int idx95 = (int) std::floor (0.95f * (lraCount - 1));
            std::nth_element (percentileWorkspace.begin(), percentileWorkspace.begin() + idx10,
                              percentileWorkspace.begin() + lraCount);
            const float p10 = percentileWorkspace[(size_t) idx10];
            std::nth_element (percentileWorkspace.begin(), percentileWorkspace.begin() + idx95,
                              percentileWorkspace.begin() + lraCount);
            const float p95 = percentileWorkspace[(size_t) idx95];
            loudnessRange.store (std::max (0.0f, p95 - p10));
        }
        else
        {
            loudnessRange.store (0.0f);
        }
    }
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
    params.push_back (std::make_unique<Parameter> ("EQ_MID_GAIN", "EQ Mid Gain",  juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));
    params.push_back (std::make_unique<Parameter> ("EQ_HI_GAIN",  "EQ Hi Gain",   juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));

    // Compressor
    params.push_back (std::make_unique<Parameter> ("COMP_THRESHOLD", "Comp Threshold", juce::NormalisableRange<float>(-60.0f, 0.0f), -18.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_RATIO",     "Comp Ratio",     juce::NormalisableRange<float>(1.0f, 20.0f), 2.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_ATTACK",    "Comp Attack",    juce::NormalisableRange<float>(0.1f, 100.0f), 10.0f));
    params.push_back (std::make_unique<Parameter> ("COMP_RELEASE",   "Comp Release",   juce::NormalisableRange<float>(10.0f, 1000.0f), 100.0f));

    // Limiter drive
    params.push_back (std::make_unique<Parameter> ("LIMITER_DRIVE",  "Limiter Drive",  juce::NormalisableRange<float>(-12.0f, 12.0f), 0.0f));

    return { params.begin(), params.end() };
}
