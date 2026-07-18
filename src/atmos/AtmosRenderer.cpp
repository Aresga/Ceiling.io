#include "atmos/AtmosRenderer.h"

#include <algorithm>

namespace ceilingIO::atmos
{
    void AtmosRenderer::prepare (double sampleRate, int bedChannels, int numObjects, int samplesPerBlock)
    {
        sampleRate_  = sampleRate;
        bedChannels_ = bedChannels;
        numObjects_  = numObjects;
        samplesPerBlock_ = samplesPerBlock;

        eqChain_.prepare (sampleRate, samplesPerBlock, bedChannels);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
        spec.numChannels      = (juce::uint32) bedChannels;
        compressor_.reset();
        compressor_.prepare (spec);
        // Mild bed compression (minimal config — not yet exposed via AtmosRenderConfig)
        compressor_.setThreshold (-20.0f);
        compressor_.setRatio (2.0f);
        compressor_.setAttack (10.0f);
        compressor_.setRelease (100.0f);

        limiter_.prepare (sampleRate, samplesPerBlock, bedChannels + numObjects);
        limiter_.setMode (ceilingIO::dsp::LimiterMode::Linked);

        analyzer_.prepare (sampleRate, samplesPerBlock, bedChannels, numObjects);
        postAnalyzer_.prepare (sampleRate, samplesPerBlock, bedChannels, numObjects);

        objectGainDb_.assign ((size_t) numObjects, 0.0f);
        bedGainDb_ = 0.0f;
        concatBuffer_.setSize (bedChannels + numObjects, samplesPerBlock);
    }

    void AtmosRenderer::setConfig (const AtmosRenderConfig& config)
    {
        config_ = config;
        limiter_.setMode (config.linkedLimiting
                             ? ceilingIO::dsp::LimiterMode::Linked
                             : ceilingIO::dsp::LimiterMode::Independent);
        limiter_.setCeilingDbtp (config.maxTruePeakDbtp);
        limiter_.setDriveGain (1.0f);
    }

    void AtmosRenderer::processBlock (juce::AudioBuffer<float>& bedBuffer,
                                        std::vector<juce::AudioBuffer<float>>& objectBuffers)
    {
        const int n      = bedBuffer.getNumSamples();
        const int bedCh  = bedBuffer.getNumChannels();
        const int numObj = (int) objectBuffers.size();
        if (n <= 0 || bedCh <= 0)
            return;

        // 1. Bed gain (uniform)
        const float bedGainLin = juce::Decibels::decibelsToGain (bedGainDb_);
        if (bedGainLin != 1.0f)
            bedBuffer.applyGain (bedGainLin);

        // 2. EQ on bed (flat in the minimal config)
        eqChain_.process (bedBuffer);

        // 3. Compression on bed
        {
            juce::dsp::AudioBlock<float> block (bedBuffer);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            compressor_.process (ctx);
        }

        // 4. Per-object gain
        for (int k = 0; k < numObj; ++k)
        {
            const float g = juce::Decibels::decibelsToGain (
                k < (int) objectGainDb_.size() ? objectGainDb_[(size_t) k] : 0.0f);
            if (g != 1.0f)
                objectBuffers[(size_t) k].applyGain (g);
        }

        // 5. Linked limiting across bed + all objects
        const int total = bedCh + numObj;
        if (total <= 0)
            return;

        concatBuffer_.setSize (total, n, false, false, true);
        for (int ch = 0; ch < bedCh; ++ch)
            concatBuffer_.copyFrom (ch, 0, bedBuffer, ch, 0, n);
        for (int k = 0; k < numObj; ++k)
            concatBuffer_.copyFrom (bedCh + k, 0, objectBuffers[(size_t) k], 0, 0, n);

        limiter_.process (concatBuffer_);

        // Copy back
        for (int ch = 0; ch < bedCh; ++ch)
            bedBuffer.copyFrom (ch, 0, concatBuffer_, ch, 0, n);
        for (int k = 0; k < numObj; ++k)
            objectBuffers[(size_t) k].copyFrom (0, 0, concatBuffer_, bedCh + k, 0, n);
    }

    bool AtmosRenderer::renderToMemory (AdmBwfReader& input,
                                          juce::MemoryBlock& outputData,
                                          const AtmosRenderConfig& config,
                                          juce::String& errorMessage,
                                          AtmosLoudnessResult* finalAnalysis)
    {
        outputData.reset();

        const int    bedChannels = input.getNumBedChannels();
        const int    numObjects  = input.getNumObjects();
        const double sampleRate  = input.getSampleRate();
        const juce::int64 length = input.getLengthInSamples();

        if (bedChannels <= 0 || length <= 0)
        {
            errorMessage = "ADM reader is not prepared";
            return false;
        }

        constexpr int blockSize = 1024;
        prepare (sampleRate, bedChannels, numObjects, blockSize);
        setConfig (config);

        juce::AudioBuffer<float> bedBuf (bedChannels, blockSize);
        std::vector<juce::AudioBuffer<float>> objBufs ((size_t) numObjects);
        for (auto& b : objBufs) b.setSize (1, blockSize);

        // ── Pass 1: analyze input loudness ─────────────────────────────────
        analyzer_.reset();
        juce::int64 pos = 0;
        while (pos < length)
        {
            const int n = (int) std::min<juce::int64> (blockSize, length - pos);
            if (! input.readBlock (bedBuf, objBufs, pos, n))
            {
                errorMessage = "ADM block read failed during analysis";
                return false;
            }
            analyzer_.processBedBlock (bedBuf);
            for (int k = 0; k < numObjects; ++k)
                analyzer_.processObjectBlock (objBufs[(size_t) k], k);
            pos += n;
        }

        const AtmosLoudnessResult inputLoud = analyzer_.getResult();

        // Uniform composite gain to hit the target (preserves bed/object
        // balance). Clamped at >= 0 dB to mirror the stereo path's behaviour.
        const float compositeGainDb = juce::jmax (0.0f, config.targetCompositeLufs - inputLoud.integratedLufsComposite);
        bedGainDb_ = compositeGainDb;
        objectGainDb_.assign ((size_t) numObjects, compositeGainDb);

        // ── Open writer (axml pass-through) ─────────────────────────────────
        auto outStream = std::make_unique<juce::MemoryOutputStream> (outputData, false);
        AdmBwfWriter writer;
        if (! writer.open (*outStream, sampleRate, bedChannels + numObjects, 24,
                            input.getBedLayout(), input.getAxmlChunk()))
        {
            errorMessage = "Unable to create ADM BWF writer";
            return false;
        }

        // ── Pass 2: process + write + post-analyze ──────────────────────────
        postAnalyzer_.reset();
        pos = 0;
        while (pos < length)
        {
            const int n = (int) std::min<juce::int64> (blockSize, length - pos);
            if (! input.readBlock (bedBuf, objBufs, pos, n))
            {
                errorMessage = "ADM block read failed during render";
                return false;
            }

            processBlock (bedBuf, objBufs);

            postAnalyzer_.processBedBlock (bedBuf);
            for (int k = 0; k < numObjects; ++k)
                postAnalyzer_.processObjectBlock (objBufs[(size_t) k], k);

            if (! writer.writeBlock (bedBuf, objBufs, n))
            {
                errorMessage = "ADM write failed";
                return false;
            }
            pos += n;
        }

        if (! writer.close())
        {
            errorMessage = "ADM BWF finalize failed";
            return false;
        }

        outStream->flush();
        outStream.reset();

        if (finalAnalysis != nullptr)
            *finalAnalysis = postAnalyzer_.getResult();

        return true;
    }
} // namespace ceilingIO::atmos