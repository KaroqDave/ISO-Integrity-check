cmake_minimum_required(VERSION 3.21)

if(WIN32)
    project(ISOIntegrityCheck VERSION 1.1.0 LANGUAGES CXX RC)
else()
    project(ISOIntegrityCheck VERSION 1.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Test)
get_filename_component(QT_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

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

if(WIN32)
    target_link_libraries(iso-integrity-check-cli PRIVATE bcrypt)
endif()

enable_testing()

add_executable(iso-core-tests
    ${ISO_CORE_SOURCES}
    tests/test_core.cpp
)
target_include_directories(iso-core-tests PRIVATE src)
target_link_libraries(iso-core-tests PRIVATE Qt6::Core Qt6::Test)

if(WIN32)
    target_link_libraries(iso-core-tests PRIVATE bcrypt)
endif()

add_test(NAME iso-core-tests COMMAND iso-core-tests)
if(WIN32)
    set_tests_properties(iso-core-tests PROPERTIES
        ENVIRONMENT "PATH=${QT_INSTALL_PREFIX}/bin;$ENV{PATH}")
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
