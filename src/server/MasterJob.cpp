#include "MasterJob.h"
#include "MainAudioProcessor.h"
#include "ceilingIOPipeline.h"

namespace server
{
    // Maximum allowable raw audio download file size (500 Megabytes)
    constexpr int64 maxDownloadSizeBytes = 100 * 1024 * 1024;

    MasterJob::MasterJob (JobStore& storeToUse, SubmitRequest requestToUse, juce::String jobIdToUse)
        : juce::ThreadPoolJob ("master-job-" + jobIdToUse), 
          store (storeToUse), 
          request (std::move (requestToUse)), 
          jobId (std::move (jobIdToUse)) 
    {}

    juce::ThreadPoolJob::JobStatus MasterJob::runJob()
    {
        const auto startedAt = juce::Time::getMillisecondCounterHiRes();

        // Lambda helper to handle unexpected failures safely
        auto fail = [&] (const juce::String& stage, const juce::String& errorCode, const juce::String& msg, int progress)
        {
            const auto elapsed = static_cast<long long>(juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startedAt));
            store.updateAndNotify (jobId, [&] (JobRecord& job) {
                job.status = jobStatusFailed;
                job.progress = progress;
                job.stage = stage;
                job.message = msg;
                job.processingTimeMs = elapsed;
                job.errorCode = errorCode;
                job.errorMessage = msg;
                job.inputLufs = 120.0;
                job.inputRms = 120.0;
                job.inputPeak = 120.0;
                job.suggestedGainDb = 0.0;
                job.outputLufs = -120.0;
                job.outputRms = -120.0;
                job.outputPeak = -120.0;
            });
            return jobHasFinished;
        };

        // 1. Fetch Stage
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.status = jobStatusProcessing; 
            job.progress = 5; 
            job.stage = "fetch"; 
            job.message = "Downloading input asset via presigned gateway";
        });
        juce::Logger::writeToLog ("[DEBUG] Job " + jobId + ": Starting fetch stage for URL: " + request.inputUrl);

        juce::MemoryBlock inputData;
        const juce::URL inputUrl (request.inputUrl);
        auto downloadOptions = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (60000);

        {
            int statusCode = 0;
            auto stream = inputUrl.createInputStream (downloadOptions.withStatusCode (&statusCode));
            if (stream == nullptr)
                return fail ("fetch", "R2_DOWNLOAD_FAILED", "Failed to pull raw asset from presigned address", 5);

            // Security Check: Protect server memory from extremely large or malicious file downloads
            const int64 assetLength = stream->getTotalLength();
            if (assetLength > maxDownloadSizeBytes)
                return fail ("fetch", "PAYLOAD_TOO_LARGE", "Input object size exceeds server allocation limits", 5);

            stream->readIntoMemoryBlock (inputData);
            if (inputData.getSize() == 0)
                return fail ("fetch", "R2_DOWNLOAD_FAILED", "Downloaded input object is empty", 5);
        }
        juce::Logger::writeToLog ( "[DEBUG] Job " + jobId + ": Completed fetch stage, downloaded " + juce::String (inputData.getSize()) + " bytes");


        // 2. Analysis Stage
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.progress = 25; 
            job.stage = "analysis"; 
            job.message = "Analyzing transient vectors";
        });
        juce::Logger::writeToLog ("[DEBUG] Job " + jobId + ": Starting analysis stage on downloaded audio data 25%");

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        
        auto inputStream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (std::move (inputStream)));
        
        if (reader == nullptr) 
            return fail ("analysis", "AUDIO_READ_FAILED", "Unable to open input format channels", 25);

        const auto analysis = ceilingIO::analyseReader (*reader, 2);
        const float renderGainDb = static_cast<float> (request.targetLoudness) - analysis.lufsDb;

        
        store.updateAndNotify (jobId, [&] (JobRecord& job) {
            job.progress = 60; 
            job.stage = "analysis complete"; 
            job.message = "Executing high-performance C++ DSP mastering matrices";
            job.inputLufs = analysis.lufsDb;
            job.inputRms = analysis.rmsDb;
            job.inputPeak = analysis.peakDb;
            job.suggestedGainDb = renderGainDb;
        });
        
        ceilingIO::PlatformPreset serverPreset;
        serverPreset.name = "nestjs-target";
        serverPreset.label = "Custom Target";
        serverPreset.targetLufs = static_cast<float> (request.targetLoudness);
        serverPreset.maxTruePeakDbtp = -1.0f;
        
        MainAudioProcessor processor;
        ceilingIO::applyAnalysisToProcessor (processor, analysis, &serverPreset, renderGainDb);

        // 3. Render Stage
        store.updateAndNotify (jobId, [] (JobRecord& job) {
            job.progress = 65; 
            job.stage = "render"; 
            job.message = "Executing DSP mastering matrices";
        });


        auto freshStream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
        std::unique_ptr<juce::AudioFormatReader> renderReader (formatManager.createReaderFor (std::move (freshStream)));

        juce::MemoryBlock outputData;
        juce::String dspError;
        ceilingIO::AnalysisResult finalAnalysis;
        if (! ceilingIO::renderReaderToMemory (*renderReader, processor, outputData, dspError, &finalAnalysis))
            return fail ("render", "DSP_RENDER_FAILED", dspError.isNotEmpty() ? dspError : "DSP math pipeline failure", 60);

        // 4. Upload Stage
        store.updateAndNotify (jobId, [&] (JobRecord& job) {
            job.progress = 85;
            job.stage = "upload"; 
            job.message = "Uploading complete master up to storage clusters";
        });

        const juce::URL targetUploadUrl (request.outputUrl);
        auto uploadPayloadUrl = targetUploadUrl.withPOSTData (outputData);
        int statusCode = 0;
        auto uploadOptions = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd ("PUT")
            .withExtraHeaders ("Content-Type: audio/wav\n")
            .withConnectionTimeoutMs (60000)
            .withStatusCode (&statusCode);

        auto uploadStream = uploadPayloadUrl.createInputStream (uploadOptions);
        if (uploadStream == nullptr || (statusCode < 200 || statusCode >= 300))
            return fail ("upload", "R2_UPLOAD_FAILED", "Target payload reject under firewall network paths", 85);

        // 5. Complete Stage
        const auto elapsed = static_cast<long long>(juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startedAt));
        store.updateAndNotify (jobId, [&] (JobRecord& job) {
            job.status = jobStatusCompleted; 
            job.progress = 100; 
            job.stage = "finalize"; 
            job.message = "Mastering completed successfully"; 
            job.processingTimeMs = elapsed;
            job.outputLufs = finalAnalysis.lufsDb;
            job.outputRms = finalAnalysis.rmsDb;
            job.outputPeak = finalAnalysis.peakDb;
        });

        return jobHasFinished;
    }
}
