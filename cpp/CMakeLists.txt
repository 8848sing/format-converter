cmake_minimum_required(VERSION 3.20)

# 第三方库的年代较早，CMake 4 需要放宽策略版本下限。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(format_converter_core VERSION 1.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FC_BUILD_CLI "Build the fccli command line tool" ON)
option(FC_BUILD_SHARED "Build the fccore shared library with a C API" ON)
option(FC_BUILD_TESTS "Build the native unit tests" OFF)
option(FC_WITH_PDFIUM "Enable PDF rendering/manipulation through PDFium" ON)
option(FC_WITH_WEBP "Enable WebP encode/decode through libwebp" ON)
option(FC_WITH_YAML "Enable YAML conversion through yaml-cpp" ON)

set(FC_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH "Third party source directory")
set(FC_PDFIUM_DIR "" CACHE PATH "PDFium package directory (contains include/ and bin/ or lib/)")

# ---------------------------------------------------------------------------
# Third party: single-file dependencies
# ---------------------------------------------------------------------------
add_library(fc_thirdparty STATIC
    "${FC_THIRD_PARTY}/miniz/miniz.c"
    "${FC_THIRD_PARTY}/tinyxml2/tinyxml2.cpp"
)
target_include_directories(fc_thirdparty PUBLIC
    "${FC_THIRD_PARTY}"
    "${FC_THIRD_PARTY}/stb"
    "${FC_THIRD_PARTY}/nlohmann"
    "${FC_THIRD_PARTY}/miniz"
    "${FC_THIRD_PARTY}/tinyxml2"
    "${FC_THIRD_PARTY}/gif"
)
if(MSVC)
    target_compile_options(fc_thirdparty PRIVATE /W0)
