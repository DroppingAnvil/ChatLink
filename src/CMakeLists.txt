# ChatLink - link a TI-Nspire CX to a PC over USB.
# Copyright (C) 2026 Christopher Willett / AnvilDevelopment.US
# SPDX-License-Identifier: GPL-3.0-or-later

# os_input is deliberately free of any calculator or USB dependency: it is the
# reusable Win32 input layer that both the model bridge and macropad mode use.
add_library(chatlink_os_input STATIC os_input/os_input.cpp)
target_include_directories(chatlink_os_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatlink_os_input PUBLIC user32)

# The message framing is pure logic with no USB dependency, so it is its own
# target and is testable without hardware attached.
add_library(chatlink_protocol STATIC link/protocol.cpp)
target_include_directories(chatlink_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The model backend. WinHTTP ships with the toolchain, so calling the Claude
# API adds no third-party dependency.
add_library(chatlink_model STATIC model/anthropic.cpp)
target_include_directories(chatlink_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatlink_model PUBLIC winhttp)

# The USB link layer: a C++ wrapper over libnspire's NavNet implementation.
add_library(chatlink_link STATIC link/nspire_link.cpp)
target_include_directories(chatlink_link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chatlink_link PUBLIC nspire chatlink_protocol)

add_executable(chatlink_tests tests/protocol_test.cpp)
target_link_libraries(chatlink_tests PRIVATE chatlink_protocol chatlink_os_input)

add_executable(ChatLink main.cpp)
target_link_libraries(ChatLink PRIVATE chatlink_link chatlink_os_input chatlink_model)

# libnspire is a DLL, so it has to sit next to the executable to run from the
# build tree.
add_custom_command(TARGET ChatLink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nspire> $<TARGET_FILE_DIR:ChatLink>
        COMMENT "Staging libnspire next to ChatLink")
