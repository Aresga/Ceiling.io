#pragma once

#include "JobModels.h"
#include "WebhookDispatcher.h"
#include <shared_mutex>
#include <unordered_map>
#include <functional>

namespace server
{
    class JobStore
    {
    public:
        // Pass the webhook dispatcher so the store can send updates automatically
        JobStore (WebhookDispatcher& dispatcher);
        ~JobStore() = default;

        // Validates and saves a new job, guarding against duplicates
        SubmitOutcome submit (const SubmitRequest& request);

        // Safely updates a job's progress and fires a webhook notice
        bool updateAndNotify (const juce::String& jobId, const std::function<void (JobRecord&)>& updater);

        // Safely fetches a copy of a job record
        std::optional<JobRecord> getJob (const juce::String& jobId) const;

    private:
        WebhookDispatcher& webhookDispatcher;
        
        // Safety lock to prevent threads from corrupting data at the same time
        mutable std::shared_mutex mutex;
        
        std::unordered_map<std::string, StoredJob> jobs;
        std::unordered_map<std::string, IdempotencyEntry> idempotencyIndex;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JobStore)
    };
}
