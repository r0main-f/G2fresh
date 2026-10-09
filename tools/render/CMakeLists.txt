juce_add_console_app(g2render PRODUCT_NAME "g2render")
target_sources(g2render PRIVATE main.cpp ${G2_UI_SOURCES})
target_include_directories(g2render PRIVATE ${PROJECT_SOURCE_DIR}/ui)
target_compile_definitions(g2render PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(g2render PRIVATE g2core G2SkinData juce::juce_gui_extra juce::juce_data_structures
  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
