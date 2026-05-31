#include <JuceHeader.h>
#include "ceilingIOPipeline.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <crow.h>

namespace
{
    constexpr const char* jobStatusQueued = "QUEUED";
    constexpr const char* jobStatusProcessing = "PROCESSING";
    constexpr const char* jobStatusCompleted = "COMPLETED";
    constexpr const char* jobStatusFailed = "FAILED";
    constexpr const char* jobStatusCancelled = "CANCELLED";

    struct SubmitRequest
    {
        juce::String trackId;
        juce::String inputUrl;     // 💥 CHANGED: Direct secure download URL passed by NestJS
        juce::String outputUrl;    // 💥 CHANGED: Direct secure upload target URL passed by NestJS
        juce::String outputKey;    // The target storage key to report back to NestJS
        double targetLoudness = 0.0;
        juce::String callbackUrl;
        juce::String idempotencyKey;
    };

    struct JobRecord
    {
        juce::String jobId;
        juce::String trackId;
        juce::String status = jobStatusQueued;
        int progress = 0;
        juce::String stage = "queued";
        juce::String message = "Accepted and queued for processing";
        juce::String outputKey;
        std::optional<long long> processingTimeMs;
        juce::String errorCode;
        juce::String errorMessage;
        juce::String callbackUrl;
        juce::String idempotencyKey;
        double targetLoudness = 0.0;
        juce::String updatedAt;
        juce::String createdAt;
    };

    struct StoredJob
    {
        JobRecord record;
        juce::String payloadSignature;
    };

    struct IdempotencyEntry
    {
        juce::String jobId;
        juce::String payloadSignature;
    };

    struct SubmitOutcome
    {
        enum class Kind { accepted, duplicate, conflict };
        Kind kind = Kind::accepted;
        JobRecord record;
        juce::String errorMessage;
    };

    static juce::String currentIsoTimestamp()
    {
        return juce::Time::getCurrentTime().formatted ("%Y-%m-%dT%H:%M:%SZ");
    }

    static crow::response makeErrorResponse (int statusCode, const juce::String& message)
    {
        crow::json::wvalue response;
        response["error"] = message.toStdString();
        return crow::response (statusCode, response);
    }

    static crow::json::wvalue makeJobResponse (const JobRecord& job)
    {
        crow::json::wvalue response;
        response["jobId"] = job.jobId.toStdString();
        response["trackId"] = job.trackId.toStdString();
        response["status"] = job.status.toStdString();
        response["progress"] = job.progress;
        response["stage"] = job.stage.toStdString();
        response["message"] = job.message.toStdString();
        response["outputKey"] = job.outputKey.isNotEmpty() ? job.outputKey.toStdString() : crow::json::wvalue();
        if (job.processingTimeMs.has_value())
            response["processingTimeMs"] = (std::int64_t) *job.processingTimeMs;
        else
            response["processingTimeMs"] = crow::json::wvalue();
        response["updatedAt"] = job.updatedAt.toStdString();
        return response;
    }

    static juce::String buildPayloadSignature (const SubmitRequest& request)
    {
        return request.trackId.trim() + "\n"
            + request.inputUrl.trim() + "\n"
            + request.outputUrl.trim() + "\n"
            + juce::String (request.targetLoudness, 6) + "\n"
            + request.callbackUrl.trim();
    }

    // 💥 NEW: Helper to send webhooks safely on background threads without blocking DSP
    static void fireWebhookNotification (const juce::String& urlString, const crow::json::wvalue& bodyPayload)
    {
        const juce::URL url (urlString);
        if (! url.isWellFormed()) return;

        crow::json::wvalue root = bodyPayload;
        juce::MemoryBlock postData;
        postData.append (root.dump().data(), root.dump().size());

        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd ("POST")
            .withExtraHeaders ("Content-Type: application/json\n")
            .withConnectionTimeoutMs (5000); // Fast timeout for status notifications

        // Fire and forget asynchronously via thread pooling allocation background contexts
        std::thread ([url, postData, options]() {
            auto uploadUrl = url.withPOSTData (postData);
            if (auto stream = uploadUrl.createInputStream (options))
            {
                // Webhook delivered successfully
                juce::Logger::writeToLog ("[Webhook] Status dispatched smoothly");
            }
        }).detach();
    }

