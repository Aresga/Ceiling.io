#include <JuceHeader.h>
#include "MainAudioProcessor.h"

// Simple vertical VU meter component. UI thread receives RMS dB values (atomics)
class VUMeter  : public juce::Component
{
public:
    VUMeter() { setOpaque(true); }
    void setLevelDb (float db) noexcept { levelDb = db; repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        auto r = getLocalBounds().toFloat().reduced (4.0f);

        // map dB (-60..0) to 0..1
        float norm = juce::jlimit (0.0f, 1.0f, (levelDb + 60.0f) / 60.0f);
        juce::Colour fill = juce::Colour::fromHSV (norm * 0.3f, 0.9f, 0.9f, 1.0f);
        g.setColour (juce::Colours::darkgrey);
        g.fillRect (r);
        g.setColour (fill);
        g.fillRect (r.withTrimmedTop ((1.0f - norm) * r.getHeight()));
    }

private:
    float levelDb = -120.0f;
};


class MainComponent  : public juce::Component,
                       private juce::Button::Listener,
                       private juce::Timer
{
public:
    MainComponent()
    : importButton ("Import WAV"), applyButton ("Apply Suggestions")
    {
        addAndMakeVisible (importButton); importButton.addListener (this);
        addAndMakeVisible (applyButton);  applyButton.addListener (this);

        addAndMakeVisible (intensitySlider);
        intensitySlider.setRange (1, 3, 1);
        intensitySlider.setValue (2);
        intensitySlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

        addAndMakeVisible (toneBox);
        toneBox.addItem ("Bright", 1);
        toneBox.addItem ("Balanced", 2);
        toneBox.addItem ("Warm", 3);
        toneBox.setSelectedId (2);
        toneBox.onChange = [this]() { applyTonePreset(); };

        addAndMakeVisible (autoApplyToggle);
        autoApplyToggle.setButtonText ("Auto Apply");

        addAndMakeVisible (rmsLabel);
        addAndMakeVisible (peakLabel);
        addAndMakeVisible (suggestedThresholdLabel);
        addAndMakeVisible (suggestedDriveLabel);

        addAndMakeVisible (inputMeter);
        addAndMakeVisible (outputMeter);

        rmsLabel.setText ("RMS: -- dB", juce::dontSendNotification);
        peakLabel.setText ("Peak: -- dB", juce::dontSendNotification);
        suggestedThresholdLabel.setText ("Suggested Threshold: -- dB", juce::dontSendNotification);
        suggestedDriveLabel.setText ("Suggested Drive: -- dB", juce::dontSendNotification);

        setSize (600, 240);

        // Initialize audio device manager and player
        deviceManager.initialise (0, 2, nullptr, true);
        player.setProcessor (&processor);
        deviceManager.addAudioCallback (&player);

        startTimerHz (20); // UI refresh + meters
    }

    ~MainComponent() override
    {
        stopTimer();
        deviceManager.removeAudioCallback (&player);
        player.setProcessor (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::darkslategrey);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (32);
        importButton.setBounds (top.removeFromLeft (120));
        applyButton.setBounds (top.removeFromLeft (140).reduced (8, 4));
        autoApplyToggle.setBounds (top.removeFromLeft (120).reduced (8, 4));

        auto left = r.removeFromLeft (160);
        intensitySlider.setBounds (left.removeFromTop (24).withTrimmedLeft (4));
        toneBox.setBounds (left.removeFromTop (28).withTrimmedLeft (4));

        auto meters = r.removeFromLeft (120);
        inputMeter.setBounds (meters.removeFromLeft (meters.getWidth() / 2).reduced (8));
        outputMeter.setBounds (meters.reduced (8));

        auto labels = r;
        rmsLabel.setBounds (labels.removeFromTop (22));
        peakLabel.setBounds (labels.removeFromTop (22));
        suggestedThresholdLabel.setBounds (labels.removeFromTop (22));
        suggestedDriveLabel.setBounds (labels.removeFromTop (22));
    }

private:
    void buttonClicked (juce::Button* b) override
    {
        if (b == &importButton)
        {
            juce::FileChooser chooser ("Select a WAV file to analyze", juce::File(), "*.wav");
            if (chooser.browseForFileToOpen())
            {
                auto f = chooser.getResult();
                int intensity = (int) intensitySlider.getValue();
                bool autoApply = autoApplyToggle.getToggleState();
                processor.startFileAnalysis (f, intensity, autoApply);
            }
        }
        else if (b == &applyButton)
        {
            processor.applyAnalysisSuggestions();
        }
    }

    void timerCallback() override
    {
        float rms = processor.getAnalysisLastRmsDb();
        float peak = processor.getAnalysisLastPeakDb();
        float thr = processor.getAnalysisSuggestedThresholdDb();
        float drv = processor.getAnalysisSuggestedDriveDb();

        rmsLabel.setText ("RMS: " + juce::String (rms, 2) + " dB", juce::dontSendNotification);
        peakLabel.setText ("Peak: " + juce::String (peak, 2) + " dB", juce::dontSendNotification);
        suggestedThresholdLabel.setText ("Suggested Threshold: " + juce::String (thr, 2) + " dB", juce::dontSendNotification);
        suggestedDriveLabel.setText ("Suggested Drive: " + juce::String (drv, 2) + " dB", juce::dontSendNotification);

        // update meters from atomics
        inputMeter.setLevelDb (processor.getInputRmsDb());
        outputMeter.setLevelDb (processor.getOutputRmsDb());
    }

    void applyTonePreset()
    {
        int sel = toneBox.getSelectedId();
        float lo = 0.0f, mid = 0.0f, hi = 0.0f;
        if (sel == 1) // Bright
        {
            lo = -1.5f; mid = 0.0f; hi = 2.5f;
        }
        else if (sel == 2) // Balanced
        {
            lo = 0.0f; mid = 0.0f; hi = 0.0f;
        }
        else // Warm
        {
            lo = 1.5f; mid = 0.5f; hi = -1.5f;
        }

        if (auto pLo = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_LO_GAIN")))
            pLo->setValueNotifyingHost (pLo->convertTo0to1 (lo));
        if (auto pMid = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_MID_GAIN")))
            pMid->setValueNotifyingHost (pMid->convertTo0to1 (mid));
        if (auto pHi = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_HI_GAIN")))
            pHi->setValueNotifyingHost (pHi->convertTo0to1 (hi));
    }

    juce::TextButton importButton, applyButton;
    juce::Slider intensitySlider;
    juce::ComboBox toneBox;
    juce::ToggleButton autoApplyToggle;
    juce::Label rmsLabel, peakLabel, suggestedThresholdLabel, suggestedDriveLabel;
    VUMeter inputMeter, outputMeter;

    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;

    MainAudioProcessor processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};


class ceilingIOApplication  : public juce::JUCEApplication
{
public:
    ceilingIOApplication() {}

    const juce::String getApplicationName() override       { return "ceilingIO"; }
    const juce::String getApplicationVersion() override    { return "0.1"; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new juce::DocumentWindow (getApplicationName(), juce::Colours::lightgrey,
                                                     juce::DocumentWindow::allButtons));
        mainWindow->setUsingNativeTitleBar (true);
        mainWindow->setContentOwned (new MainComponent(), true);
        mainWindow->centreWithSize (600, 240);
        mainWindow->setVisible (true);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<juce::DocumentWindow> mainWindow;
};

START_JUCE_APPLICATION (ceilingIOApplication)
