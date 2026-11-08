cmake_minimum_required(VERSION 3.20)
project(NidraIA LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Biblioteca do Visual C++ embutida no .exe: roda em qualquer Windows sem instalar nada
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(NidraIA src/nidraia.cpp)
target_include_directories(NidraIA PRIVATE third_party)
target_compile_definitions(NidraIA PRIVATE _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
target_compile_options(NidraIA PRIVATE /utf-8 /W4 /permissive- /EHsc /bigobj)
target_link_libraries(NidraIA PRIVATE ws2_32 shell32)

# Copia o .exe para a raiz do projeto, ao lado das pastas motor/, modelos/ e public/
add_custom_command(TARGET NidraIA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:NidraIA> ${CMAKE_SOURCE_DIR}/NidraIA.exe)
