#pragma once

#include <JuceHeader.h>
#include <atomic>

class AnalysisWorker
{
public:
    AnalysisWorker() = default;
    ~AnalysisWorker() = default;

    // Begins asynchronous analysis of an audio file. The analysis runs on a
    // background thread and updates internal atomics with suggestions.
    // An optional completion callback is invoked on the message thread when
    // analysis finishes. The callback must be lightweight.
    // intensity: 1=Low,2=Medium,3=High
    void analyzeFile (const juce::File& audioFile, int intensity = 2, std::function<void()> onComplete = {});

    float getSuggestedThresholdDb() const noexcept { return suggestedThresholdDb.load(); }
    float getSuggestedDriveDb() const noexcept { return suggestedDriveDb.load(); }
    float getLastRmsDb() const noexcept { return lastRmsDb.load(); }
    float getLastPeakDb() const noexcept { return lastPeakDb.load(); }

private:
    std::atomic<float> suggestedThresholdDb{ -18.0f };
    std::atomic<float> suggestedDriveDb{ 0.0f };
    std::atomic<float> lastRmsDb{ -120.0f };
    std::atomic<float> lastPeakDb{ -120.0f };
    std::atomic<bool> isRunning{ false };
};
