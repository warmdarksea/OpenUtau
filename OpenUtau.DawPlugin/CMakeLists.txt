cmake_minimum_required(VERSION 3.22)

# OpenUtau DAW bridge plugin: the plugin half of OpenUtau.Core/DawIntegration
# (PROTOCOL spec: OpenUtau.Core/DawIntegration/API.md). Builds as VST3, CLAP
# and - against the GPL FST headers - VST2, plus the bridge-host console
# harness that OpenUtau.Test/Core/DawIntegration/DawRealPluginTest.cs drives.
#
# This subproject is C++/JUCE and GPLv3 (JUCE GPL mode + FST), unlike the
# Apache-2.0 C# application around it. See README.md.

project(OpenUtauDawPlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)

# The bundled FST headers stand in for Steinberg's unredistributable VST2 SDK.
set(JUCE_GLOBAL_VST2_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/fst" CACHE PATH "" FORCE)

add_subdirectory(3rdparty/JUCE EXCLUDE_FROM_ALL)

juce_add_plugin(OpenUtauBridge
    COMPANY_NAME "OpenUtau"
    COMPANY_WEBSITE "https://github.com/stakira/OpenUtau"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE OpnU
    PLUGIN_CODE OuBr
    FORMATS VST3 VST
    PRODUCT_NAME "OpenUtau Bridge"
    # No moduleinfo.json: it is optional per the VST3 spec, and at least one
    # host's metadata scanner (Bitwig 5.x on Linux) chokes on the relaxed JSON
    # the generator emits, rejecting the whole bundle.
    VST3_AUTO_MANIFEST FALSE
    BUNDLE_ID "world.openutau.bridge"
)

set(BRIDGE_SOURCES
    source/BridgeProtocol.h
    source/BridgeProtocol.cpp
    source/BridgeState.h
    source/BridgeState.cpp
    source/BridgeServer.h
    source/BridgeServer.cpp
    source/BridgeProcessor.h
    source/BridgeProcessor.cpp
    source/BridgeEditor.h
    source/BridgeEditor.cpp
)

target_sources(OpenUtauBridge PRIVATE ${BRIDGE_SOURCES})
target_include_directories(OpenUtauBridge PRIVATE source 3rdparty/xxHash)
target_compile_definitions(OpenUtauBridge PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0  # GPLv3 build
)
target_link_libraries(OpenUtauBridge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# JUCE clears the default "lib" prefix for VST3 but not for the VST2 shared
# object; hosts derive the friendly name from the filename.
if(UNIX AND NOT APPLE AND TARGET OpenUtauBridge_VST)
    set_target_properties(OpenUtauBridge_VST PROPERTIES PREFIX "")
endif()

# CLAP through clap-juce-extensions, Bitwig's native format.
add_subdirectory(3rdparty/clap-juce-extensions EXCLUDE_FROM_ALL)
clap_juce_extensions_plugin(TARGET OpenUtauBridge
    CLAP_ID "world.openutau.bridge"
    CLAP_FEATURES instrument "vocal synthesis")

# bridge-host: the standalone conformance harness DawRealPluginTest drives.
# It is the same server and timeline as the plugin, paced by a fake transport.
juce_add_console_app(bridge-host PRODUCT_NAME "bridge-host")
target_sources(bridge-host PRIVATE
    ${BRIDGE_SOURCES}
    source/bridgehost/BridgeHostMain.cpp
)
target_include_directories(bridge-host PRIVATE source 3rdparty/xxHash)
target_compile_definitions(bridge-host PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    OUBRIDGE_HEADLESS_HOST=1
)
target_link_libraries(bridge-host PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
)

# Install layout: the usual Linux plugin prefix shape (lib/vst3, lib/vst,
# lib/clap) plus the conformance host under bin/.
get_target_property(bridgeVst3Dir OpenUtauBridge_VST3 ARCHIVE_OUTPUT_DIRECTORY)
install(DIRECTORY "${bridgeVst3Dir}/OpenUtau Bridge.vst3" DESTINATION lib/vst3 USE_SOURCE_PERMISSIONS)
if(TARGET OpenUtauBridge_VST)
    install(TARGETS OpenUtauBridge_VST LIBRARY DESTINATION lib/vst)
endif()
if(TARGET OpenUtauBridge_CLAP)
    install(TARGETS OpenUtauBridge_CLAP LIBRARY DESTINATION lib/clap)
endif()
install(TARGETS bridge-host RUNTIME DESTINATION bin)

# Protocol unit tests: framing and envelopes, no sockets.
juce_add_console_app(bridge-protocol-test PRODUCT_NAME "bridge-protocol-test")
target_sources(bridge-protocol-test PRIVATE
    source/BridgeProtocol.cpp
    source/tests/BridgeProtocolTest.cpp
)
target_include_directories(bridge-protocol-test PRIVATE source 3rdparty/xxHash)
target_compile_definitions(bridge-protocol-test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(bridge-protocol-test PRIVATE juce::juce_core juce::juce_recommended_config_flags)
add_test(NAME bridgeProtocol COMMAND bridge-protocol-test)
