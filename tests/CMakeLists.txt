include(FetchContent)
# Built from source so universal (arm64 + x86_64) and cross-platform builds work.
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(g2tests
  test_bitstream.cpp
  test_hostclock.cpp
  test_midi_assembler.cpp
  test_audio_io.cpp
  test_resampler.cpp
  test_edit.cpp
  test_cables.cpp
  test_focus.cpp
  test_split_bar.cpp
  test_morph.cpp
  test_file.cpp
  test_patch.cpp
  test_convert.cpp
  test_replace.cpp
  test_special.cpp
  test_mutate.cpp
  test_patch_load.cpp
  test_uprate.cpp
  test_param_text.cpp
  test_param_text_golden.cpp
  test_graphs.cpp
  test_library.cpp
  test_proto_framing.cpp
  test_proto_messages.cpp
  test_proto_session.cpp
  test_proto_link.cpp
  test_proto_live.cpp
  test_proto_log.cpp
  test_bridge.cpp
  test_engine.cpp
  test_clavia.cpp
  test_synth_settings.cpp
  test_live_show.cpp
  test_focus_keys.cpp
  test_inc_dec.cpp
  test_module_bar_keys.cpp
  test_module_search.cpp
  test_cable_curve.cpp
  test_cable_view.cpp
  test_cable_colours.cpp
  test_zoom_fit.cpp
  test_typed_value.cpp
  test_selection_file.cpp
  test_button_press.cpp
  test_patch_keys.cpp
  test_drag_copy.cpp
  test_paste_layout.cpp
  test_drum_selectors.cpp
  test_mutator_set.cpp
  test_perf_sync.cpp
  test_benchpack.cpp
)
target_link_libraries(g2tests PRIVATE g2bridgelib g2engine g2clavia Catch2::Catch2WithMain) # g2proto and g2core come with it
target_include_directories(g2tests PRIVATE ${PROJECT_SOURCE_DIR}/plugin/Source) # HostClock.h, AudioIo.h, Resampler.h (plain C++)
target_include_directories(g2tests PRIVATE ${PROJECT_SOURCE_DIR}/ui) # SynthSettingsForm.h (plain C++)
target_compile_definitions(g2tests PRIVATE G2_BRIDGE_EXECUTABLE="$<TARGET_FILE:g2bridge>")
add_dependencies(g2tests g2bridge)
if(TARGET g2usb)
  target_link_libraries(g2tests PRIVATE g2usb)
  target_compile_definitions(g2tests PRIVATE G2_HAVE_USB=1)
