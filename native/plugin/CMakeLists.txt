# JUCE front panel: standalone app + VST3.
# Set -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to build against a local checkout.
include(FetchContent)
FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_plugin(MoodEmu
  VERSION 0.1.0
  COMPANY_NAME "cba-guitar-pedal"
  PLUGIN_MANUFACTURER_CODE Cbgp
  PLUGIN_CODE Mdem
  FORMATS Standalone VST3
  PRODUCT_NAME "MoodEmu"
  DESCRIPTION "Wet channel + micro-looper emulator (unofficial, inspired by the Chase Bliss MOOD)"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "MoodEmu needs the audio input to run your guitar through the pedal."
  VST3_CATEGORIES Fx Delay Reverb
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MoodEmu PRIVATE PluginProcessor.cpp PluginEditor.cpp)
target_include_directories(MoodEmu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(MoodEmu PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0 _CRT_SECURE_NO_WARNINGS=1)
target_link_libraries(MoodEmu
  PRIVATE juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
