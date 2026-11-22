cmake_minimum_required(VERSION 3.24)
project(WinAudioEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /MP /EHsc /utf-8 /permissive-)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_compile_options($<$<CONFIG:Release>:/Oi>)
endif()

# 1. Pure DSP & lock-free utilities static library (platform-minimal for fast unit testing)
add_library(WinAudioDSP STATIC
    engine/src/dsp/Equalizer.cpp
    engine/src/dsp/ToneBooster.cpp
    engine/src/dsp/LoudnessNorm.cpp
    engine/src/dsp/SpatialEffects.cpp
    engine/src/dsp/Maximizer.cpp
    engine/src/dsp/SpectrumAnalyzer.cpp
    engine/src/dsp/MasterDspChain.cpp
    engine/src/audio/DriftResampler.cpp
    engine/src/audio/AcousticCalibrator.cpp
)

target_include_directories(WinAudioDSP PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
)

# 2. Native Windows Audio Engine DLL (C-ABI exported for C# .NET 9 P/Invoke)
add_library(WinAudioEngine SHARED
    engine/src/audio/PolicyConfig.cpp
    engine/src/audio/DeviceManager.cpp
    engine/src/audio/OutputWorker.cpp
    engine/src/audio/CaptureWorker.cpp
    engine/src/audio/AudioEngine.cpp
    engine/src/WinAudioExports.cpp
)

target_compile_definitions(WinAudioEngine PRIVATE WINAUDIO_BUILD_DLL)
target_include_directories(WinAudioEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
)
target_link_libraries(WinAudioEngine PRIVATE
    WinAudioDSP
    ole32
    avrt
    mmdevapi
    ksuser
    uuid
    propsys
)

# 3. C++ Automated Unit & Integration Test Suite
add_executable(WinAudioEngineTests
    engine/tests/EngineTests.cpp
)

target_include_directories(WinAudioEngineTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
)
target_link_libraries(WinAudioEngineTests PRIVATE
    WinAudioDSP
    WinAudioEngine
    ole32
)

enable_testing()
add_test(NAME WinAudioEngineTests COMMAND WinAudioEngineTests)
