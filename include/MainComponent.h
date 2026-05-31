#pragma once
#include <JuceHeader.h>
#include <array>
#include "MainAudioProcessor.h"

// ==============================================================================
// CUSTOM HARDWARE-INSPIRED STUDIO LOOK AND FEEL
// ==============================================================================
class StudioLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel()
    {
        // Dark theme global color definitions
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xFF16191C));
        setColour (juce::ComboBox::backgroundColourId,       juce::Colour (0xFF202529));
        setColour (juce::ComboBox::outlineColourId,          juce::Colour (0xFF2D343A));
        setColour (juce::TextButton::buttonColourId,         juce::Colour (0xFF202529));
        setColour (juce::TextButton::buttonOnColourId,       juce::Colour (0xFF00ADB5));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
        auto radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.4f;
        auto toX = bounds.getCentreX();
        auto toY = bounds.getCentreY();
        auto len = radius - 2.0f;
        
        // Calculate needle rotation angle
        auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // Draw background tracks/outer ring
        g.setColour (juce::Colour (0xFF101214));
        g.fillEllipse (toX - radius, toY - radius, radius * 2.0f, radius * 2.0f);
        
        g.setColour (juce::Colour (0xFF2D343A));
        g.drawEllipse (toX - radius, toY - radius, radius * 2.0f, radius * 2.0f, 2.0f);

        // Draw active arc neon gradient (Cyan to Lime tracker)
        juce::Path arcPath;
        arcPath.addCentredArc (toX, toY, radius, radius, 0.0f, rotaryStartAngle, angle, true);
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xFF00F2FE), toX - radius, toY,
            juce::Colour (0xFF4FACFE), toX + radius, toY, false));
        g.strokePath (arcPath, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Draw center rotating knob cap
        auto capRadius = radius * 0.75f;
        g.setColour (juce::Colour (0xFF202529));
        g.fillEllipse (toX - capRadius, toY - capRadius, capRadius * 2.0f, capRadius * 2.0f);

        // Draw indicator dot/needle indicator
        g.setColour (juce::Colour (0xFF00F2FE));
        juce::Path needle;
        needle.addRectangle (-1.5f, -capRadius, 3.0f, capRadius * 0.4f);
        g.fillPath (needle, juce::AffineTransform::rotation (angle).translated (toX, toY));
    }
};

// ==============================================================================
// LED SEGMENTED VU METER COMPONENT
// ==============================================================================
class VUMeter  : public juce::Component
{
public:
    VUMeter() { setOpaque (true); }
    void setLevelDb (float db) noexcept { levelDb = db; repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF101214));
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        
        int numSegments = 24;
        float segmentHeight = bounds.getHeight() / numSegments;
        float norm = juce::jlimit (0.0f, 1.0f, (levelDb + 60.0f) / 60.0f);
        int segmentsToFill = static_cast<int> (norm * numSegments);

        for (int i = 0; i < numSegments; ++i)
        {
            auto segmentArea = bounds.removeFromBottom (segmentHeight).reduced (1.0f, 2.0f);
            
            if (i < segmentsToFill)
            {
                // Dynamic coloration based on stack height (Cyan low, Lime mid, Amber top)
                if (i > 20)      g.setColour (juce::Colour (0xFFFF4B2B)); // Peak Alert
                else if (i > 14) g.setColour (juce::Colour (0xFF00FF87)); // Optimal Master
                else             g.setColour (juce::Colour (0xFF00F2FE)); // Low Level
            }
            else
            {
                g.setColour (juce::Colour (0xFF1A1F24)); // Off segments
            }
            g.fillRect (segmentArea);
        }
    }

private:
    float levelDb = -120.0f;
};

// ==============================================================================
// PLACEHOLDER FFT INPUT VISUALIZER
// ==============================================================================
class DummySpectrumVisualizer : public juce::Component
{
public:
    DummySpectrumVisualizer()
        : fft (fftOrder),
          window (fftSize, juce::dsp::WindowingFunction<float>::hann)
    {
        setOpaque (true);
    }

