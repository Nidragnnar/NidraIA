cmake_minimum_required(VERSION 3.20)
project(GeradorImagens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Biblioteca do Visual C++ embutida no .exe: roda em qualquer Windows sem instalar nada
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(GeradorImagens src/gerador.cpp)
target_include_directories(GeradorImagens PRIVATE third_party)
target_compile_definitions(GeradorImagens PRIVATE _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
target_compile_options(GeradorImagens PRIVATE /utf-8 /W4 /permissive- /EHsc /bigobj)
target_link_libraries(GeradorImagens PRIVATE ws2_32 shell32)

# Copia o .exe para a raiz do projeto, ao lado das pastas motor/, modelos/ e public/
add_custom_command(TARGET GeradorImagens POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:GeradorImagens> ${CMAKE_SOURCE_DIR}/GeradorImagens.exe)
