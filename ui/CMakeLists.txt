# EarbudControls.UI.exe: the Dusk WinUI app. Built from loose XAML (loaded with XamlReader), so no
# MSBuild XAML compiler is needed. Uses the Windows App SDK 2.5.1 set pinned in scripts/bootstrap.ps1.
set(EARBUD_UI_SDK "${CMAKE_SOURCE_DIR}/.deps/nuget/winappsdk-2.5.1")
if(NOT EXISTS "${CMAKE_BINARY_DIR}/generated-ui/winrt/Microsoft.UI.Xaml.h")
  message(FATAL_ERROR "Run scripts/bootstrap.ps1 to generate the WinUI C++ projections.")
endif()
add_executable(EarbudControls.UI WIN32 main.cpp app.manifest app.rc)
set_property(TARGET EarbudControls.UI APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Dusk.xaml")
target_include_directories(EarbudControls.UI BEFORE PRIVATE "${CMAKE_BINARY_DIR}/generated-ui"
  "${EARBUD_UI_SDK}/microsoft.windowsappsdk.winui/include" "${EARBUD_UI_SDK}/microsoft.windowsappsdk.interactiveexperiences/include"
  "${EARBUD_UI_SDK}/microsoft.windowsappsdk.foundation/include" "${EARBUD_UI_SDK}/microsoft.windowsappsdk.runtime/include")
target_compile_definitions(EarbudControls.UI PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(EarbudControls.UI PRIVATE /W4 /utf-8 /bigobj /EHsc)
# The bootstrapper is only used by unpackaged local runs, so the installed app never loads it.
target_link_libraries(EarbudControls.UI PRIVATE windowsapp comctl32 ksuser d3d11 d3dcompiler d2d1 dxgi dxguid delayimp crypt32 wintrust bcrypt shell32
  "${EARBUD_UI_SDK}/microsoft.windowsappsdk.foundation/lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib")
target_link_options(EarbudControls.UI PRIVATE "/DELAYLOAD:Microsoft.WindowsAppRuntime.Bootstrap.dll")
set_target_properties(EarbudControls.UI PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_custom_command(TARGET EarbudControls.UI POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EARBUD_UI_SDK}/microsoft.windowsappsdk.foundation/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll" "$<TARGET_FILE_DIR:EarbudControls.UI>"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:EarbudControls.UI>/Assets"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Dusk.xaml" "$<TARGET_FILE_DIR:EarbudControls.UI>/Dusk.xaml")

if(BUILD_TESTING)
  add_executable(earbud-ui-model-tests model_tests.cpp)
  target_compile_options(earbud-ui-model-tests PRIVATE /W4 /utf-8 /EHsc)
  add_test(NAME earbud-ui-model COMMAND earbud-ui-model-tests)
endif()
