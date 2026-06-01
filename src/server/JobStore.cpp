#include "JobStore.h"

namespace server
{
    JobStore::JobStore (WebhookDispatcher& dispatcher)
        : webhookDispatcher (dispatcher)
    {}

    SubmitOutcome JobStore::submit (const SubmitRequest& request)
    {
        const auto signature = buildPayloadSignature (request);
        const auto idempotencyKey = request.idempotencyKey.trim().toStdString();

        // Exclusive lock: completely blocks other threads while writing data
        std::unique_lock lock (mutex);

        // Check if we have seen this identity key before
        if (auto it = idempotencyIndex.find (idempotencyKey); it != idempotencyIndex.end())
        {
            if (it->second.payloadSignature != signature)
            {
                return { SubmitOutcome::Kind::conflict, {}, "idempotencyKey reuses a different payload" };
            }
            auto jobIt = jobs.find (it->second.jobId.toStdString());
            return { SubmitOutcome::Kind::duplicate, jobIt->second.record, {} };
        }

        // Setup a fresh job record
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

    bool JobStore::updateAndNotify (const juce::String& jobId, const std::function<void (JobRecord&)>& updater)
    {
        JobRecord updatedRecord;
        {
            std::unique_lock lock (mutex);
            auto it = jobs.find (jobId.toStdString());
            if (it == jobs.end()) return false;

            // Apply updates
            updater (it->second.record);
            it->second.record.updatedAt = currentIsoTimestamp();
            updatedRecord = it->second.record;
        }

        // Build JSON webhook payload structure
        crow::json::wvalue webhookBody;
        webhookBody["jobId"] = updatedRecord.jobId.toStdString();
        webhookBody["trackId"] = updatedRecord.trackId.toStdString();
        webhookBody["status"] = updatedRecord.status.toStdString();
        webhookBody["progress"] = updatedRecord.progress;
        webhookBody["stage"] = updatedRecord.stage.toStdString();
        webhookBody["message"] = updatedRecord.message.toStdString();

        if (updatedRecord.progress >= 60)
        {
            webhookBody["inputLufs"] = updatedRecord.inputLufs;
            webhookBody["inputRms"] = updatedRecord.inputRms;
            webhookBody["inputPeak"] = updatedRecord.inputPeak;
            webhookBody["suggestedGainDb"] = updatedRecord.suggestedGainDb;
        }
        
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

        // Send out payload through the background queue
        webhookDispatcher.queueWebhook (updatedRecord.callbackUrl, webhookBody);
        return true;
    }

    std::optional<JobRecord> JobStore::getJob (const juce::String& jobId) const
    {
        // Shared lock: allows multiple threads to read data simultaneously safely
        std::shared_lock lock (mutex);
        
        auto it = jobs.find (jobId.toStdString());
        if (it == jobs.end()) return std::nullopt;
        
        return it->second.record;
    }
}
