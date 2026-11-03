# EarbudControls-Setup.exe. Built by scripts/prepare-release.ps1 after the MSIX is signed, with the
# payload folder passed in: EarbudControls.msix, Microsoft.WindowsAppRuntime.2.msix and the .cer.
cmake_minimum_required(VERSION 3.31)
project(EarbudControlsSetup LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no C++ runtime to install first
if(NOT DEFINED EARBUD_PAYLOAD OR NOT DEFINED EARBUD_VERSION)
  message(FATAL_ERROR "Pass -DEARBUD_PAYLOAD=<folder> -DEARBUD_VERSION=<x.y.z>.")
endif()
foreach(part EarbudControls.msix Microsoft.WindowsAppRuntime.2.msix EarbudControls.Local.cer)
  if(NOT EXISTS "${EARBUD_PAYLOAD}/${part}")
    message(FATAL_ERROR "Missing ${EARBUD_PAYLOAD}/${part}")
  endif()
endforeach()
file(SHA256 "${EARBUD_PAYLOAD}/EarbudControls.msix" APP_SHA)
file(SHA256 "${EARBUD_PAYLOAD}/Microsoft.WindowsAppRuntime.2.msix" RUNTIME_SHA)
file(SHA256 "${EARBUD_PAYLOAD}/EarbudControls.Local.cer" CERT_SHA)
string(TOUPPER "${APP_SHA}" APP_SHA)
string(TOUPPER "${RUNTIME_SHA}" RUNTIME_SHA)
string(TOUPPER "${CERT_SHA}" CERT_SHA)
file(TO_NATIVE_PATH "${EARBUD_PAYLOAD}" PAYLOAD_NATIVE)
string(REPLACE "\\" "\\\\" PAYLOAD_RC "${PAYLOAD_NATIVE}")
file(WRITE "${CMAKE_BINARY_DIR}/payload.h"
"#pragma once\n#define IDR_APP 101\n#define IDR_RUNTIME 102\n#define IDR_CERT 103\n#ifdef __cplusplus\n"
"static constexpr wchar_t kVersion[] = L\"${EARBUD_VERSION}\";\n"
"static constexpr wchar_t kAppSha256[] = L\"${APP_SHA}\";\n"
"static constexpr wchar_t kRuntimeSha256[] = L\"${RUNTIME_SHA}\";\n"
"static constexpr wchar_t kCertSha256[] = L\"${CERT_SHA}\";\n#endif\n")
file(WRITE "${CMAKE_BINARY_DIR}/payload.rc"
"#include \"payload.h\"\n#include <winres.h>\n"
"1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/setup.ico\"\n"
"IDR_APP RCDATA \"${PAYLOAD_RC}\\\\EarbudControls.msix\"\n"
"IDR_RUNTIME RCDATA \"${PAYLOAD_RC}\\\\Microsoft.WindowsAppRuntime.2.msix\"\n"
"IDR_CERT RCDATA \"${PAYLOAD_RC}\\\\EarbudControls.Local.cer\"\n"
"1 24 \"${CMAKE_CURRENT_SOURCE_DIR}/setup.manifest\"\n")
add_executable(EarbudControls-Setup WIN32 setup.cpp "${CMAKE_BINARY_DIR}/payload.rc")
target_include_directories(EarbudControls-Setup PRIVATE "${CMAKE_BINARY_DIR}")
target_compile_definitions(EarbudControls-Setup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(EarbudControls-Setup PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(EarbudControls-Setup PRIVATE windowsapp comctl32 crypt32 bcrypt shell32 advapi32 ole32)
target_link_options(EarbudControls-Setup PRIVATE /MANIFEST:NO) # the manifest comes from payload.rc
set_target_properties(EarbudControls-Setup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
