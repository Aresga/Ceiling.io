#include "MainComponent.h"
#include <thread>
#include <cmath>

// ==============================================================================
// EXECUTION ARCHITECTURE & LAYOUT CONSTRUCTOR
// ==============================================================================
MainComponent::MainComponent()
    : processedSource (transport, processor),
      exportProgressBar (exportProgress)
{
    // Apply our premium matte-dark LookAndFeel styling globally to this container
    setLookAndFeel (&studioLookAndFeel);

    // Modern Header Groupings
    addAndMakeVisible (analysisTitleLabel);
    addAndMakeVisible (chainTitleLabel);
    addAndMakeVisible (deliveryTitleLabel);

    // Column 1 Components: Analysis, Loading, Metrics
    addAndMakeVisible (importButton); 
    importButton.addListener (this);
    importButton.setButtonText ("Import WAV");

    addAndMakeVisible (fileLabel);
    fileLabel.setFont (juce::FontOptions (13.0f).withStyle ("Italic"));
    fileLabel.setText ("No file loaded", juce::dontSendNotification);

    addAndMakeVisible (rmsLabel);
    addAndMakeVisible (peakLabel);
    rmsLabel.setText ("Original RMS: -- dB", juce::dontSendNotification);
    peakLabel.setText ("Original Peak: -- dB", juce::dontSendNotification);

    // Mount real-time live bouncing spectrum engine components
    addAndMakeVisible (spectrumVisualizer);
    processedSource.setSpectrumVisualizer (&spectrumVisualizer);

    // Column 2 Components: Pro Audio Hardware Rotary Knobs
    addAndMakeVisible (intensitySlider);
    intensitySlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    intensitySlider.setRange (0.0, 1.0, 0.01);
    intensitySlider.setValue (0.5);
    intensitySlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    intensitySlider.onValueChange = [this]() { applyIntensityFromAnalysis(); };
    addAndMakeVisible (intensityLabel);
    intensityLabel.setText ("INTENSITY", juce::dontSendNotification);
    intensityLabel.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (toneBox);
    toneBox.addItem ("Bright", 1);
    toneBox.addItem ("Balanced", 2);
    toneBox.addItem ("Warm", 3);
    toneBox.setSelectedId (2);
    toneBox.onChange = [this]() { applyTonePreset(); };
    addAndMakeVisible (toneLabel);
    toneLabel.setText ("TONE PRESET", juce::dontSendNotification);
    toneLabel.setJustificationType (juce::Justification::centred);

    // Glue Compression - Using compThresholdSlider natively as a pro knob
    addAndMakeVisible (compThresholdSlider);
    compThresholdSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    compThresholdSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

    // Maximizer Drive - Using limiterDriveSlider natively as a pro knob
    addAndMakeVisible (limiterDriveSlider);
    limiterDriveSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    limiterDriveSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible (driveLabel);
    driveLabel.setText ("MAXIMIZER DRIVE", juce::dontSendNotification);
    driveLabel.setJustificationType (juce::Justification::centred);

    // Target Ceiling - Using eqHiSlider natively as a pro knob matching mockup
    addAndMakeVisible (eqHiSlider);
    eqHiSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    eqHiSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

    // Column 3 Components: Delivery Presets & Main Engines
    profiles = {
        { "Spotify",    -14.0f, -1.0f, 0.5f, 0.0f, 1.5f, -16.0f, 2.0f, 10.0f, 120.0f, 1.5f },
        { "Apple Music",-16.0f, -1.0f, 0.0f, 0.0f, 1.0f, -18.0f, 2.0f, 15.0f, 150.0f, 1.0f },
        { "YouTube",    -14.0f, -1.0f, 0.5f, 0.0f, 1.0f, -16.0f, 2.5f, 10.0f, 120.0f, 1.5f },
        { "SoundCloud", -14.0f, -1.0f, 0.5f, 0.0f, 1.0f, -16.0f, 2.5f, 10.0f, 120.0f, 1.5f },
        { "CD",         -10.0f, -0.3f, 0.0f, 0.0f, 0.5f, -14.0f, 3.0f, 8.0f, 100.0f, 2.5f }
    };

    addAndMakeVisible (profileBox);
    for (int i = 0; i < (int) profiles.size(); ++i)
        profileBox.addItem (profiles[(size_t) i].name, i + 1);
    profileBox.setSelectedId (1);
    profileBox.onChange = [this]() { applyProfilePreset(); };

    addAndMakeVisible (profileTargetLabel);
    profileTargetLabel.setFont (juce::FontOptions (12.0f));
    applyProfilePreset();

    addAndMakeVisible (exportButton); 
    exportButton.addListener (this);
    exportButton.setButtonText ("BUILD MASTER");

    // Playback Engine Control Triggers
    addAndMakeVisible (playButton);   playButton.addListener (this);
    addAndMakeVisible (stopButton);   stopButton.addListener (this);
    addAndMakeVisible (listenAfterToggle);
    listenAfterToggle.setToggleState (true, juce::dontSendNotification);
    processedSource.setBypass (false);
    listenAfterToggle.onClick = [this]() { processedSource.setBypass (!listenAfterToggle.getToggleState()); };

    addAndMakeVisible (autoApplyToggle);

    // Progress Reporting Setup
    addAndMakeVisible (exportStatusLabel);
    exportStatusLabel.setText ("Export Status: Idle", juce::dontSendNotification);
    addAndMakeVisible (exportProgressBar);
    exportProgressBar.setPercentageDisplay (true);

    // Segmented VU Meters Mounting Setup
    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);
    addAndMakeVisible (lblInputMeter);
    addAndMakeVisible (lblOutputMeter);
    lblInputMeter.setJustificationType (juce::Justification::centred);
    lblOutputMeter.setJustificationType (juce::Justification::centred);

    auto setupValueLabel = [this] (juce::Label& label, float size, juce::Colour colour)
    {
        addAndMakeVisible (label);
        label.setFont (juce::FontOptions (size).withStyle ("Bold"));
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, colour);
    };

    auto setupCaptionLabel = [this] (juce::Label& label, const juce::String& text)
    {
        addAndMakeVisible (label);
        label.setFont (juce::FontOptions (11.0f));
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (0xFF7A8086));
    };

    setupValueLabel (integratedLufsLabel, 28.0f, juce::Colour (0xFF00F2FE));
    setupValueLabel (shortTermLufsLabel, 22.0f, juce::Colour (0xFF00F2FE));
    setupValueLabel (momentaryLufsLabel, 22.0f, juce::Colour (0xFF00F2FE));
    setupValueLabel (truePeakMaxLabel, 22.0f, juce::Colour (0xFFE7ECEF));
    setupValueLabel (loudnessRangeLabel, 22.0f, juce::Colour (0xFFBFC7CF));

    integratedLufsLabel.setText ("--.- LUFS", juce::dontSendNotification);
    shortTermLufsLabel.setText ("--.- LUFS", juce::dontSendNotification);
    momentaryLufsLabel.setText ("--.- LUFS", juce::dontSendNotification);
    truePeakMaxLabel.setText ("--.- dBTP", juce::dontSendNotification);
    loudnessRangeLabel.setText ("--.- LU", juce::dontSendNotification);

    setupCaptionLabel (lblIntegrated, "INTEGRATED LUFS");
    setupCaptionLabel (lblShortTerm, "SHORT-TERM LUFS");
    setupCaptionLabel (lblMomentary, "MOMENTARY LUFS");
    setupCaptionLabel (lblTruePeak, "TRUE PEAK MAX");
    setupCaptionLabel (lblLoudnessRange, "LOUDNESS RANGE");

    // Hide variables we calculate strictly on the backend
    auto setupHiddenSlider = [] (juce::Slider& s) { s.setVisible (false); };
    setupHiddenSlider (eqLoSlider); setupHiddenSlider (eqMidSlider);
    setupHiddenSlider (compRatioSlider); setupHiddenSlider (compAttackSlider); setupHiddenSlider (compReleaseSlider);

    intensitySlider.setVisible (false);
    intensityLabel.setVisible (false);
    toneBox.setVisible (false);
    toneLabel.setVisible (false);
    compThresholdSlider.setVisible (false);
    limiterDriveSlider.setVisible (false);
    eqHiSlider.setVisible (false);
    driveLabel.setVisible (false);

    // State Tracking Value-Tree Value Mapping Attachments
    eqLoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "EQ_LO_GAIN", eqLoSlider);
    eqMidAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "EQ_MID_GAIN", eqMidSlider);
    compRatioAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "COMP_RATIO", compRatioSlider);
    compAttackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "COMP_ATTACK", compAttackSlider);
    compReleaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "COMP_RELEASE", compReleaseSlider);
    
    // Explicitly binding visible custom knobs onto the active backend DSP state tree
    eqHiAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "EQ_HI_GAIN", eqHiSlider);
    compThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "COMP_THRESHOLD", compThresholdSlider);
    limiterDriveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, "LIMITER_DRIVE", limiterDriveSlider);

    setSize (960, 540);

    deviceManager.initialise (0, 2, nullptr, true);
    formatManager.registerBasicFormats();
    sourcePlayer.setSource (&processedSource);
    deviceManager.addAudioCallback (&sourcePlayer);

    startTimerHz (25);
}