    void setSamples (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        if (numSamples <= 0 || buffer.getNumChannels() == 0)
            return;

        const float* data = buffer.getReadPointer (0, startSample);
        for (int i = 0; i < numSamples; ++i)
            pushNextSampleIntoFifo (data[i]);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF101214));
        auto bounds = getLocalBounds().toFloat();

        std::array<float, scopeSize> scopeCopy{};
        {
            const juce::SpinLock::ScopedLockType lock (scopeLock);
            if (! hasSpectrum)
                return;
            scopeCopy = scopeData;
        }

        juce::Path p;
        p.startNewSubPath (bounds.getX(), bounds.getBottom());

        const float width = bounds.getWidth();
        const float height = bounds.getHeight();
        for (int i = 0; i < scopeSize; ++i)
        {
            const float x = bounds.getX() + width * (float) i / (float) (scopeSize - 1);
            const float y = bounds.getBottom() - scopeCopy[(size_t) i] * height;
            p.lineTo (x, juce::jlimit (bounds.getY(), bounds.getBottom(), y));
        }

        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0x4000F2FE), bounds.getX(), bounds.getBottom(),
            juce::Colour (0x0500FF87), bounds.getX(), bounds.getY(), false));
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

private:
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int scopeSize = 64;

    void pushNextSampleIntoFifo (float sample) noexcept
    {
        if (fifoIndex == fftSize)
        {
            std::copy (fifo.begin(), fifo.end(), fftData.begin());
            window.multiplyWithWindowingTable (fftData.data(), fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData.data());
            updateScopeData();
            fifoIndex = 0;
        }

        fifo[(size_t) fifoIndex++] = sample;
    }

    void updateScopeData()
    {
        std::array<float, scopeSize> nextScope{};
        for (int i = 0; i < scopeSize; ++i)
        {
            const float skewed = 1.0f - std::exp (std::log (1.0f - (float) i / (float) scopeSize) * 0.2f);
            const int idx = juce::jlimit (0, fftSize / 2, (int) (skewed * (float) (fftSize / 2)));
            const float level = juce::Decibels::gainToDecibels (fftData[(size_t) idx], -120.0f);
            nextScope[(size_t) i] = juce::jlimit (0.0f, 1.0f, (level + 120.0f) / 120.0f);
        }

        const juce::SpinLock::ScopedLockType lock (scopeLock);
        scopeData = nextScope;
        hasSpectrum = true;
    }

    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<float, fftSize> fifo{};
    std::array<float, fftSize * 2> fftData{};
    std::array<float, scopeSize> scopeData{};
    int fifoIndex = 0;
    bool hasSpectrum = false;
    juce::SpinLock scopeLock;
};

// ==============================================================================
// SYSTEM AUDIO TIMING AND ROUTING TARGET
// ==============================================================================
class ProcessedAudioSource : public juce::AudioSource
{
public:
    ProcessedAudioSource (juce::AudioTransportSource& transportSource, MainAudioProcessor& proc)
        : transport (transportSource), processor (proc) {}

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        transport.prepareToPlay (samplesPerBlockExpected, sampleRate);
        processor.prepareToPlay (sampleRate, samplesPerBlockExpected);
        tempBuffer.setSize (2, samplesPerBlockExpected);
    }

    void releaseResources() override
    {
        transport.releaseResources();
        processor.releaseResources();
        tempBuffer.setSize (0, 0);
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        if (bypass.load())
        {
            transport.getNextAudioBlock (info);
            if (spectrumVisualizer != nullptr && info.buffer != nullptr)
                spectrumVisualizer->setSamples (*info.buffer, info.startSample, info.numSamples);
            return;
        }

        if (info.numSamples <= 0 || info.buffer == nullptr)
            return;

        tempBuffer.clear();
        juce::AudioSourceChannelInfo tempInfo (&tempBuffer, 0, info.numSamples);
        transport.getNextAudioBlock (tempInfo);

        juce::MidiBuffer midi;
        processor.processBlock (tempBuffer, midi);

        const int channels = std::min (info.buffer->getNumChannels(), tempBuffer.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
            info.buffer->copyFrom (ch, info.startSample, tempBuffer, ch, 0, info.numSamples);

        for (int ch = channels; ch < info.buffer->getNumChannels(); ++ch)
            info.buffer->clear (ch, info.startSample, info.numSamples);

        if (spectrumVisualizer != nullptr)
            spectrumVisualizer->setSamples (tempBuffer, 0, info.numSamples);
    }

    void setBypass (bool shouldBypass) { bypass.store (shouldBypass); }
    void setSpectrumVisualizer (DummySpectrumVisualizer* visualizer) { spectrumVisualizer = visualizer; }

private:
    juce::AudioTransportSource& transport;
    MainAudioProcessor& processor;
    juce::AudioBuffer<float> tempBuffer;
    std::atomic<bool> bypass{ false };
    DummySpectrumVisualizer* spectrumVisualizer = nullptr;
};

