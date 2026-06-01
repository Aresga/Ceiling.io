#include <JuceHeader.h>
#include <crow.h>

#include "JobModels.h"
#include "WebhookDispatcher.h"
#include "JobStore.h"
#include "MasterJob.h"


namespace server
{
    struct ServerContext
    {
        ServerContext() 
            : store (dispatcher),
              threadPool (std::max (1u, std::thread::hardware_concurrency())) 
        {}

        WebhookDispatcher dispatcher;
        JobStore store;
        juce::ThreadPool threadPool;
    };

    // Helper to format a single job record into a standard Crow JSON response
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
            response["processingTimeMs"] = static_cast<std::int64_t> (*job.processingTimeMs);
        else
            response["processingTimeMs"] = crow::json::wvalue();
            
        response["updatedAt"] = job.updatedAt.toStdString();
        return response;
    }

    // Standard error formatter
    static crow::response makeErrorResponse (int statusCode, const juce::String& message)
    {
        crow::json::wvalue response;
        response["error"] = message.toStdString();
        return crow::response (statusCode, response);
    }
}

int main (int argc, char* argv[])
{
    // Required to safely use JUCE string, memory, and network functions
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    auto logFile = juce::File::getCurrentWorkingDirectory().getChildFile ("ceilingIO_server.log");

    // unique_ptr keeps the logger object alive safely until main exits
    auto fileLogger = std::make_unique<juce::FileLogger> (logFile, "=== ceilingIO Server Session Started ===");
    juce::Logger::setCurrentLogger (fileLogger.get());
    
    juce::Logger::writeToLog ("Server log file initialized at: " + logFile.getFullPathName());
    
    int port = 8080;
    if (argc > 1) 
        port = juce::String (argv[1]).getIntValue();

    server::ServerContext context;
    crow::SimpleApp app;

    // POST: Submit a mastering job
    CROW_ROUTE (app, "/api/v1/master/jobs").methods (crow::HTTPMethod::Post)
    ([&] (const crow::request& req) {
        try {
            juce::String errorMsg;
            auto request = server::parseSubmitRequest (req, errorMsg);
            if (! request.has_value()) 
                return server::makeErrorResponse (400, errorMsg);

            auto outcome = context.store.submit (*request);
            if (outcome.kind == server::SubmitOutcome::Kind::conflict) 
                return server::makeErrorResponse (409, outcome.errorMessage);

            // If it's a completely new unique job request, drop it into the worker pool
            if (outcome.kind == server::SubmitOutcome::Kind::accepted) {
                auto* job = new server::MasterJob (context.store, *request, outcome.record.jobId);
                context.threadPool.addJob (job, true); // true means thread pool manages cleanup memory
            }

            crow::json::wvalue response;
            response["accepted"] = true;
            response["jobId"] = outcome.record.jobId.toStdString();
            response["status"] = outcome.record.status.toStdString();
            response["statusUrl"] = "/api/v1/master/jobs/" + outcome.record.jobId.toStdString();
            return crow::response (202, response);
        } 
        catch (const std::exception& e) {
            return server::makeErrorResponse (500, juce::String ("Internal Server Error: ") + e.what());
        }
    });

    // GET: Query status of a specific job
    CROW_ROUTE (app, "/api/v1/master/jobs/<string>").methods (crow::HTTPMethod::Get)
    ([&] (const crow::request& req, const std::string& jobId) {
        try {
            auto job = context.store.getJob (juce::String (jobId));
            if (! job.has_value()) 
                return server::makeErrorResponse (404, "Job record not located");
                
            return crow::response (200, server::makeJobResponse (*job));
        } 
        catch (const std::exception& e) {
            return server::makeErrorResponse (500, juce::String ("Internal Server Error: ") + e.what());
        }
    });

    // GET: Liveness / Health check probe
    CROW_ROUTE (app, "/health").methods (crow::HTTPMethod::Get)
    ([]() {
        crow::json::wvalue response;
        response["status"] = "ok";
        response["timestamp"] = server::currentIsoTimestamp().toStdString();
        return crow::response (200, response);
    });

    // Start the Crow application runner loop
    app.port (port).multithreaded().run();
    return 0;
}
