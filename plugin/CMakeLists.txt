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
  Source/AutomationBank.cpp
  Source/MidiForwarder.cpp
  Source/NativeSoundEngine.cpp
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
    g2bridgelib # with g2proto and g2core
    g2engine
    G2SkinData
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# The g2bridge executable goes with the app (next to its executable) and in
# each plugin bundle's Resources, where SynthSync looks for it.
if(TARGET g2bridge)
  foreach(format ${G2_PLUGIN_FORMATS})
    set(target G2Editor_${format})
    if(NOT TARGET ${target})
      continue()
    endif()
    add_dependencies(${target} g2bridge)
    get_target_property(artefact ${target} JUCE_PLUGIN_ARTEFACT_FILE)
    if(format STREQUAL "Standalone")
      # On macOS the executable is in the .app's Contents/MacOS; elsewhere the
      # app is the executable itself (JUCE leaves the artefact property unset).
      set(dest "$<TARGET_FILE_DIR:${target}>")
    elseif(artefact)
      set(dest "${artefact}/Contents/Resources")
    else()
      continue()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:g2bridge>" "${dest}/"
      VERBATIM)
  endforeach()
endif()
