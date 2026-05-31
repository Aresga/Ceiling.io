cmake_minimum_required(VERSION 3.15)
project(ceilingIO VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ceilingIO_FETCH_SERVER_DEPS "Fetch Crow and Asio for the server target" OFF)

# Point directly to the location you confirmed, but allow Docker and other
# environments to override it with a cache path.
set(JUCE_DIR "/home/ares/JUCE" CACHE PATH "Path to the JUCE source tree")
if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE_DIR must point to a valid JUCE checkout")
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

if (ceilingIO_FETCH_SERVER_DEPS)
    include(FetchContent)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(AUTORUN_UNIT_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        GIT_SUBMODULES "")

    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)

    FetchContent_Declare(crow
        GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        GIT_SUBMODULES "")

    FetchContent_MakeAvailable(crow)

    # AWS SDK support removed; presigned-URL flow uses HTTP(S) via JUCE curl.
endif()

juce_add_gui_app(ceilingIO
    PRODUCT_NAME "ceilingIO"
    NEEDS_CURL TRUE
    NEEDS_WEB_BROWSER FALSE
)

target_compile_definitions(ceilingIO PRIVATE JUCE_WEB_BROWSER=0)

set(ceilingIO_SHARED_SOURCES
    src/MainAudioProcessor.cpp
    src/AnalysisWorker.cpp
    src/ceilingIOPipeline.cpp)

target_sources(ceilingIO PRIVATE
    ${ceilingIO_SHARED_SOURCES}
    src/MainComponent.cpp
    src/Main.cpp)

target_include_directories(ceilingIO PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ceilingIO PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_events
    juce::juce_data_structures
    juce::juce_core
)

if (APPLE)
    target_link_libraries(ceilingIO PRIVATE "-fobjc-arc")
endif()

juce_generate_juce_header(ceilingIO)

juce_add_console_app(ceilingIOCli
    PRODUCT_NAME "ceilingIO CLI"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_compile_definitions(ceilingIOCli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_sources(ceilingIOCli PRIVATE
    ${ceilingIO_SHARED_SOURCES}
    src/MainCli.cpp)

target_include_directories(ceilingIOCli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(ceilingIOCli PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_core)

juce_generate_juce_header(ceilingIOCli)

juce_add_console_app(ceilingIOServer
    PRODUCT_NAME "ceilingIO Server"
    NEEDS_CURL TRUE
    NEEDS_WEB_BROWSER FALSE)

target_compile_definitions(ceilingIOServer PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1)

target_sources(ceilingIOServer PRIVATE
    ${ceilingIO_SHARED_SOURCES}
    src/server/MainServer.cpp)

target_include_directories(ceilingIOServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(ceilingIOServer PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_events
    juce::juce_data_structures
    juce::juce_core)

if (APPLE)
    target_link_libraries(ceilingIOServer PRIVATE "-fobjc-arc")
endif()

juce_generate_juce_header(ceilingIOServer)

if (TARGET Crow::Crow)
    target_link_libraries(ceilingIOServer PRIVATE Crow::Crow)
endif()

# AWS SDK linking removed; server uses presigned HTTP(S) URLs only.




