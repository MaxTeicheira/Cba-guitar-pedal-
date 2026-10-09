# JUCE front panel: standalone app + VST3.
# Set -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to build against a local checkout.
include(FetchContent)
FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_plugin(BadMoodEmu
  VERSION 0.2.0
  COMPANY_NAME "cba-guitar-pedal"
  PLUGIN_MANUFACTURER_CODE Cbgp
  PLUGIN_CODE Bdme
  FORMATS Standalone VST3
  PRODUCT_NAME "BadMoodEmu"
  DESCRIPTION "Wet channel + micro-looper emulator (unofficial, inspired by the Chase Bliss BAD MOOD)"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "BadMoodEmu needs the audio input to run your guitar through the pedal."
  BUNDLE_ID com.cba-guitar-pedal.badmoodemu
  VST3_CATEGORIES Fx Delay Reverb
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(BadMoodEmu PRIVATE PluginProcessor.cpp PluginEditor.cpp)
target_compile_definitions(BadMoodEmu PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_REPORT_APP_USAGE=0 _CRT_SECURE_NO_WARNINGS=1)
target_link_libraries(BadMoodEmu
  PRIVATE juce::juce_audio_utils badmood_dsp
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
