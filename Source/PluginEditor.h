#pragma once

#include "PluginProcessor.h"
#include "UI/Analyser.h"
#include "UI/Guide.h"
#include "UI/Widgets.h"

class AtelierEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit AtelierEditor (AtelierProcessor&);
    ~AtelierEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // listens to every control so the Guide can explain whatever is under the pointer
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int baseW = 1280, fullH = 904, compactH = 696;   // with / without the Guide strip

private:
    using Section = atelier::ui::Section;

    /** Everything is laid out at base size inside this and scaled to the window. */
    struct Content final : juce::Component
    {
        void paint (juce::Graphics&) override;
        juce::String flowText;
    };

    void timerCallback() override;
    void modeChanged (int mode);
    void layoutRows();
    void updateFlowText();
    void updateHover (const juce::MouseEvent&);
    int baseH() const   { return bottomView != viewNone ? fullH : compactH; }
    void applyWindowSize (int width);
    void setBottomView (int view);

    // presets live as small XML files in ~/Library/Audio/Presets/Channel Audio/Atelier
    static juce::File presetFolder();
    void refreshPresets();
    void presetChosen();
    void savePreset();
    void loadPreset (const juce::File&);
    void resetToInit();

    void knob (Section&, const char* paramId, const char* caption);
    void combo (Section&, const char* paramId, const char* caption, int width = 92);
    juce::ComboBox* headerCombo (Section&, const char* paramId, int width);
    atelier::ui::Readout* readout (Section&, const char* caption);

    AtelierProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    atelier::ui::AtelierLookAndFeel lnf;
    Content content;

    juce::OwnedArray<juce::Component> owned;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;

    Section input, gate, tube, deEss, eq, comp, tape, iron, image, limiter, output;
    atelier::ui::MeterPanel meterPanel;
    atelier::ui::GuidePanel guidePanel;
    atelier::ui::TubeLamp* lamp = nullptr;
    atelier::ui::Readout *gateRead = nullptr, *peakRead = nullptr;
    juce::ComboBox *eqMsBox = nullptr, *ditherBox = nullptr;
    float grClipDisp = 0.0f;
    std::unique_ptr<atelier::ui::ChoiceButtons> modeButtons;
    std::unique_ptr<atelier::ui::LedButton> bypassButton, lowLatButton;
    float keyHold = -100.0f, recentIn = -100.0f;
    atelier::ui::AnalyserPanel analyserPanel;
    atelier::ui::TextToggle guideButton { "GUIDE", atelier::ui::col::gold }, scopeButton { "ANALYSER", atelier::ui::col::gold };
    atelier::ui::TextToggle aButton { "A", atelier::ui::col::ivory }, bButton { "B", atelier::ui::col::ivory };
    atelier::ui::TextToggle undoButton { "UNDO", atelier::ui::col::ivory }, redoButton { "REDO", atelier::ui::col::ivory };
    atelier::ui::TextToggle* deltaButton = nullptr;
    juce::ComboBox presetBox;
    juce::Array<juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;

    enum { viewNone = 0, viewGuide, viewAnalyser };
    int bottomView = viewGuide;

    int mode = 0, ticks = 0;
    float inHold[2] { -100.0f, -100.0f }, outHold[2] { -100.0f, -100.0f };
    float vuDisp = -40.0f, grCompDisp = 0.0f, grGateDisp = 0.0f, grDsDisp = 0.0f, grLimDisp = 0.0f, heatDisp = 0.0f;
    float peakHold = -100.0f;
    int peakHoldTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AtelierEditor)
};