else()
    target_compile_options(fc_thirdparty PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Third party: yaml-cpp
# ---------------------------------------------------------------------------
set(FC_HAVE_YAML 0)
if(FC_WITH_YAML AND EXISTS "${FC_THIRD_PARTY}/yaml-cpp/CMakeLists.txt")
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${FC_THIRD_PARTY}/yaml-cpp" "${CMAKE_BINARY_DIR}/yaml-cpp" EXCLUDE_FROM_ALL)
    set(FC_HAVE_YAML 1)
    message(STATUS "yaml-cpp: enabled")
else()
    message(STATUS "yaml-cpp: disabled (third_party/yaml-cpp not found)")
endif()

# ---------------------------------------------------------------------------
# Third party: libwebp
# ---------------------------------------------------------------------------
set(FC_HAVE_WEBP 0)
if(FC_WITH_WEBP AND EXISTS "${FC_THIRD_PARTY}/libwebp/CMakeLists.txt")
    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${FC_THIRD_PARTY}/libwebp" "${CMAKE_BINARY_DIR}/libwebp" EXCLUDE_FROM_ALL)
    set(FC_HAVE_WEBP 1)
    message(STATUS "libwebp: enabled")
else()
    message(STATUS "libwebp: disabled (third_party/libwebp not found)")
endif()

# ---------------------------------------------------------------------------
# Third party: PDFium
# ---------------------------------------------------------------------------
set(FC_HAVE_PDFIUM 0)
set(FC_PDFIUM_DLL "")

if(FC_WITH_PDFIUM)
    if(NOT FC_PDFIUM_DIR)
        if(ANDROID)
            set(_candidate "${FC_THIRD_PARTY}/pdfium-android-${ANDROID_ABI}")
        elseif(WIN32)
            set(_candidate "${FC_THIRD_PARTY}/pdfium-win")
        else()
            set(_candidate "${FC_THIRD_PARTY}/pdfium")
        endif()
        if(EXISTS "${_candidate}/include/fpdfview.h")
            set(FC_PDFIUM_DIR "${_candidate}")
        endif()
    endif()

    if(FC_PDFIUM_DIR AND EXISTS "${FC_PDFIUM_DIR}/include/fpdfview.h")
        set(FC_HAVE_PDFIUM 1)
        message(STATUS "PDFium: ${FC_PDFIUM_DIR}")
        if(WIN32)
            set(FC_PDFIUM_DLL "${FC_PDFIUM_DIR}/bin/pdfium.dll")
        endif()
    else()
        message(STATUS "PDFium: disabled (package not found)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(FC_CORE_SOURCES
    src/common.cpp
    src/fs_utils.cpp
    src/process.cpp
    src/image.cpp
    src/tiff_decode.cpp
    src/pdf_writer.cpp
    src/pdf_ops.cpp
    src/pdfium_api.cpp
    src/office.cpp
    src/media.cpp
    src/data.cpp
    src/zip_writer.cpp
    src/xlsx.cpp
    src/docx.cpp
    src/docx_text.cpp
    src/pptx.cpp
    src/registry.cpp
    src/engine.cpp
    src/core_api.cpp
)

if(WIN32)
    list(APPEND FC_CORE_SOURCES src/image_codec_win.cpp)
elseif(ANDROID)
    # Android 没有 WIC：通过 JNI 调用 BitmapFactory / ImageDecoder 解码。
    list(APPEND FC_CORE_SOURCES src/image_codec_android.cpp)
else()
    list(APPEND FC_CORE_SOURCES src/image_codec_stub.cpp)
endif()

# MP3 编码（shine，LGPL-2.0-only）：Android 系统没有 MP3 编码器，桌面端走 FFmpeg，所以只在 Android 编。
if(ANDROID)
    list(APPEND FC_CORE_SOURCES src/mp3_encoder.cpp)
    foreach(_shine_src bitstream.c huffman.c l3bitstream.c l3loop.c l3mdct.c
            l3subband.c layer3.c reservoir.c tables.c)
        list(APPEND FC_CORE_SOURCES "${FC_THIRD_PARTY}/shine/${_shine_src}")
    endforeach()
endif()

add_library(fc_core STATIC ${FC_CORE_SOURCES})
target_include_directories(fc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(ANDROID)
    target_include_directories(fc_core PUBLIC "${FC_THIRD_PARTY}/shine")
endif()
target_link_libraries(fc_core PUBLIC fc_thirdparty)
if(FC_HAVE_YAML)
    target_link_libraries(fc_core PUBLIC yaml-cpp)
    target_compile_definitions(fc_core PUBLIC FC_HAVE_YAML=1)
else()
    target_compile_definitions(fc_core PUBLIC FC_HAVE_YAML=0)
endif()
if(FC_HAVE_WEBP)
    target_link_libraries(fc_core PUBLIC webp)
    target_compile_definitions(fc_core PUBLIC FC_HAVE_WEBP=1)
else()
    target_compile_definitions(fc_core PUBLIC FC_HAVE_WEBP=0)
endif()
if(FC_HAVE_PDFIUM)
    target_compile_definitions(fc_core PUBLIC FC_HAVE_PDFIUM=1)
    target_include_directories(fc_core PUBLIC "${FC_PDFIUM_DIR}/include" "${FC_PDFIUM_DIR}/include/cpp")
    if(NOT WIN32)
        # 直接按路径取库，避免 Android toolchain 的 find_root_path 限制。
        set(FC_PDFIUM_LIBRARY "")
        foreach(_pdfium_name libpdfium.so libpdfium.a)
            if(EXISTS "${FC_PDFIUM_DIR}/lib/${_pdfium_name}")
                set(FC_PDFIUM_LIBRARY "${FC_PDFIUM_DIR}/lib/${_pdfium_name}")
                break()
            endif()
        endforeach()
        if(NOT FC_PDFIUM_LIBRARY)
            message(FATAL_ERROR "PDFium library not found under ${FC_PDFIUM_DIR}/lib")
        endif()
        target_link_libraries(fc_core PUBLIC "${FC_PDFIUM_LIBRARY}")
    endif()
else()
    target_compile_definitions(fc_core PUBLIC FC_HAVE_PDFIUM=0)
endif()

target_compile_definitions(fc_core PUBLIC FC_CORE_STATIC=1)

if(WIN32)
    target_compile_definitions(fc_core PUBLIC FC_OS_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(fc_core PUBLIC ole32 windowscodecs shell32)
else()
    target_compile_definitions(fc_core PUBLIC FC_OS_POSIX=1)
    if(NOT ANDROID)
        target_link_libraries(fc_core PUBLIC pthread)
    endif()
endif()

if(MSVC)
    target_compile_options(fc_core PRIVATE /utf-8 /W3)
else()
    target_compile_options(fc_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
endif()

# ---------------------------------------------------------------------------
# C API shared library
# ---------------------------------------------------------------------------
if(FC_BUILD_SHARED)
    add_library(fccore SHARED src/core_api.cpp)
    target_link_libraries(fccore PRIVATE fc_core)
    target_compile_definitions(fccore PRIVATE FC_CORE_EXPORTS=1)
    target_include_directories(fccore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(fccore PROPERTIES OUTPUT_NAME "fccore")
    if(WIN32 AND FC_PDFIUM_DLL)
        add_custom_command(TARGET fccore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FC_PDFIUM_DLL}" "$<TARGET_FILE_DIR:fccore>/pdfium.dll")
    endif()
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(FC_BUILD_CLI)
    add_executable(fccli src/cli_main.cpp)
    target_link_libraries(fccli PRIVATE fc_core)
    if(MINGW)
        # Avoid shipping libstdc++/libgcc/libwinpthread DLLs next to the CLI.
        target_link_options(fccli PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    if(WIN32 AND FC_PDFIUM_DLL)
        add_custom_command(TARGET fccli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FC_PDFIUM_DLL}" "$<TARGET_FILE_DIR:fccli>/pdfium.dll")
    endif()
    if(WIN32)
        set(FC_PORTABLE_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
        add_custom_command(TARGET fccli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${FC_PORTABLE_BIN}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:fccli>" "${FC_PORTABLE_BIN}/fccli.exe")
        if(FC_PDFIUM_DLL)
            add_custom_command(TARGET fccli POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FC_PDFIUM_DLL}" "${FC_PORTABLE_BIN}/pdfium.dll")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Android JNI bridge
# ---------------------------------------------------------------------------
if(ANDROID)
    add_library(fccore_jni SHARED android/fc_jni.cpp)
    target_link_libraries(fccore_jni PRIVATE fc_core log)
    target_compile_definitions(fccore_jni PRIVATE FC_CORE_STATIC=1)
    set_target_properties(fccore_jni PROPERTIES OUTPUT_NAME "fccore_jni")
endif()

# ---------------------------------------------------------------------------
# Native tests
# ---------------------------------------------------------------------------
if(FC_BUILD_TESTS)
    enable_testing()
    add_executable(fc_tests tests/test_core.cpp)
    target_link_libraries(fc_tests PRIVATE fc_core)
    add_test(NAME fc_tests COMMAND fc_tests)
endif()