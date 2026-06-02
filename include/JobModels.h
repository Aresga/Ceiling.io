#pragma once

#include <JuceHeader.h>
#include <crow.h>
#include <optional>

namespace server
{
    // Job Status Constants
    inline const juce::String jobStatusQueued     = "QUEUED";
    inline const juce::String jobStatusProcessing = "PROCESSING";
    inline const juce::String jobStatusCompleted  = "COMPLETED";
    inline const juce::String jobStatusFailed     = "FAILED";
    inline const juce::String jobStatusCancelled  = "CANCELLED";

    // Maximum safe length for incoming text fields to prevent memory abuse
    constexpr size_t maxFieldLength = 2048;

    struct SubmitRequest
    {
        juce::String trackId;
        juce::String inputUrl;     
        juce::String outputUrl;    
        juce::String outputKey;    
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
        double inputLufs = 120.0;
        double inputRms = 120.0;
        double inputPeak = 120.0;
        double suggestedGainDb = 0.0;
        double outputLufs = -120.0;
        double outputRms = -120.0;
        double outputPeak = -120.0;
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

    // Helper to generate timestamps
    static inline juce::String currentIsoTimestamp()
    {
        return juce::Time::getCurrentTime().formatted ("%Y-%m-%dT%H:%M:%SZ");
    }

    // Creates a unique signature to verify if job data matches an existing key
    static inline juce::String buildPayloadSignature (const SubmitRequest& request)
    {
        return request.trackId.trim() + "\n"
            + request.inputUrl.trim() + "\n"
            + request.outputUrl.trim() + "\n"
            + juce::String (request.targetLoudness, 6) + "\n"
            + request.callbackUrl.trim();
    }

    // Parses and validates the incoming JSON safely
    static inline std::optional<SubmitRequest> parseSubmitRequest (const crow::request& req, juce::String& errorMessage)
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
            
            juce::String value = object->getProperty (name).toString().trim();
            
            // Security Check: Block missing data or suspiciously large inputs
            if (value.isEmpty())
            {
                errorMessage = juce::String ("Field cannot be empty: ") + name;
                return std::nullopt;
            }
            if (value.length() > maxFieldLength)
            {
                errorMessage = juce::String ("Field exceeds maximum safe length: ") + name;
                return std::nullopt;
            }
            
            return value;
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
}
