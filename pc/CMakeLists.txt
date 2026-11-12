cmake_minimum_required(VERSION 3.20)
project(mobcam LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "MobCam's PC side needs MSVC (Visual Studio Build Tools) and the Windows 11 SDK")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLLs are loaded by Windows services / .NET, so don't depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /wd4100 /wd4458)

set(BIN_DIR "${CMAKE_BINARY_DIR}/bin")

# ---- Virtual camera media source (loaded by Windows Frame Server)
add_library(mobcam-source SHARED
  source/activator.cpp
  source/dllmain.cpp
  source/frame_source.cpp
  source/media_source.cpp
  source/media_stream.cpp
  source/mobcam-source.def
)
target_link_libraries(mobcam-source PRIVATE
  mfplat mfuuid mf mfsensorgroup propsys ole32 oleaut32 advapi32 gdi32 user32)

# ---- Engine sources shared by the DLL (for the app) and the CLI
set(CORE_SOURCES
  core/decoder.cpp
  core/engine.cpp
  core/frame_writer.cpp
  core/install.cpp
  core/net.cpp
  core/secure.cpp
)
set(CORE_LIBS mfplat mfuuid mf mfsensorgroup wmcodecdspuuid ole32 advapi32 shell32 ws2_32 iphlpapi bcrypt ncrypt)

# mobcam-core.dll: C API used by the WPF app through P/Invoke
add_library(mobcam-core SHARED ${CORE_SOURCES})
target_compile_definitions(mobcam-core PRIVATE MOBCAM_CORE_EXPORTS)
target_link_libraries(mobcam-core PRIVATE ${CORE_LIBS})

# mobcam-cli.exe: command line + elevated installer helper (engine linked statically)
add_executable(mobcam-cli cli/main.cpp ${CORE_SOURCES})
target_compile_definitions(mobcam-cli PRIVATE MOBCAM_CORE_STATIC)
target_link_libraries(mobcam-cli PRIVATE ${CORE_LIBS})

foreach(t mobcam-source mobcam-core mobcam-cli)
  set_target_properties(${t} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${BIN_DIR}")
endforeach()
