cmake_minimum_required(VERSION 3.21)

project(ISOIntegrityCheck VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
get_filename_component(QT_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

add_executable(iso-integrity-check WIN32
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
target_include_directories(iso-integrity-check PRIVATE src)
target_link_libraries(iso-integrity-check PRIVATE Qt6::Core Qt6::Widgets)

if(WIN32)
    # Hardware-accelerated hashing via the Windows CNG (BCrypt) API.
    target_link_libraries(iso-integrity-check PRIVATE bcrypt)
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

# Install rules: place the executable at the install root and deploy the Qt
# runtime next to it, producing a clean, self-contained bundle. Used by
# scripts/build-standalone.ps1 to populate the standalone export folder via
# `cmake --install <build> --config Release --prefix <dir>`.
install(TARGETS iso-integrity-check RUNTIME DESTINATION ".")
if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
    install(CODE "
        execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
            --no-translations --no-compiler-runtime
            \"\${CMAKE_INSTALL_PREFIX}/iso-integrity-check.exe\")
    ")
endif()
