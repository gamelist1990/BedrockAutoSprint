cmake_minimum_required(VERSION 3.24)
project(BedrockAutoSprint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch the SDK include/ and implementation from GitHub at CONFIGURE/BUILD time.
# No SDK files are copied into this standalone client project's source tree.
include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING
    "Git tag or commit of BedrockDynamicSDK to use when building the client")
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL
    "SDK diagnostic/test targets are never needed by a sample client" FORCE)

FetchContent_Declare(
    bedrock_sdk
    GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(bedrock_sdk)

# Standalone DLL: linked SDK implementation is STATIC. Only Windows system
# dependencies (WinHTTP, BCrypt and standard system DLLs) remain.
if(WIN32)
    add_library(bedrock_autosprint_minhook STATIC
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src/buffer.c
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src/hook.c
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src/trampoline.c
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src/hde/hde64.c
    )
    target_include_directories(bedrock_autosprint_minhook PUBLIC
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/include)
    target_include_directories(bedrock_autosprint_minhook PRIVATE
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src
        ${bedrock_sdk_SOURCE_DIR}/third_party/minhook-vendor/src/hde)

    add_library(BedrockAutoSprint SHARED src/auto_sprint.cpp src/win32_client.cpp)
    target_include_directories(BedrockAutoSprint PRIVATE include)
    option(BEDROCK_AUTOSPRINT_DEBUG_CONSOLE
        "Open optional Win32 console to show debug logs" OFF)
    target_link_libraries(BedrockAutoSprint PRIVATE
        bedrock::events bedrock::symbols bedrock::logging bedrock_autosprint_minhook)
    if(BEDROCK_AUTOSPRINT_DEBUG_CONSOLE)
        target_compile_definitions(BedrockAutoSprint PRIVATE
            BEDROCK_AUTOSPRINT_DEBUG_CONSOLE=1)
    endif()
    target_compile_features(BedrockAutoSprint PRIVATE cxx_std_20)
    if(MSVC)
        foreach(target bedrock_autosprint_minhook BedrockAutoSprint)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(auto_sprint_test tests/auto_sprint_test.cpp src/auto_sprint.cpp)
    target_include_directories(auto_sprint_test PRIVATE include)
    target_link_libraries(auto_sprint_test PRIVATE bedrock::events)
    target_compile_features(auto_sprint_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(auto_sprint_test PRIVATE /UNDEBUG)
        set_property(TARGET auto_sprint_test PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(auto_sprint_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME auto_sprint_test COMMAND auto_sprint_test)
endif()
