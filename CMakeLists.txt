cmake_minimum_required(VERSION 3.21)

if(WIN32)
    project(ISOIntegrityCheck VERSION 1.1.1 LANGUAGES CXX RC)
else()
    project(ISOIntegrityCheck VERSION 1.1.1 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(ISO_BUILD_TESTS "Build ISO Integrity Check C++ tests" ON)
if(WIN32)
    set(_iso_use_openssl_default OFF)
else()
    set(_iso_use_openssl_default ON)
endif()
option(ISO_USE_OPENSSL "Use OpenSSL/libcrypto for native hashing when available" ${_iso_use_openssl_default})

# Fallback Qt discovery: when configuring without a preset or CMAKE_PREFIX_PATH
# (e.g. straight from an IDE), auto-locate a complete Qt MSVC kit installed under
# C:/Qt so find_package(Qt6) works out of the box. Skipped if Qt is already
# pointed at via Qt6_DIR, CMAKE_PREFIX_PATH, or the matching environment variable.
if(WIN32 AND NOT Qt6_DIR AND NOT CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
    file(GLOB _iso_qt_configs
        "C:/Qt/*/msvc*_64/lib/cmake/Qt6/Qt6Config.cmake")
    if(_iso_qt_configs)
        list(SORT _iso_qt_configs COMPARE NATURAL)
        list(LENGTH _iso_qt_configs _iso_qt_count)
        math(EXPR _iso_qt_last "${_iso_qt_count} - 1")
        list(GET _iso_qt_configs ${_iso_qt_last} _iso_qt_config)
        get_filename_component(_iso_qt_cmake_dir "${_iso_qt_config}" DIRECTORY)
        get_filename_component(_iso_qt_prefix "${_iso_qt_cmake_dir}/../../.." ABSOLUTE)
        list(APPEND CMAKE_PREFIX_PATH "${_iso_qt_prefix}")
        message(STATUS "Auto-detected Qt at ${_iso_qt_prefix}")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
if(ISO_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
get_filename_component(QT_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

if(ISO_USE_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        set(ISO_HAS_OPENSSL ON)
        message(STATUS "Using OpenSSL for native hashing")
    else()
        message(STATUS "OpenSSL not found; falling back to Qt hashing")
    endif()
endif()

set(ISO_CORE_SOURCES
    src/core/checksum.cpp
    src/core/checksum.h
    src/core/verifier.cpp
    src/core/verifier.h
)

set(ISO_GUI_SOURCES
    src/gui/app_settings.cpp
    src/gui/app_settings.h
    src/gui/main.cpp
    src/gui/main_window.cpp
    src/gui/main_window.h
    src/gui/theme.cpp
    src/gui/theme.h
    src/gui/verification_controller.cpp
    src/gui/verification_controller.h
    resources/resources.qrc
)

if(WIN32)
    add_executable(iso-integrity-check WIN32 ${ISO_CORE_SOURCES} ${ISO_GUI_SOURCES})
else()
    add_executable(iso-integrity-check ${ISO_CORE_SOURCES} ${ISO_GUI_SOURCES})
endif()

target_include_directories(iso-integrity-check PRIVATE src)
target_link_libraries(iso-integrity-check PRIVATE Qt6::Core Qt6::Widgets)
target_compile_definitions(iso-integrity-check PRIVATE ISO_APP_VERSION="${PROJECT_VERSION}")
if(ISO_HAS_OPENSSL)
    target_link_libraries(iso-integrity-check PRIVATE OpenSSL::Crypto)
    target_compile_definitions(iso-integrity-check PRIVATE ISO_HAS_OPENSSL)
endif()

if(WIN32)
    target_link_libraries(iso-integrity-check PRIVATE bcrypt)
    target_sources(iso-integrity-check PRIVATE resources/app.rc)
endif()

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_INSTALL_PREFIX}/bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(
            TARGET iso-integrity-check
            POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:iso-integrity-check>"
            COMMENT "Deploying Qt runtime files"
        )
    endif()
endif()

add_executable(iso-integrity-check-cli ${ISO_CORE_SOURCES} src/cli/main.cpp)
target_include_directories(iso-integrity-check-cli PRIVATE src)
target_link_libraries(iso-integrity-check-cli PRIVATE Qt6::Core)
target_compile_definitions(iso-integrity-check-cli PRIVATE ISO_APP_VERSION="${PROJECT_VERSION}")
if(ISO_HAS_OPENSSL)
    target_link_libraries(iso-integrity-check-cli PRIVATE OpenSSL::Crypto)
    target_compile_definitions(iso-integrity-check-cli PRIVATE ISO_HAS_OPENSSL)
endif()

if(WIN32)
    target_link_libraries(iso-integrity-check-cli PRIVATE bcrypt)
endif()

if(ISO_BUILD_TESTS)
    enable_testing()

    add_executable(iso-core-tests
        ${ISO_CORE_SOURCES}
        tests/test_core.cpp
    )
    target_include_directories(iso-core-tests PRIVATE src)
    target_link_libraries(iso-core-tests PRIVATE Qt6::Core Qt6::Test)
    if(ISO_HAS_OPENSSL)
        target_link_libraries(iso-core-tests PRIVATE OpenSSL::Crypto)
        target_compile_definitions(iso-core-tests PRIVATE ISO_HAS_OPENSSL)
    endif()

    if(WIN32)
        target_link_libraries(iso-core-tests PRIVATE bcrypt)
    endif()

    add_test(NAME iso-core-tests COMMAND iso-core-tests)
    if(WIN32)
        set_tests_properties(iso-core-tests PROPERTIES
            ENVIRONMENT "PATH=${QT_INSTALL_PREFIX}/bin;$ENV{PATH}")
    endif()
endif()

if(WIN32)
    install(TARGETS iso-integrity-check RUNTIME DESTINATION ".")
    install(TARGETS iso-integrity-check-cli RUNTIME DESTINATION ".")
    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "
            execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --no-translations --no-compiler-runtime
                \"\${CMAKE_INSTALL_PREFIX}/iso-integrity-check.exe\")
        ")
    endif()
else()
    install(TARGETS iso-integrity-check iso-integrity-check-cli RUNTIME DESTINATION bin)
    install(FILES resources/iso-integrity-check.desktop DESTINATION share/applications)
    install(FILES resources/icons/app.svg DESTINATION share/icons/hicolor/scalable/apps
            RENAME iso-integrity-check.svg)
endif()
