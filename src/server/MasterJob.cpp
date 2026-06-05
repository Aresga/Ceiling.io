#include "MasterJob.h"
#include "MainAudioProcessor.h"
#include "ceilingIOPipeline.h"

namespace server
{
    // Maximum allowable raw audio download file size (100 MB)
    constexpr juce::int64 maxDownloadSizeBytes = 100 * 1024 * 1024;

    //==============================================================================
    // Stage 1 — Fetch
    // Downloads the raw audio asset from the presigned R2 URL into memory.
    //==============================================================================
    static StageResult fetchStage (const juce::String& inputUrl, juce::MemoryBlock& inputData)
    {
        int statusCode = 0;
        auto stream = juce::URL (inputUrl)
            .createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (60000)
                    .withStatusCode (&statusCode));

        if (stream == nullptr)
            return { false, "R2_DOWNLOAD_FAILED", "Failed to pull raw asset from presigned address" };

        if (stream->getTotalLength() > maxDownloadSizeBytes)
            return { false, "PAYLOAD_TOO_LARGE", "Input object size exceeds server allocation limits" };

        stream->readIntoMemoryBlock (inputData);

        if (inputData.getSize() == 0)
            return { false, "R2_DOWNLOAD_FAILED", "Downloaded input object is empty" };

        return { true, {}, {} };
    }

    //==============================================================================
    // Stage 2 — Analysis
    // Decodes the audio, runs LUFS/RMS/Peak analysis, resolves platform & genre
    // presets, and computes the render gain with headroom protection.
    //==============================================================================
    static StageResult analysisStage (const juce::MemoryBlock& inputData,
                                      const SubmitRequest& request,
                                      juce::AudioFormatManager& formatManager,
                                      ceilingIO::AnalysisResult& analysis,
                                      const ceilingIO::PlatformPreset*& platformPtr,
                                      const ceilingIO::GenrePreset*& genrePtr,
                                      ceilingIO::PlatformPreset& fallbackPreset,
                                      float& renderGainDb)
    {
        auto stream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (std::move (stream)));

        if (reader == nullptr)
            return { false, "AUDIO_READ_FAILED", "Unable to open input format channels" };

        analysis   = ceilingIO::analyseReader (*reader, 2);
        platformPtr = ceilingIO::findPlatformPreset (request.platform);
        genrePtr    = ceilingIO::findGenrePreset (request.genre);

        // Fallback: if no known platform matched, build one from the request's custom target
        if (platformPtr == nullptr)
        {
            fallbackPreset.name            = "custom";
            fallbackPreset.label           = "Custom Target";
            fallbackPreset.targetLufs      = static_cast<float> (request.targetLoudness);
            fallbackPreset.maxTruePeakDbtp = -1.0f;
            platformPtr = &fallbackPreset;
        }

        // Scale down EQ/comp aggressiveness for tracks already louder than -18 LUFS.
        // At -12 LUFS protectionFactor reaches 0.0 — all processing is bypassed.
        // float protectionFactor = 1.0f;
        // if (analysis.lufsDb > -18.0f)
        // {
        //     protectionFactor = juce::jmap  (analysis.lufsDb, -18.0f, -12.0f, 1.0f, 0.0f);
        //     protectionFactor = juce::jlimit (0.0f, 1.0f, protectionFactor);
        // } 
        // // for quite tracks below -18 LUFS, we can actually allow a bit more aggressive processing to hit the target loudness.
        // else if (analysis.lufsDb < -24.0f)
        // {   
        //     protectionFactor = juce::jmap  (analysis.lufsDb, -24.0f, -18.0f, 1.25f, 1.0f);
        //     protectionFactor = juce::jlimit (1.0f, 1.25f, protectionFactor);
        // }   

        // Only compensate headroom for the low shelf boost that will actually fire.
        // If protectionFactor is 0 the EQ won't boost, so the pad stays at 0.
        const float effectiveLowBump   = (genrePtr != nullptr) ? genrePtr->baseLowShelfDb : 0.0f;
        const float dynamicHeadroomPad = (effectiveLowBump > 0.0f) ? -(effectiveLowBump * 0.5f) : 0.0f;

        // How much gain can we apply before peaks hit the ceiling
        const float peakHeadroom   = (platformPtr->maxTruePeakDbtp - analysis.peakDb);  // e.g. -1.0 - (-7.7) = 6.7 dB
        const float gainByLufs     = platformPtr->targetLufs - analysis.lufsDb;         // e.g. +10.5 dB
        const float gainByPeak     = peakHeadroom;                                       // e.g. +6.7 dB

        // Use whichever is smaller — don't push harder than the peaks allow
        renderGainDb   = juce::jmin (gainByLufs, gainByPeak) + dynamicHeadroomPad;


        // renderGainDb = (platformPtr->targetLufs - analysis.lufsDb) + dynamicHeadroomPad;
        if (renderGainDb < 0.0f)
            renderGainDb = 0.0f;
        
        return { true, {}, {} };
    }

    //==============================================================================
    // Stage 3 — DSP Setup + Render
    // Configures the processor with the computed parameters, runs the DSP chain
    // block-by-block, and writes a 24-bit WAV to outputData.
    //==============================================================================
    static StageResult renderStage (const juce::MemoryBlock& inputData,
                                    juce::AudioFormatManager& formatManager,
                                    const ceilingIO::AnalysisResult& analysis,
                                    const ceilingIO::PlatformPreset* platformPtr,
                                    const ceilingIO::GenrePreset* genrePtr,
                                    float renderGainDb,
                                    juce::MemoryBlock& outputData,
                                    ceilingIO::AnalysisResult& finalAnalysis)
    {
        MainAudioProcessor processor;
        ceilingIO::applyAnalysisToProcessor (processor, analysis, platformPtr, genrePtr, renderGainDb);

        auto stream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
        std::unique_ptr<juce::AudioFormatReader> renderReader (formatManager.createReaderFor (std::move (stream)));

        if (renderReader == nullptr)
            return { false, "AUDIO_READ_FAILED", "Unable to re-open audio for render pass" };

        juce::String dspError;
        if (! ceilingIO::renderReaderToMemory (*renderReader, processor, outputData, dspError, &finalAnalysis))
            return { false, "DSP_RENDER_FAILED", dspError.isNotEmpty() ? dspError : "DSP math pipeline failure" };

        return { true, {}, {} };
    }

    //==============================================================================
    // Stage 4 — Upload
    // PUTs the rendered WAV to the presigned output URL on R2.
    //==============================================================================
    static StageResult uploadStage (const juce::String& outputUrl,
                                    const juce::MemoryBlock& outputData,
                                    const juce::String& contentType)
    {
        int statusCode = 0;
        auto uploadStream = juce::URL (outputUrl)
            .withPOSTData (outputData)
            .createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withHttpRequestCmd ("PUT")
                    .withExtraHeaders ("Content-Type: " + contentType + "\n")
                    .withConnectionTimeoutMs (60000)
                    .withStatusCode (&statusCode));

        if (uploadStream == nullptr || statusCode < 200 || statusCode >= 300)
            return { false, "R2_UPLOAD_FAILED", "Target payload rejected under firewall network paths" };

        return { true, {}, {} };
    }

    //==============================================================================
    // MasterJob — constructor
    //==============================================================================
    MasterJob::MasterJob (JobStore& storeToUse, SubmitRequest requestToUse, juce::String jobIdToUse)
        : juce::ThreadPoolJob ("master-job-" + jobIdToUse),
          store (storeToUse),
          request (std::move (requestToUse)),
          jobId   (std::move (jobIdToUse))
    {}

    //==============================================================================
    // MasterJob::runJob — pipeline orchestrator
    //==============================================================================
    juce::ThreadPoolJob::JobStatus MasterJob::runJob()
    {
        const auto startedAt = juce::Time::getMillisecondCounterHiRes();

        // Helper: stamp the job as failed and return immediately
        auto fail = [&] (const juce::String& stage,
                         const StageResult& result,
                         int progress) -> juce::ThreadPoolJob::JobStatus
        {
            const auto elapsed = static_cast<long long> (
                juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startedAt));

            store.updateAndNotify (jobId, [&] (JobRecord& job) {
                job.status           = jobStatusFailed;
                job.progress         = progress;
                job.stage            = stage;
                job.message          = result.errorMessage;
                job.processingTimeMs = elapsed;
                job.errorCode        = result.errorCode;
                job.errorMessage     = result.errorMessage;
                job.inputLufs        = -120.0;
                job.inputRms         = -120.0;
                job.inputPeak        = -120.0;
                job.suggestedGainDb  = 0.0;
                job.outputLufs       = -120.0;
                job.outputRms        = -120.0;
                job.outputPeak       = -120.0;
            });
            return jobHasFinished;
        };

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        // ── Fetch ────────────────────────────────────────────────────────────────
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.status   = jobStatusProcessing;
            job.progress = 5;
            job.stage    = "fetch";
            job.message  = "Downloading input asset via presigned gateway";
        });

        juce::MemoryBlock inputData;
        if (auto r = fetchStage (request.inputUrl, inputData); !r.ok)
            return fail ("fetch", r, 5);

        // ── Analysis ─────────────────────────────────────────────────────────────
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.progress = 25;
            job.stage    = "analysis";
            job.message  = "Analyzing transient vectors";
        });

        ceilingIO::AnalysisResult        analysis;
        const ceilingIO::PlatformPreset* platformPtr = nullptr;
        const ceilingIO::GenrePreset*    genrePtr    = nullptr;
        ceilingIO::PlatformPreset        fallbackPreset;
        float                            renderGainDb = 0.0f;

        if (auto r = analysisStage (inputData, request, formatManager,
                                    analysis, platformPtr, genrePtr,
                                    fallbackPreset, renderGainDb); !r.ok)
            return fail ("analysis", r, 25);

        store.updateAndNotify (jobId, [&] (JobRecord& job) {
            job.progress        = 60;
            job.stage           = "analysis complete";
            job.message         = "Executing DSP mastering matrices";
            job.inputLufs       = analysis.lufsDb;
            job.inputRms        = analysis.rmsDb;
            job.inputPeak       = analysis.peakDb;
            job.suggestedGainDb = renderGainDb;
        });

        // ── Render ───────────────────────────────────────────────────────────────
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.progress = 65;
            job.stage    = "render";
            job.message  = "Executing DSP mastering matrices";
        });

        juce::MemoryBlock             outputData;
        ceilingIO::AnalysisResult     finalAnalysis;


        if (auto r = renderStage (inputData, formatManager, analysis,
                                  platformPtr, genrePtr, renderGainDb,
                                  outputData, finalAnalysis); !r.ok)
            return fail ("render", r, 65);
        
        // ── Transcode (MP3 output) ───────────────────────────────────
        if (request.outputFormat == "MP3")
        {
            juce::MemoryBlock mp3Data;
            if (! ceilingIO::encodeWavToMp3 (outputData, mp3Data))
                return fail ("transcode", { false, "MP3_ENCODE_FAILED", "LAME encoding failed" }, 80);

            outputData = std::move (mp3Data);
        }

        // ── Upload ───────────────────────────────────────────────────────────────
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.progress = 85;
            job.stage    = "upload";
            job.message  = "Uploading complete master to storage clusters";
        });

        const juce::String contentType = (request.outputFormat == "MP3") ? "audio/mpeg" : "audio/wav";
        juce::Logger::writeToLog ("[Info]: Uploading with content type: " + contentType);

        if (auto r = uploadStage (request.outputUrl, outputData, contentType); !r.ok)
            return fail ("upload", r, 85);

        // ── Complete ─────────────────────────────────────────────────────────────
        const auto elapsed = static_cast<long long> (
            juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startedAt));

        store.updateAndNotify (jobId, [&] (JobRecord& job) {
            job.status           = jobStatusCompleted;
            job.progress         = 100;
            job.stage            = "finalize";
            job.message          = "Mastering completed successfully";
            job.processingTimeMs = elapsed;
            job.outputLufs       = finalAnalysis.lufsDb;
            job.outputRms        = finalAnalysis.rmsDb;
            job.outputPeak       = finalAnalysis.peakDb;
        });

        return jobHasFinished;
    }

} // namespace server