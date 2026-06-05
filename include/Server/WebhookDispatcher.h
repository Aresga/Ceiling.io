#pragma once

#include <JuceHeader.h>
#include <crow.h>

namespace server
{
    class WebhookDispatcher
    {
    public:
        WebhookDispatcher();
        ~WebhookDispatcher();

        // Sends the webhook payload safely using a background thread
        void queueWebhook (const juce::String& url, const crow::json::wvalue& payload);

    private:
        juce::ThreadPool threadPool;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebhookDispatcher)
    };
}
