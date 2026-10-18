cmake_minimum_required(VERSION 3.22)

# Both must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
# Windows: the C++ runtime inside the binaries, so they load without the
# Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(DrumRebuilder VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.4, pinned by commit and checksum: the same JUCE as Adormis Audio,
# so drb_core can be compiled into it unchanged.
# For local builds, -DDRB_JUCE_DIR=/path/to/JUCE skips the download.
set(DRB_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(NOT TARGET juce::juce_core)
    if(DRB_JUCE_DIR)
        add_subdirectory(${DRB_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            URL      https://github.com/juce-framework/JUCE/archive/3fb5cbc0340758596dac39f7c8de47d562b4896e.tar.gz
            URL_HASH SHA256=2bb7095497a7bb7269dbdf841a7c6aa0939eaeb2a2bc9724ac4758d3ae3565a7)
        FetchContent_MakeAvailable(JUCE)
    endif()
endif()

# drb_core: analysis, MIDI, samples and the audio engine. No GUI, no plugin
# code, so another JUCE app (Adormis Audio) can link it as it is. It's an
# INTERFACE library: every target compiles the sources with its own JUCE
# modules, which avoids two copies of juce_core in one binary.
add_library(drb_core INTERFACE)
target_sources(drb_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Analyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Onsets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Tempo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/AudioFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Player.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Metronome.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Classify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Midi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Samples.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Rebuild.cpp)
target_include_directories(drb_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(drb_core INTERFACE juce::juce_audio_formats juce::juce_dsp)

# Only the top-level build makes the plugin and the tests; a parent project
# that pulls this folder in just gets drb_core.
if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

# AU exists only on macOS.
set(DRB_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND DRB_FORMATS AU)
endif()

juce_add_plugin(DrumRebuilder
    PRODUCT_NAME "Drum Rebuilder"
    COMPANY_NAME "Adormis"
    BUNDLE_ID    "io.github.jastincheis.drumrebuilder"
    PLUGIN_MANUFACTURER_CODE Ador
    PLUGIN_CODE  Drbr
    IS_SYNTH     TRUE
    NEEDS_MIDI_INPUT  FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Instrument Drum
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS      ${DRB_FORMATS}
    VERSION      ${PROJECT_VERSION})

target_sources(DrumRebuilder PRIVATE
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp
    plugin/WaveformView.cpp
    plugin/MidiEditorView.cpp)

target_compile_definitions(DrumRebuilder PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(DrumRebuilder
    PRIVATE
        drb_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Tests: synthetic drum beats with known hits, run through the analyzer, scored.
juce_add_console_app(drb_tests PRODUCT_NAME "drb_tests")
target_sources(drb_tests PRIVATE
    tests/TestMain.cpp
    tests/SyntheticDrums.cpp
    tests/Evaluation.cpp)
target_compile_definitions(drb_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(drb_tests
    PRIVATE
        drb_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME drb_tests COMMAND drb_tests)