MainComponent::~MainComponent()
{
    setLookAndFeel (nullptr);
    stopTimer();
    transport.stop();
    sourcePlayer.setSource (nullptr);
    deviceManager.removeAudioCallback (&sourcePlayer);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFF16191C));

    auto drawBlock = [&g] (const juce::Rectangle<int>& area)
    {
        if (area.isEmpty())
            return;
        auto bounds = area.toFloat();
        g.setColour (juce::Colour (0xFF101214));
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (juce::Colour (0xFF2D343A));
        g.drawRoundedRectangle (bounds, 8.0f, 1.0f);
    };

    drawBlock (integratedBlock);
    drawBlock (shortTermBlock);
    drawBlock (momentaryBlock);
    drawBlock (truePeakBlock);
    drawBlock (loudnessRangeBlock);
}

// ==============================================================================
// PRO RESPONSIVE THREE COLUMN LAYOUT SYSTEM
// ==============================================================================
void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (20);
    
    // Top Row Section Headers Allocation
    auto headerArea = area.removeFromTop (30);
    auto colWidth = area.getWidth() / 3;
    
    analysisTitleLabel.setBounds (headerArea.removeFromLeft (colWidth));
    chainTitleLabel.setBounds (headerArea.removeFromLeft (colWidth));
    deliveryTitleLabel.setBounds (headerArea);

    // Core Content Slicing Calculations
    auto contentArea = ! area.isEmpty() ? area : getLocalBounds().reduced(20);
    auto column1 = contentArea.removeFromLeft (colWidth - 20);
    auto column2 = contentArea.removeFromLeft (colWidth - 10);
    auto meterPanel = contentArea.removeFromLeft (95);
    auto column3 = contentArea;

    // Column 1: Analysis Layout Engineering
    importButton.setBounds (column1.removeFromTop (36));
    column1.removeFromTop (10);
    fileLabel.setBounds (column1.removeFromTop (20));
    rmsLabel.setBounds (column1.removeFromTop (20));
    peakLabel.setBounds (column1.removeFromTop (20));
    column1.removeFromTop (15);
    spectrumVisualizer.setBounds (column1.removeFromTop (180));

    // Column 2: Loudness dashboard layout (text-driven blocks)
    auto dashboardArea = column2.reduced (6, 4);
    const int rowGap = 10;
    const int colGap = 10;
    const int totalHeight = dashboardArea.getHeight();
    const int row1Height = (int) std::round (totalHeight * 0.40f);
    const int row2Height = (int) std::round (totalHeight * 0.30f);
    const int row3Height = std::max (0, totalHeight - row1Height - row2Height - 2 * rowGap);

    auto row1 = dashboardArea.removeFromTop (row1Height);
    dashboardArea.removeFromTop (rowGap);
    auto row2 = dashboardArea.removeFromTop (row2Height);
    dashboardArea.removeFromTop (rowGap);
    auto row3 = dashboardArea.removeFromTop (row3Height);

    integratedBlock = row1;

    auto row2Left = row2.removeFromLeft ((row2.getWidth() - colGap) / 2);
    row2.removeFromLeft (colGap);
    auto row2Right = row2;

    shortTermBlock = row2Left;
    momentaryBlock = row2Right;

    auto row3Left = row3.removeFromLeft ((row3.getWidth() - colGap) / 2);
    row3.removeFromLeft (colGap);
    auto row3Right = row3;

    truePeakBlock = row3Left;
    loudnessRangeBlock = row3Right;

    auto layoutBlock = [] (const juce::Rectangle<int>& block, juce::Label& value, juce::Label& caption, float valueRatio)
    {
        auto inner = block.reduced (10);
        const int valueHeight = (int) std::round (inner.getHeight() * valueRatio);
        value.setBounds (inner.removeFromTop (valueHeight));
        caption.setBounds (inner);
    };

    layoutBlock (integratedBlock, integratedLufsLabel, lblIntegrated, 0.65f);
    layoutBlock (shortTermBlock, shortTermLufsLabel, lblShortTerm, 0.60f);
    layoutBlock (momentaryBlock, momentaryLufsLabel, lblMomentary, 0.60f);
    layoutBlock (truePeakBlock, truePeakMaxLabel, lblTruePeak, 0.60f);
    layoutBlock (loudnessRangeBlock, loudnessRangeLabel, lblLoudnessRange, 0.60f);

    // Symmetrical Segmented VU Meter Mounting Rails
    auto leftMeterTrack = meterPanel.removeFromLeft (42);
    lblInputMeter.setBounds (leftMeterTrack.removeFromTop (16));
    inputMeter.setBounds (leftMeterTrack.reduced (4, 0));

    auto rightMeterTrack = meterPanel.removeFromRight (42);
    lblOutputMeter.setBounds (rightMeterTrack.removeFromTop (16));
    outputMeter.setBounds (rightMeterTrack.reduced (4, 0));

    // Column 3: Custom Preset Matrix & Mastering Triggers
    profileBox.setBounds (column3.removeFromTop (32));
    profileTargetLabel.setBounds (column3.removeFromTop (24));
    column3.removeFromTop (45);
    
    exportButton.setBounds (column3.removeFromTop (54));
    column3.removeFromTop (20);

    auto playRow = column3.removeFromTop (32);
    playButton.setBounds (playRow.removeFromLeft (80));
    playRow.removeFromLeft (10);
    stopButton.setBounds (playRow.removeFromLeft (80));

    column3.removeFromTop (20);
    listenAfterToggle.setBounds (column3.removeFromTop (22));
    autoApplyToggle.setBounds (column3.removeFromTop (22));
    
    column3.removeFromTop (20);
    exportStatusLabel.setBounds (column3.removeFromTop (20));
    exportProgressBar.setBounds (column3.removeFromTop (16));
}

