cmake_minimum_required(VERSION 3.20)
project(vst3-to-clap VERSION 1.0.1 LANGUAGES C CXX)

if(MSVC)
    # CLAP hosts do not deploy the Visual C++ runtime for plug-ins. Statically link
    # it so released wrappers load on Windows machines without the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
    enable_language(OBJCXX)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    endif()
endif()

if(WIN32)
    # windows.h defines min/max as macros, which breaks std::max and friends.
    # Kept to NOMINMAX alone: WIN32_LEAN_AND_MEAN would also apply to the
    # vendored VST3 hosting sources compiled below.
    add_compile_definitions(NOMINMAX)
endif()

option(V3C_BUILD_TESTS "Build the test host" ON)

# ---------------------------------------------------------------------------
# SDKs: use local checkouts if given/present, otherwise fetch them.
# ---------------------------------------------------------------------------
set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk" CACHE PATH "VST3 SDK root")
set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/clap" CACHE PATH "CLAP SDK root")

include(FetchContent)
if(NOT EXISTS "${VST3_SDK_ROOT}/pluginterfaces")
    FetchContent_Declare(vst3sdk_src
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        GIT_SUBMODULES base pluginterfaces public.sdk cmake)
    FetchContent_Populate(vst3sdk_src)
    set(VST3_SDK_ROOT "${vst3sdk_src_SOURCE_DIR}")
endif()
if(NOT EXISTS "${CLAP_SDK_ROOT}/include/clap/clap.h")
    FetchContent_Declare(clap_src
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG main
        GIT_SHALLOW TRUE)
    FetchContent_Populate(clap_src)
    set(CLAP_SDK_ROOT "${clap_src_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# The subset of the VST3 SDK needed to host plug-ins
# ---------------------------------------------------------------------------
set(SDK ${VST3_SDK_ROOT})
set(VST3_HOSTING_SOURCES
    ${SDK}/pluginterfaces/base/coreiids.cpp
    ${SDK}/pluginterfaces/base/funknown.cpp
    ${SDK}/pluginterfaces/base/ustring.cpp
    ${SDK}/pluginterfaces/base/conststringtable.cpp
    ${SDK}/public.sdk/source/common/commoniids.cpp
    ${SDK}/public.sdk/source/common/memorystream.cpp
    ${SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${SDK}/public.sdk/source/vst/hosting/module.cpp
    ${SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${SDK}/public.sdk/source/vst/hosting/eventlist.cpp
    ${SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${SDK}/public.sdk/source/vst/moduleinfo/moduleinfoparser.cpp
)
if(WIN32)
    list(APPEND VST3_HOSTING_SOURCES ${SDK}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(APPLE)
    list(APPEND VST3_HOSTING_SOURCES ${SDK}/public.sdk/source/vst/hosting/module_mac.mm)
    set_source_files_properties(${SDK}/public.sdk/source/vst/hosting/module_mac.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    list(APPEND VST3_HOSTING_SOURCES ${SDK}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

add_library(vst3_hosting STATIC ${VST3_HOSTING_SOURCES})
target_include_directories(vst3_hosting PUBLIC ${SDK})
target_compile_definitions(vst3_hosting PUBLIC
    $<$<CONFIG:Debug>:DEVELOPMENT=1>
    $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>)
if(WIN32)
    target_compile_definitions(vst3_hosting PUBLIC NOMINMAX UNICODE _UNICODE)
    target_link_libraries(vst3_hosting PUBLIC ole32 shell32)
elseif(APPLE)
    target_link_libraries(vst3_hosting PUBLIC "-framework CoreFoundation" "-framework Foundation")
else()
    target_link_libraries(vst3_hosting PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()
if(MSVC)
    target_compile_options(vst3_hosting PUBLIC /utf-8 /Zc:__cplusplus)
endif()

# ---------------------------------------------------------------------------
# The wrapper: a CLAP plug-in that loads VST3 plug-ins
# ---------------------------------------------------------------------------
add_library(vst3-to-clap-wrapper MODULE
    src/entry.cpp
    src/registry.cpp
    src/plugin.cpp
    src/runloop.cpp
)
target_include_directories(vst3-to-clap-wrapper PRIVATE ${CLAP_SDK_ROOT}/include src)
target_link_libraries(vst3-to-clap-wrapper PRIVATE vst3_hosting)

if(APPLE)
    set_target_properties(vst3-to-clap-wrapper PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION clap
        MACOSX_BUNDLE_GUI_IDENTIFIER org.vst3-to-clap.wrapper
        MACOSX_BUNDLE_BUNDLE_NAME vst3-to-clap-wrapper
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    target_link_options(vst3-to-clap-wrapper PRIVATE
        "-Wl,-exported_symbol,_clap_entry")
else()
    set_target_properties(vst3-to-clap-wrapper PROPERTIES PREFIX "" SUFFIX ".clap")
endif()
if(UNIX AND NOT APPLE)
    # keep libstdc++ internal so the wrapper doesn't clash with the host or the wrapped plug-in
    target_link_options(vst3-to-clap-wrapper PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc
        -Wl,--exclude-libs,ALL -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map)
endif()

if(MINGW)
    # self-contained DLL/EXE: no libstdc++/libgcc/winpthread DLLs next to the plug-in
    target_link_options(vst3-to-clap-wrapper PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# The converter command line tool
# ---------------------------------------------------------------------------
add_executable(vst3-to-clap tools/vst3-to-clap.cpp)
target_link_libraries(vst3-to-clap PRIVATE vst3_hosting)
add_dependencies(vst3-to-clap vst3-to-clap-wrapper)
if(MINGW)
    target_link_options(vst3-to-clap PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Test host
# ---------------------------------------------------------------------------
if(V3C_BUILD_TESTS)
    add_executable(clap-test-host tests/clap-test-host.cpp)
    target_include_directories(clap-test-host PRIVATE ${CLAP_SDK_ROOT}/include)
    target_link_libraries(clap-test-host PRIVATE ${CMAKE_DL_LIBS})
    if(UNIX AND NOT APPLE)
        find_package(X11)
        if(X11_FOUND)
            add_executable(clap-gui-test tests/clap-gui-test.cpp)
            target_include_directories(clap-gui-test PRIVATE ${CLAP_SDK_ROOT}/include ${X11_INCLUDE_DIR})
            target_link_libraries(clap-gui-test PRIVATE ${X11_LIBRARIES} ${X11_XTest_LIB} ${CMAKE_DL_LIBS})
        endif()
    endif()
endif()

install(TARGETS vst3-to-clap RUNTIME DESTINATION bin)
install(TARGETS vst3-to-clap-wrapper LIBRARY DESTINATION bin BUNDLE DESTINATION bin)