    static std::optional<SubmitRequest> parseSubmitRequest (const crow::request& req, juce::String& errorMessage)
    {
        const auto root = juce::JSON::parse (req.body);
        auto* object = root.getDynamicObject();

        if (object == nullptr)
        {
            errorMessage = "Invalid JSON payload";
            return std::nullopt;
        }

        auto readRequiredString = [&] (const char* name) -> std::optional<juce::String>
        {
            if (! object->hasProperty (name))
            {
                errorMessage = juce::String ("Missing required field: ") + name;
                return std::nullopt;
            }
            return object->getProperty (name).toString().trim();
        };

        SubmitRequest request;
        
        auto trackId = readRequiredString ("trackId"); if (!trackId) return std::nullopt; request.trackId = *trackId;
        auto inputUrl = readRequiredString ("inputUrl"); if (!inputUrl) return std::nullopt; request.inputUrl = *inputUrl;
        auto outputUrl = readRequiredString ("outputUrl"); if (!outputUrl) return std::nullopt; request.outputUrl = *outputUrl;
        auto outputKey = readRequiredString ("outputKey"); if (!outputKey) return std::nullopt; request.outputKey = *outputKey;
        auto callbackUrl = readRequiredString ("callbackUrl"); if (!callbackUrl) return std::nullopt; request.callbackUrl = *callbackUrl;
        auto idempotencyKey = readRequiredString ("idempotencyKey"); if (!idempotencyKey) return std::nullopt; request.idempotencyKey = *idempotencyKey;

        if (! object->hasProperty ("targetLoudness"))
        {
            errorMessage = "Missing targetLoudness";
            return std::nullopt;
        }
        request.targetLoudness = object->getProperty ("targetLoudness").toString().getDoubleValue();

        return request;
    }

    class JobStore
    {
    public:
        SubmitOutcome submit (const SubmitRequest& request)
        {
            const auto signature = buildPayloadSignature (request);
            const auto idempotencyKey = request.idempotencyKey.trim().toStdString();

            std::unique_lock lock (mutex);

            if (auto it = idempotencyIndex.find (idempotencyKey); it != idempotencyIndex.end())
            {
                if (it->second.payloadSignature != signature)
                {
                    return { SubmitOutcome::Kind::conflict, {}, "idempotencyKey reuses a different payload" };
                }
                auto jobIt = jobs.find (it->second.jobId.toStdString());
                return { SubmitOutcome::Kind::duplicate, jobIt->second.record, {} };
            }

            JobRecord record;
            record.jobId = juce::Uuid().toString();
            record.trackId = request.trackId.trim();
            record.status = jobStatusQueued;
            record.outputKey = request.outputKey.trim();
            record.callbackUrl = request.callbackUrl.trim();
            record.idempotencyKey = request.idempotencyKey.trim();
            record.targetLoudness = request.targetLoudness;
            record.updatedAt = currentIsoTimestamp();
            record.createdAt = record.updatedAt;

            jobs.emplace (record.jobId.toStdString(), StoredJob { record, signature });
            idempotencyIndex.emplace (idempotencyKey, IdempotencyEntry { record.jobId, signature });

            return { SubmitOutcome::Kind::accepted, record, {} };
        }