// ==============================================================================
// ASYNCHRONOUS CONTROLLER ACTIONS & METERS SAMPLING LOOPS
// ==============================================================================
void MainComponent::buttonClicked (juce::Button* b)
{
    if (b == &importButton)
    {
        auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        chooser = std::make_unique<juce::FileChooser> ("Select a WAV file to analyze", juce::File(), "*.wav");

        chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.existsAsFile())
            {
                loadFile (f);
                int intensity = (int)(intensitySlider.getValue() * 100.0);
                bool autoApply = autoApplyToggle.getToggleState();
                processor.startFileAnalysis (f, intensity, autoApply);
            }
        });
    }
    else if (b == &exportButton)
    {
        if (! currentFile.existsAsFile())
            return;

        auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;
        chooser = std::make_unique<juce::FileChooser> ("Export mastered WAV",
                                                       currentFile.getSiblingFile (currentFile.getFileNameWithoutExtension() + "_mastered.wav"),
                                                       "*.wav");

        chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
        {
            auto outFile = fc.getResult();
            if (outFile.getFullPathName().isNotEmpty())
                startExportProcessedFile (outFile, true);
        });
    }
    else if (b == &playButton)
    {
        if (readerSource) transport.start();
    }
    else if (b == &stopButton)
    {
        transport.stop();
    }
}

