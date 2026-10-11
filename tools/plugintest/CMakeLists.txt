# g2plugintest: hosts the built G2fresh.vst3 as a DAW would and checks what a DAW relies on (the emulated G2
# starting by itself, its latency, sound from MIDI, the synth memory in the state, offline rendering; with
# --outputs and --inputs its output and input buses, each with a patch: see the usage in main.cpp). Needs the
# user's G2 OS set up once (Synth > Connect to Emulated G2), as the plugin does. An Audio Unit is found by the
# system, not by its path: a .component given here must be the one installed.
# A GUI app without a window: the plugin needs a running message loop (a console app has none on macOS).
juce_add_gui_app(g2plugintest PRODUCT_NAME "g2plugintest")
target_sources(g2plugintest PRIVATE main.cpp)
target_compile_definitions(g2plugintest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}> JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(g2plugintest PRIVATE juce::juce_audio_processors juce::juce_audio_utils juce::juce_gui_extra
  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
