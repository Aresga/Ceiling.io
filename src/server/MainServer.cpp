#include <JuceHeader.h>
#include "ceilingIOPipeline.h"

#include <optional>
#include <sstream>
#include <thread>

#include <crow.h>

// AWS SDK removed: server uses presigned HTTP(S) URLs only.

namespace
{
    struct MasterRequest
    {
        juce::String inputUrl;
        juce::String outputUrl;
        juce::String presetName;
    };

    // Presigned URL flow: treat URLs as opaque HTTP(S) endpoints (no bucket parsing).
    static std::optional<MasterRequest> parseMasterRequest (const crow::request& req, juce::String& errorMessage)
    {
        auto payload = crow::json::load (req.body);
        if (! payload)
        {
            errorMessage = "Invalid JSON payload";
            return std::nullopt;
        }

        MasterRequest request;
        request.inputUrl = juce::String (static_cast<std::string> (payload["input_url"].s()));
        request.outputUrl = juce::String (static_cast<std::string> (payload["output_url"].s()));
        request.presetName = juce::String (static_cast<std::string> (payload["preset"].s()));

        if (request.inputUrl.isEmpty() || request.outputUrl.isEmpty() || request.presetName.isEmpty())
        {
            errorMessage = "Missing input_url, output_url, or preset";
            return std::nullopt;
        }

        return request;
    }

    // No URL-to-bucket parsing needed when using presigned URLs.
    static bool downloadWithJuceUrl (const juce::String& urlText, juce::MemoryBlock& outputData, juce::String& errorMessage)
    {
        juce::URL url (urlText);
        if (! url.isWellFormed())
        {
            errorMessage = "Invalid input URL";
            return false;
        }

        outputData.reset();
        if (! url.readEntireBinaryStream (outputData, false))
        {
            errorMessage = "Failed to download input URL";
            return false;
        }

        return outputData.getSize() > 0;
    }

    static bool uploadWithJuceUrl (const juce::String& urlText, const juce::MemoryBlock& payload, juce::String& errorMessage)
    {
        juce::URL url (urlText);
        if (! url.isWellFormed())
        {
            errorMessage = "Invalid output URL";
            return false;
        }

        const auto uploadUrl = url.withPOSTData (payload);
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd ("PUT")
            .withConnectionTimeoutMs (30000)
            .withStatusCode (&statusCode);

        auto response = uploadUrl.createInputStream (options);
        if (response == nullptr)
        {
            errorMessage = "Failed to upload rendered audio";
            return false;
        }

        juce::MemoryBlock sink;
        response->readIntoMemoryBlock (sink);
        return statusCode == 0 || (statusCode >= 200 && statusCode < 300);
    }

    static bool downloadInput (const juce::String& urlText, juce::MemoryBlock& outputData, juce::String& errorMessage)
    {
        return downloadWithJuceUrl (urlText, outputData, errorMessage);
    }

    static bool uploadOutput (const juce::String& urlText, const juce::MemoryBlock& payload, juce::String& errorMessage)
    {
        return uploadWithJuceUrl (urlText, payload, errorMessage);
    }

    static void processMasterJob (MasterRequest request)
    {
        juce::MemoryBlock inputData;
        juce::String errorMessage;

        if (! downloadInput (request.inputUrl, inputData, errorMessage))
        {
            juce::Logger::writeToLog ("[ceilingIOServer] " + errorMessage);
            return;
        }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        auto makeReader = [&]() -> std::unique_ptr<juce::AudioFormatReader>
        {
            auto inputStream = std::make_unique<juce::MemoryInputStream> (inputData.getData(), inputData.getSize(), false);
            return std::unique_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (std::move (inputStream)));
        };

        auto analysisReader = makeReader();
        if (analysisReader == nullptr)
        {
            juce::Logger::writeToLog ("[ceilingIOServer] Unable to open input audio for analysis");
            return;
        }

        const auto* preset = ceilingIO::findPreset (request.presetName);
        if (preset == nullptr)
        {
            juce::Logger::writeToLog ("[ceilingIOServer] Unknown preset: " + request.presetName);
            return;
        }

        const auto analysis = ceilingIO::analyseReader (*analysisReader, 2);
        const float normalizationGainDb = ceilingIO::computeNormalizationGainDb (analysis, *preset);

        MainAudioProcessor processor;
        ceilingIO::applyAnalysisToProcessor (processor, analysis, preset, normalizationGainDb);

        auto renderReader = makeReader();
        if (renderReader == nullptr)
        {
            juce::Logger::writeToLog ("[ceilingIOServer] Unable to reopen input audio for rendering");
            return;
        }

        juce::MemoryBlock outputData;
        if (! ceilingIO::renderReaderToMemory (*renderReader, processor, outputData, errorMessage))
        {
            juce::Logger::writeToLog ("[ceilingIOServer] " + errorMessage);
            return;
        }

        if (! uploadOutput (request.outputUrl, outputData, errorMessage))
            juce::Logger::writeToLog ("[ceilingIOServer] " + errorMessage);

        inputData.reset();
        outputData.reset();
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    int port = 8080;
    if (argc > 1)
        port = juce::String (argv[1]).getIntValue();

    crow::SimpleApp app;

    CROW_ROUTE (app, "/api/v1/master").methods (crow::HTTPMethod::Post)
    ([&] (const crow::request& req)
    {
        juce::String errorMessage;
        auto request = parseMasterRequest (req, errorMessage);
        if (! request.has_value())
        {
            crow::json::wvalue response;
            response["error"] = errorMessage.toStdString();
            return crow::response (400, response);
        }

        std::thread ([request = std::move (*request)]() mutable
        {
            processMasterJob (std::move (request));
        }).detach();

        crow::json::wvalue response;
        response["accepted"] = true;
        return crow::response (202, response);
    });

	CROW_ROUTE (app, "/health").methods (crow::HTTPMethod::Get)
	([]()
	{
		crow::json::wvalue response;
		response["status"] = "ok";
		response["timestamp"] = juce::Time::getCurrentTime().toString (true, true).toStdString();
		response["version"] = "1.0.0";
		response["app"] = "ceiling.io server";
		return crow::response (200, response);
	});

    app.port (port).multithreaded().run();
    return 0;
}