void MainComponent::timerCallback()
{
    auto formatValue = [] (float value, const juce::String& unit, int maxChars)
    {
        if (! std::isfinite (value))
            return juce::String ("--.- ") + unit;

        auto text = juce::String (value, 1);
        if (text.length() > maxChars)
            text = text.substring (0, maxChars);
        return text + " " + unit;
    };

    float rms = processor.getAnalysisLastRmsDb();
    float peak = processor.getAnalysisLastPeakDb();
    float thr = processor.getAnalysisSuggestedThresholdDb();
    float drv = processor.getAnalysisSuggestedDriveDb();

    rmsLabel.setText ("Original RMS: " + juce::String (rms, 2) + " dB", juce::dontSendNotification);
    peakLabel.setText ("Original Peak: " + juce::String (peak, 2) + " dB", juce::dontSendNotification);

    inputMeter.setLevelDb (processor.getInputRmsDb());
    outputMeter.setLevelDb (processor.getOutputRmsDb());

    const float integrated = processor.getIntegratedLufs();
    const float shortTerm = processor.getShortTermLufs();
    const float momentary = processor.getMomentaryLufs();
    const float lra = processor.getLoudnessRange();
    const float truePeak = processor.getTruePeakMaxDbtp();

    integratedLufsLabel.setText (formatValue (integrated, "LUFS", 6), juce::dontSendNotification);
    shortTermLufsLabel.setText (formatValue (shortTerm, "LUFS", 6), juce::dontSendNotification);
    momentaryLufsLabel.setText (formatValue (momentary, "LUFS", 6), juce::dontSendNotification);
    loudnessRangeLabel.setText (formatValue (lra, "LU", 6), juce::dontSendNotification);
    truePeakMaxLabel.setText (formatValue (truePeak, "dBTP", 6), juce::dontSendNotification);

    const auto peakColour = truePeak > 0.0f ? juce::Colour (0xFFFF4B2B) : juce::Colour (0xFFE7ECEF);
    truePeakMaxLabel.setColour (juce::Label::textColourId, peakColour);

    if (! juce::approximatelyEqual (lastSuggestedThreshold, thr) || ! juce::approximatelyEqual (lastSuggestedDrive, drv))
    {
        lastSuggestedThreshold = thr;
        lastSuggestedDrive = drv;
        applyIntensityFromAnalysis();
    }

    exportProgress = exportProgressAtomic.load();
    if (exportInProgress.load())
        exportStatusLabel.setText ("Export Status: Processing...", juce::dontSendNotification);
    else if (exportProgress >= 1.0)
        exportStatusLabel.setText ("Export Status: Complete!", juce::dontSendNotification);
    else
        exportStatusLabel.setText ("Export Status: Idle", juce::dontSendNotification);
}