endif()
target_compile_definitions(g2tests PRIVATE
  G2_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
  G2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_definitions(g2tests PRIVATE G2_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

include(Catch)
catch_discover_tests(g2tests)

# The libusb transport as it is, on a scripted libusb (fake_libusb.cpp) instead of the real one: no hardware needed.
# It runs on a clock the tests move, so they take milliseconds; the timeout is for an I/O thread that would never end.
if(TARGET g2usb)
  add_executable(g2usbtests test_usb_transport.cpp fake_libusb.cpp ${PROJECT_SOURCE_DIR}/usb/src/libusb_transport.cpp)
  target_include_directories(g2usbtests PRIVATE ${PROJECT_SOURCE_DIR}/usb/include)
  target_include_directories(g2usbtests SYSTEM PRIVATE $<TARGET_PROPERTY:usb-1.0,INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(g2usbtests PRIVATE g2proto Catch2::Catch2WithMain)
  target_compile_definitions(g2usbtests PRIVATE G2_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
  catch_discover_tests(g2usbtests PROPERTIES TIMEOUT 60)
endif()

# Checks that need Python: the generated module database is in sync with
# data/*.json, and the reference decoder round-trips the corpus.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mutate_param_classes_are_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/mutate/gen_param_classes.py --check)
  add_test(NAME replace_tables_are_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/moduledb/gen_replace.py --check)
  add_test(NAME patch_load_table_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/patchload/gen_table.py --check)
  add_test(NAME protocol_vectors_are_current
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/protocol/gen_cpp.py --check)
  add_test(NAME no_clavia_pixels_in_assets
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/assets/build_assets.py --check)
  add_test(NAME module_db_generated_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/moduledb/gen_cpp.py --check)
  file(GLOB G2_CORPUS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/corpus/pch2csd/*.pch2)
  add_test(NAME reference_decoder_roundtrip
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pch2/pch2dump.py roundtrip ${G2_CORPUS_FILES})
endif()

# The patch library's audition (SynthSync::audition, in the editor's own code) on the virtual G2: the document is
# never touched and the synth gets back what it had. g2render is a JUCE program: not on Linux, where the tests run
# without a display.
if(TARGET g2render AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME library_audition_on_the_virtual_g2 COMMAND g2render audition)
endif()

# The hardware bench pack (bench/hardware-pack, tools/benchpack): the committed patches are what the generator
# writes, the printable pages are what the instructions give, and the analysis finds the faults of synthetic
# recordings. With the emulator built and G2_FIRMWARE set (Clavia's OS update .dmg), four of its patches are also
# played on the emulated G2 and compared with the committed predictions (B10: with its stimulus on In 1); skipped
# otherwise.
add_test(NAME benchpack_patches_are_current
  COMMAND g2benchpack --check ${PROJECT_SOURCE_DIR}/bench/hardware-pack)
if(Python3_FOUND)
  add_test(NAME benchpack_pages_are_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/benchpack/make_html.py --check)
  add_test(NAME benchpack_stimulus_is_current
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/benchpack/make_stimulus.py --check)
  add_test(NAME benchpack_analysis_selftest
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/benchpack/selftest.py)
  if(TARGET g2benchrender)
    add_test(NAME benchpack_expected_matches_emulator
      COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/benchpack/expected.py --check --only B01,B07,B10,B13
        --build ${CMAKE_BINARY_DIR})
    set_tests_properties(benchpack_expected_matches_emulator PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# The real-time G2 emulator (emu/g2emu, built with -DG2_BUILD_EMU=ON). Its [firmware] case boots the user's own OS
# when it finds it (G2_FIRMWARE, or original/firmware) and is skipped otherwise.
if(TARGET g2emu)
  add_executable(g2emutests test_g2emu.cpp test_live_firmware.cpp test_midi_out.cpp)
  target_link_libraries(g2emutests PRIVATE g2emu g2coldfire dsp56kEmu Catch2::Catch2WithMain)
  target_include_directories(g2emutests PRIVATE ${PROJECT_SOURCE_DIR}/emu/g2emu/src)
  target_include_directories(g2emutests PRIVATE ${PROJECT_SOURCE_DIR}/ui) # SynthSettingsForm.h (plain C++)
  target_include_directories(g2emutests PRIVATE ${PROJECT_SOURCE_DIR}/plugin/Source) # EmulatedMachine.h (plain C++)
  target_include_directories(g2emutests SYSTEM PRIVATE ${G2_DSP56300_SOURCE_DIR}/source)
  target_compile_definitions(g2emutests PRIVATE G2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  catch_discover_tests(g2emutests)
endif()

# The stand-alone app has G2fresh's icon (macOS: the bundle's .icns, which JUCE makes from plugin/icon/G2fresh.png).
if(APPLE AND TARGET G2Editor_Standalone AND Python3_FOUND)
  add_test(NAME app_has_its_icon
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/app_icon_check.py --app $<TARGET_BUNDLE_DIR:G2Editor_Standalone>)
endif()

# The audio path's measurements (tools/audiopath) run the emulated G2 the plugin runs: four DSPs. Needs G2_FIRMWARE.
if(TARGET g2emu AND Python3_FOUND)
  add_test(NAME audiopath_runner_is_the_plugins_machine
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/audiopath/runner_check.py --tool $<TARGET_FILE:g2audiopath>)
  set_tests_properties(audiopath_runner_is_the_plugins_machine PROPERTIES SKIP_RETURN_CODE 77)
endif()
