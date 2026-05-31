#include "AnalysisWorker.h"

void AnalysisWorker::analyzeFile (const juce::File& audioFile, int intensity, std::function<void()> onComplete)
{
    // Launch background thread to avoid blocking caller/UI.
    if (isRunning.load())
        return; // already running

    isRunning.store (true);

    std::thread([this, audioFile, intensity, onComplete]() {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));
        if (! reader)
        {
            isRunning.store (false);
            if (onComplete)
                juce::MessageManager::callAsync (onComplete);
            return; // failed to open
        }

        const int numChannels = (int) reader->numChannels;
        const juce::int64 totalSamples = reader->lengthInSamples;
        const int blockSize = 4096;

        juce::AudioBuffer<float> buffer (numChannels, blockSize);

        juce::int64 samplesRead = 0;
        double sumSquares = 0.0;
        float peak = 0.0f;

        while (samplesRead < totalSamples)
        {
            int samplesThisIter = (int) std::min<juce::int64> (blockSize, totalSamples - samplesRead);
            reader->read (&buffer, 0, samplesThisIter, samplesRead, true, true);

            for (int c = 0; c < numChannels; ++c)
            {
                const float* data = buffer.getReadPointer (c);
                for (int i = 0; i < samplesThisIter; ++i)
                {
                    float v = data[i];
                    sumSquares += (double) v * (double) v;
                    float av = std::abs (v);
                    if (av > peak) peak = av;
                }
            }

            samplesRead += samplesThisIter;
        }

        const double meanSquare = sumSquares / (double) (std::max<juce::int64> (1, totalSamples) * std::max (1, numChannels));
        const double rms = std::sqrt (meanSquare);

        const float rmsDb = juce::Decibels::gainToDecibels ((float) rms, -120.0f);
        const float peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);

        // Heuristic suggestions dependent on intensity:
        // intensity 1 (Low)  -> conservative: smaller drive, milder threshold
        // intensity 2 (Med)  -> medium defaults
        // intensity 3 (High) -> aggressive: higher drive, lower threshold
        int clampedIntensity = juce::jlimit (1, 3, intensity);

        float thresholdOffset = 0.0f;
        float driveOffset = 0.0f;
        if (clampedIntensity == 1) { thresholdOffset = 1.0f; driveOffset = 0.0f; }
        else if (clampedIntensity == 2) { thresholdOffset = 3.0f; driveOffset = 0.5f; }
        else { thresholdOffset = 6.0f; driveOffset = 1.5f; }

        float suggestedCompTh = rmsDb - thresholdOffset;
        suggestedCompTh = juce::jlimit (-60.0f, 0.0f, suggestedCompTh);

        float suggestedDrive = std::max (0.0f, -1.0f - peakDb + driveOffset);
        suggestedDrive = juce::jlimit (0.0f, 12.0f, suggestedDrive);

        lastRmsDb.store (rmsDb);
        lastPeakDb.store (peakDb);
        suggestedThresholdDb.store (suggestedCompTh);
        suggestedDriveDb.store (suggestedDrive);

        isRunning.store (false);
        if (onComplete)
            juce::MessageManager::callAsync (onComplete);
    }).detach();
}