        bool updateAndNotify (const juce::String& jobId, const std::function<void (JobRecord&)>& updater)
        {
            JobRecord updatedRecord;
            {
                std::unique_lock lock (mutex);
                auto it = jobs.find (jobId.toStdString());
                if (it == jobs.end()) return false;

                updater (it->second.record);
                it->second.record.updatedAt = currentIsoTimestamp();
                updatedRecord = it->second.record;
            }

            // 💥 NEW: Automatically notify NestJS webhook on every functional state jump
            crow::json::wvalue webhookBody;
            webhookBody["jobId"] = updatedRecord.jobId.toStdString();
            webhookBody["trackId"] = updatedRecord.trackId.toStdString();
            webhookBody["status"] = updatedRecord.status.toStdString();
            webhookBody["progress"] = updatedRecord.progress;
            webhookBody["stage"] = updatedRecord.stage.toStdString();
            webhookBody["message"] = updatedRecord.message.toStdString();
            
            if (updatedRecord.status == jobStatusCompleted) {
                webhookBody["processedKey"] = updatedRecord.outputKey.toStdString();
            } else {
                webhookBody["processedKey"] = crow::json::wvalue();
            }

            if (updatedRecord.errorCode.isNotEmpty()) {
                webhookBody["errorCode"] = updatedRecord.errorCode.toStdString();
            } else {
                webhookBody["errorCode"] = crow::json::wvalue();
            }

            if (updatedRecord.processingTimeMs.has_value()) {
                webhookBody["processingTimeMs"] = *updatedRecord.processingTimeMs;
            } else {
                webhookBody["processingTimeMs"] = crow::json::wvalue();
            }

            fireWebhookNotification (updatedRecord.callbackUrl, webhookBody);
            return true;
        }

        std::optional<JobRecord> getJob (const juce::String& jobId) const
        {
            std::shared_lock lock (mutex);
            auto it = jobs.find (jobId.toStdString());
            if (it == jobs.end()) return std::nullopt;
            return it->second.record;
        }

    private:
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, StoredJob> jobs;
        std::unordered_map<std::string, IdempotencyEntry> idempotencyIndex;
    };

    struct ServerState
    {
        ServerState() : threadPool (std::max(1u, std::thread::hardware_concurrency())) {}
        JobStore jobs;
        juce::ThreadPool threadPool;
    };

    static ServerState& getServerState() { static ServerState state; return state; }

    class MasterJob final : public juce::ThreadPoolJob
    {
    public:
        MasterJob (JobStore& storeToUse, SubmitRequest requestToUse, juce::String jobIdToUse)
            : juce::ThreadPoolJob ("master-job-" + jobIdToUse), store (storeToUse), request (std::move (requestToUse)), jobId (std::move (jobIdToUse)) {}

