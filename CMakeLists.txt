cmake_minimum_required(VERSION 3.16)

project(ScadaKit
    VERSION 1.0.0
    DESCRIPTION "Small SCADA gateway: Modbus master/slave, tag engine, alarms, web HMI"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SCADAKIT_BUILD_TESTS "Build the test suite" ON)
option(SCADAKIT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

function(scadakit_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(SCADAKIT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(SCADAKIT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ------------------------------------------------------------------- core ----
add_library(scada_core
    src/alarm.cpp
    src/dashboard.cpp
    src/gateway.cpp
    src/historian.cpp
    src/http.cpp
    src/master.cpp
    src/modbus.cpp
    src/plant.cpp
    src/rtdb.cpp
    src/slave.cpp
    src/socket.cpp
    src/tag.cpp
    src/types.cpp)

target_include_directories(scada_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(scada_core PUBLIC Threads::Threads)
if(WIN32)
    # Winsock lives in ws2_32; MSVC links it automatically with a pragma but
    # MinGW does not.
    target_link_libraries(scada_core PUBLIC ws2_32)
endif()

scadakit_enable_warnings(scada_core)

# --------------------------------------------------------------- executable ----
add_executable(scadakit src/main.cpp)
target_link_libraries(scadakit PRIVATE scada_core)
scadakit_enable_warnings(scadakit)

# -------------------------------------------------------------------- tests ----
include(CTest)
if(SCADAKIT_BUILD_TESTS AND BUILD_TESTING)
    add_executable(scadakit_tests
        tests/test_main.cpp
        tests/test_alarm.cpp
        tests/test_gateway.cpp
        tests/test_http.cpp
        tests/test_modbus.cpp
        tests/test_tag.cpp)

    target_link_libraries(scadakit_tests PRIVATE scada_core)
    target_include_directories(scadakit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    scadakit_enable_warnings(scadakit_tests)

    add_test(NAME scadakit.unit COMMAND scadakit_tests)
    add_test(NAME scadakit.cli.help COMMAND scadakit --help)
    add_test(NAME scadakit.cli.version COMMAND scadakit --version)
endif()

# ------------------------------------------------------------------ install ----
install(TARGETS scadakit RUNTIME DESTINATION bin)
install(DIRECTORY include/scada DESTINATION include)
install(DIRECTORY config/ DESTINATION share/scadakit/config)