// ==============================================================================
// MAIN RECONFIGURED COMPONENT PLATFORM
// ==============================================================================
class MainComponent  : public juce::Component,
                       public juce::Button::Listener,
                       public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* b) override;
    void timerCallback() override;

private:
    void applyTonePreset();
    void applyProfilePreset();
    void applyIntensityFromAnalysis();
    void loadFile (const juce::File& file);
    void startExportProcessedFile (const juce::File& outFile, bool processAudio);
    void exportProcessedFile (const juce::File& outFile, bool processAudio);

    struct ProfilePreset
    {
        juce::String name;
        float targetLufs = -14.0f;
        float targetTruePeak = -1.0f;
        float eqLo = 0.0f;
        float eqMid = 0.0f;
        float eqHi = 0.0f;
        float compThreshold = -18.0f;
        float compRatio = 2.0f;
        float compAttack = 10.0f;
        float compRelease = 100.0f;
        float limiterDrive = 1.0f;
    };

    // Styling handler instance
    StudioLookAndFeel studioLookAndFeel;

    // Headings and UI Containers
    juce::Label analysisTitleLabel{ "analysis", "ANALYSIS & IMPORT" };
    juce::Label chainTitleLabel{ "chain", "MASTERING CHAIN" };
    juce::Label deliveryTitleLabel{ "delivery", "DELIVERY & PRESETS" };
    
    // Core Layout Functional interactives
    juce::TextButton importButton{ "Import WAV" }, applyButton{ "Apply Suggestions" };
    juce::TextButton playButton{ "Play" }, stopButton{ "Stop" }, exportButton{ "BUILD MASTER" };
    juce::ToggleButton listenAfterToggle{ "Listen After" };
    
    juce::Slider intensitySlider;
    juce::ComboBox toneBox;
    juce::ComboBox profileBox;
    juce::Label profileTargetLabel;
    juce::ToggleButton autoApplyToggle{ "Auto Apply" };
    
    juce::Label rmsLabel, peakLabel, suggestedThresholdLabel, suggestedDriveLabel;
    juce::Label intensityLabel, toneLabel, driveLabel;
    juce::Label fileLabel, exportStatusLabel;
    
    double exportProgress = 0.0;
    juce::ProgressBar exportProgressBar{ exportProgress };
    
    DummySpectrumVisualizer spectrumVisualizer;
    VUMeter inputMeter, outputMeter;
    juce::Label lblInputMeter{ "lin", "INPUT" }, lblOutputMeter{ "lout", "OUTPUT" };

    juce::Label integratedLufsLabel;
    juce::Label shortTermLufsLabel;
    juce::Label momentaryLufsLabel;
    juce::Label loudnessRangeLabel;
    juce::Label truePeakMaxLabel;

    juce::Label lblIntegrated;
    juce::Label lblShortTerm;
    juce::Label lblTruePeak;
    juce::Label lblMomentary;
    juce::Label lblLoudnessRange;

    juce::Rectangle<int> integratedBlock;
    juce::Rectangle<int> shortTermBlock;
    juce::Rectangle<int> momentaryBlock;
    juce::Rectangle<int> truePeakBlock;
    juce::Rectangle<int> loudnessRangeBlock;

    // Native architectural sliders hidden safely behind the clean rotary mapping
    juce::Slider eqLoSlider, eqMidSlider, eqHiSlider;
    juce::Slider compThresholdSlider, compRatioSlider, compAttackSlider, compReleaseSlider;
    juce::Slider limiterDriveSlider;

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioFormatManager formatManager;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    MainAudioProcessor processor;
    ProcessedAudioSource processedSource;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File currentFile;
    std::atomic<double> exportProgressAtomic{ 0.0 };
    std::atomic<bool> exportInProgress{ false };

    float lastSuggestedThreshold = 0.0f;
    float lastSuggestedDrive = 0.0f;

    std::vector<ProfilePreset> profiles;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqLoAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqMidAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqHiAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compRatioAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compAttackAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> compReleaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> limiterDriveAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};