        JobStatus runJob() override
        {
            const auto startedAt = juce::Time::getMillisecondCounterHiRes();

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
                });
                return jobHasFinished;
            };

            store.updateAndNotify (jobId, [] (JobRecord& job) {
                job.status = jobStatusProcessing; job.progress = 5; job.stage = "fetch"; job.message = "Downloading input asset via presigned gateway";
            });

            // 💥 FIX: Download using the secure presigned input URL directly
            juce::MemoryBlock inputData;
            const juce::URL inputUrl (request.inputUrl);
            auto downloadOptions = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (60000);

            {
                int statusCode = 0;
                auto stream = inputUrl.createInputStream (downloadOptions.withStatusCode (&statusCode));
                if (stream == nullptr)
                    return fail ("fetch", "R2_DOWNLOAD_FAILED", "Failed to pull raw asset from presigned address", 5);

                stream->readIntoMemoryBlock (inputData);
                if (inputData.getSize() == 0)
                    return fail ("fetch", "R2_DOWNLOAD_FAILED", "Downloaded input object is empty", 5);
            }

            store.updateAndNotify (jobId, [] (JobRecord& job) {
                job.progress = 25; job.stage = "analysis"; job.message = "Analyzing transient vectors";
            });

            juce::AudioFormatManager formatManager;
            formatManager.registerBasicFormats();
            
            auto inputStream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (std::move (inputStream)));
            
            if (reader == nullptr) return fail ("analysis", "AUDIO_READ_FAILED", "Unable to open input format channels", 25);

            const auto analysis = ceilingIO::analyseReader (*reader, 2);
            const float renderGainDb = static_cast<float> (request.targetLoudness) - analysis.lufsDb;

            MainAudioProcessor processor;
            ceilingIO::applyAnalysisToProcessor (processor, analysis, nullptr, renderGainDb);

            store.updateAndNotify (jobId, [] (JobRecord& job) {
                job.progress = 60; job.stage = "render"; job.message = "Executing high-performance C++ DSP mastering matrices";
            });

            // Re-read for streaming pass render execution
            auto freshStream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
            std::unique_ptr<juce::AudioFormatReader> renderReader (formatManager.createReaderFor (std::move (freshStream)));

            juce::MemoryBlock outputData;
            juce::String dspError;
            if (! ceilingIO::renderReaderToMemory (*renderReader, processor, outputData, dspError))
                return fail ("render", "DSP_RENDER_FAILED", dspError.isNotEmpty() ? dspError : "DSP math pipeline failure", 60);

            store.updateAndNotify (jobId, [] (JobRecord& job) {
                job.progress = 85; job.stage = "upload"; job.message = "Uploading complete master up to storage clusters";
            });

            // 💥 FIX: Upload output securely straight into the pre-authorized PUT link issued by NestJS
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

            const auto elapsed = static_cast<long long>(juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startedAt));
            store.updateAndNotify (jobId, [&] (JobRecord& job) {
                job.status = jobStatusCompleted; job.progress = 100; job.stage = "render"; job.message = "Mastering completed successfully"; job.processingTimeMs = elapsed;
            });

            return jobHasFinished;
        }

    private:
        JobStore& store;
        SubmitRequest request;
        juce::String jobId;
    };
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    int port = 8080;
    if (argc > 1) port = juce::String (argv[1]).getIntValue();

    auto& serverState = getServerState();
    crow::SimpleApp app;

    CROW_ROUTE (app, "/api/v1/master/jobs").methods (crow::HTTPMethod::Post)
    ([&] (const crow::request& req) {
        try {
            juce::String errorMsg;
            auto request = parseSubmitRequest (req, errorMsg);
            if (! request.has_value()) return makeErrorResponse (400, errorMsg);

            auto outcome = serverState.jobs.submit (*request);
            if (outcome.kind == SubmitOutcome::Kind::conflict) return makeErrorResponse (409, outcome.errorMessage);

            if (outcome.kind == SubmitOutcome::Kind::accepted) {
                auto* job = new MasterJob (serverState.jobs, *request, outcome.record.jobId);
                serverState.threadPool.addJob (job, true);
            }

            crow::json::wvalue response;
            response["accepted"] = true;
            response["jobId"] = outcome.record.jobId.toStdString();
            response["status"] = outcome.record.status.toStdString();
            response["statusUrl"] = "/api/v1/master/jobs/" + outcome.record.jobId.toStdString();
            return crow::response (202, response);
        } catch (const std::exception& e) {
            return makeErrorResponse (500, juce::String ("Internal Error: ") + e.what());
        }
    });

    CROW_ROUTE (app, "/api/v1/master/jobs/<string>").methods (crow::HTTPMethod::Get)
    ([&] (const crow::request& req, const std::string& jobId) {
        try {
            auto job = serverState.jobs.getJob (juce::String (jobId));
            if (! job.has_value()) return makeErrorResponse (404, juce::String ("Job record not located"));
            return crow::response (200, makeJobResponse (*job));
        } catch (const std::exception& e) {
            return makeErrorResponse (500, juce::String ("Internal Error: ") + e.what());
        }
    });

    CROW_ROUTE (app, "/health").methods (crow::HTTPMethod::Get)
    ([]() {
        crow::json::wvalue response;
        response["status"] = "ok";
        response["timestamp"] = currentIsoTimestamp().toStdString();
        return crow::response (200, response);
    });

    app.port (port).multithreaded().run();
    return 0;
}