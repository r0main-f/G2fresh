set(G2_PLUGIN_FORMATS Standalone VST3)
if(APPLE)
  list(APPEND G2_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(G2Editor
  PRODUCT_NAME "G2fresh"
  COMPANY_NAME "G2fresh"
  BUNDLE_ID "org.g2fresh.editor"
  PLUGIN_MANUFACTURER_CODE G2fr
  PLUGIN_CODE G2ed
  FORMATS ${G2_PLUGIN_FORMATS}
  # An instrument so hosts route a MIDI track to it; it outputs silence.
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_CATEGORIES Instrument Tools
  AU_MAIN_TYPE kAudioUnitType_MusicDevice)

target_sources(G2Editor PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/StandaloneApp.cpp
  ${G2_UI_SOURCES})
target_include_directories(G2Editor PRIVATE ${PROJECT_SOURCE_DIR}/ui)

target_compile_definitions(G2Editor PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

target_link_libraries(G2Editor
  PRIVATE
    g2core
    G2SkinData
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
