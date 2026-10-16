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
# dependencies (WinHTTP, BCrypt, User32 and standard system DLLs) remain.
if(WIN32)
    add_library(BedrockAutoSprint SHARED src/auto_sprint.cpp src/win32_client.cpp)
    target_include_directories(BedrockAutoSprint PRIVATE include)
    target_link_libraries(BedrockAutoSprint PRIVATE bedrock::events bedrock::symbols user32)
    target_compile_features(BedrockAutoSprint PRIVATE cxx_std_20)
    if(MSVC)
        set_property(TARGET BedrockAutoSprint PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