void MainComponent::applyTonePreset()
{
    int sel = toneBox.getSelectedId();
    float lo = 0.0f, mid = 0.0f, hi = 0.0f;
    if (sel == 1)      { lo = -1.2f; mid = 0.0f; hi = 2.0f; }
    else if (sel == 2) { lo = 0.0f;  mid = 0.0f; hi = 0.0f; }
    else if (sel == 3) { lo = 1.5f;  mid = 0.3f; hi = -1.5f; }

    if (auto pLo = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_LO_GAIN")))
        pLo->setValueNotifyingHost (pLo->convertTo0to1 (lo));
    if (auto pMid = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_MID_GAIN")))
        pMid->setValueNotifyingHost (pMid->convertTo0to1 (mid));
    if (auto pHi = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_HI_GAIN")))
        pHi->setValueNotifyingHost (pHi->convertTo0to1 (hi));
}

void MainComponent::applyIntensityFromAnalysis()
{
    const float norm = juce::jlimit (0.0f, 1.0f, (float) intensitySlider.getValue());
    const float suggestedTh = processor.getAnalysisSuggestedThresholdDb();
    const float suggestedDrive = processor.getAnalysisSuggestedDriveDb();

    const float drive = juce::jlimit (0.0f, 12.0f, suggestedDrive * (0.5f + norm));
    const float threshold = juce::jlimit (-60.0f, 0.0f, suggestedTh + (0.5f - norm) * 6.0f);

    if (auto* pTh = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_THRESHOLD")))
        pTh->setValueNotifyingHost (pTh->convertTo0to1 (threshold));
    if (auto* pDrv = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
        pDrv->setValueNotifyingHost (pDrv->convertTo0to1 (drive));
}

void MainComponent::applyProfilePreset()
{
    int sel = profileBox.getSelectedId();
    if (sel <= 0 || sel > (int) profiles.size())
        return;

    const auto& p = profiles[(size_t) (sel - 1)];

    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_LO_GAIN")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.eqLo));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_MID_GAIN")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.eqMid));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("EQ_HI_GAIN")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.eqHi));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_THRESHOLD")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.compThreshold));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RATIO")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.compRatio));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_ATTACK")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.compAttack));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("COMP_RELEASE")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.compRelease));
    if (auto* v = dynamic_cast<juce::AudioParameterFloat*> (processor.apvts.getParameter ("LIMITER_DRIVE")))
        v->setValueNotifyingHost (v->convertTo0to1 (p.limiterDrive));

    profileTargetLabel.setText ("Target: " + juce::String (p.targetLufs, 1) + " LUFS, " + juce::String (p.targetTruePeak, 1) + " dBTP",
                                juce::dontSendNotification);
}

