#include "WebhookDispatcher.h"

namespace server
{
    // A safe background task runner for a single webhook request
    class WebhookJob final : public juce::ThreadPoolJob
    {
    public:
        WebhookJob (const juce::String& url, const crow::json::wvalue& payload)
            : juce::ThreadPoolJob ("WebhookJob"), targetUrl (url), jsonPayload (payload) {}

        JobStatus runJob() override
        {
            const juce::URL url (targetUrl);
            if (! url.isWellFormed()) 
                return jobHasFinished;

            juce::MemoryBlock postData;
            std::string dumped = jsonPayload.dump();
            postData.append (dumped.data(), dumped.size());

            auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withHttpRequestCmd ("POST")
                .withExtraHeaders ("Content-Type: application/json\n")
                .withConnectionTimeoutMs (5000); // 5-second network cutoff safety

            auto uploadUrl = url.withPOSTData (postData);
            
            if (auto stream = uploadUrl.createInputStream (options))
            {
                juce::Logger::writeToLog ("[Webhook] Status dispatched successfully to: " + targetUrl);
            }
            else
            {
                juce::Logger::writeToLog ("[Webhook] Failed to connect to target url: " + targetUrl);
            }

            return jobHasFinished;
        }

    private:
        juce::String targetUrl;
        crow::json::wvalue jsonPayload;
    };

    WebhookDispatcher::WebhookDispatcher() 
        : threadPool (2) // Allocates up to 2 concurrent webhook lines max
    {}

    WebhookDispatcher::~WebhookDispatcher()
    {
        // Security/Stability: Safely stop all pending webhooks before shutting down
        threadPool.removeAllJobs (true, 5000);
    }

    void WebhookDispatcher::queueWebhook (const juce::String& url, const crow::json::wvalue& payload)
    {
        if (url.isEmpty()) return;
        
        // This transfers ownership to the ThreadPool which cleans it up automatically
        threadPool.addJob (new WebhookJob (url, payload), true);
    }
}
