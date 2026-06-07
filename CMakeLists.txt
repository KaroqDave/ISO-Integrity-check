cmake_minimum_required(VERSION 3.21)

if(WIN32)
    project(ISOIntegrityCheck VERSION 1.1.0 LANGUAGES CXX RC)
else()
    project(ISOIntegrityCheck VERSION 1.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
get_filename_component(QT_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

set(ISO_INTEGRITY_CHECK_SOURCES
    src/core/checksum.cpp
    src/core/checksum.h
    src/core/verifier.cpp
    src/core/verifier.h
    src/gui/main.cpp
    src/gui/main_window.cpp
    src/gui/main_window.h
    src/gui/theme.cpp
    src/gui/theme.h
    resources/resources.qrc
)

if(WIN32)
    add_executable(iso-integrity-check WIN32 ${ISO_INTEGRITY_CHECK_SOURCES})
else()
    add_executable(iso-integrity-check ${ISO_INTEGRITY_CHECK_SOURCES})
endif()

target_include_directories(iso-integrity-check PRIVATE src)
target_link_libraries(iso-integrity-check PRIVATE Qt6::Core Qt6::Widgets)

if(WIN32)
    # Hardware-accelerated hashing via the Windows CNG (BCrypt) API.
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

# Install rules used by scripts/build-standalone.ps1 (Windows) and
# scripts/build-appimage.sh (Linux) via `cmake --install`.
if(WIN32)
    install(TARGETS iso-integrity-check RUNTIME DESTINATION ".")
    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "
            execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --no-translations --no-compiler-runtime
                \"\${CMAKE_INSTALL_PREFIX}/iso-integrity-check.exe\")
        ")
    endif()
else()
    install(TARGETS iso-integrity-check RUNTIME DESTINATION bin)
    install(FILES resources/iso-integrity-check.desktop DESTINATION share/applications)
    install(FILES resources/icons/app.svg DESTINATION share/icons/hicolor/scalable/apps
            RENAME iso-integrity-check.svg)
endif()