void MainComponent::loadFile (const juce::File& file)
{
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (! reader)
    {
        fileLabel.setText ("Failed to load file", juce::dontSendNotification);
        return;
    }

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    transport.setSource (readerSource.get(), 0, nullptr, readerSource->getAudioFormatReader()->sampleRate);
    currentFile = file;
    fileLabel.setText ("File: " + file.getFileName(), juce::dontSendNotification);

    juce::AudioBuffer<float> preview (1, 512);
    preview.clear();
    if (auto* readerPtr = readerSource->getAudioFormatReader())
    {
        readerPtr->read (&preview, 0, preview.getNumSamples(), 0, true, false);
        spectrumVisualizer.setSamples (preview, 0, preview.getNumSamples());
    }
}

void MainComponent::startExportProcessedFile (const juce::File& outFile, bool processAudio)
{
    if (exportInProgress.load())
        return;

    exportInProgress.store (true);
    exportProgressAtomic.store (0.0);
    exportButton.setEnabled (false);

    std::thread ([this, outFile, processAudio]()
    {
        exportProcessedFile (outFile, processAudio);
        exportProgressAtomic.store (1.0);
        exportInProgress.store (false);

        juce::MessageManager::callAsync ([this]()
        {
            exportButton.setEnabled (true);
        });
    }).detach();
}

void MainComponent::exportProcessedFile (const juce::File& outFile, bool processAudio)
{
    if (! currentFile.existsAsFile())
        return;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (currentFile));
    if (! reader)
        return;

    if (outFile.existsAsFile())
        outFile.deleteFile();

    auto outStream = outFile.createOutputStream();
    if (outStream == nullptr)
        return;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (outStream.get(),
                                                                          reader->sampleRate,
                                                                          (unsigned int) reader->numChannels,
                                                                          24,
                                                                          {},
                                                                          0));
    if (! writer)
        return;
    outStream.release();

    const int blockSize = 1024;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, blockSize);

    MainAudioProcessor offlineProcessor;
    offlineProcessor.apvts.replaceState (processor.apvts.copyState());
    offlineProcessor.prepareToPlay (reader->sampleRate, blockSize);

    juce::int64 samplesRead = 0;
    while (samplesRead < reader->lengthInSamples)
    {
        const int samplesThisIter = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - samplesRead);
        reader->read (&buffer, 0, samplesThisIter, samplesRead, true, true);

        if (processAudio)
        {
            juce::MidiBuffer midi;
            offlineProcessor.processBlock (buffer, midi);
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, samplesThisIter);
        samplesRead += samplesThisIter;
        exportProgressAtomic.store ((double) samplesRead / (double) std::max<juce::int64> (1, reader->lengthInSamples));
    }

    offlineProcessor.releaseResources();
}