cmake_minimum_required(VERSION 3.15)
project(GodBytePP VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gbpp_lib
    frontend/lexer.cpp
    frontend/parser.cpp
    frontend/sema.cpp
    frontend/irgen.cpp
    backend/regalloc.cpp
    backend/codegen_win64.cpp
    backend/codegen_sysv.cpp
    backend/codegen_arm64.cpp
    backend/factory.cpp
    backend/x86_encoder.cpp
    backend/emitter_asm.cpp
    backend/emitter_coff.cpp
    backend/emitter_elf.cpp
)

target_include_directories(gbpp_lib PUBLIC include)

if(MSVC)
    target_compile_options(gbpp_lib PRIVATE /W4)
else()
    target_compile_options(gbpp_lib PRIVATE -Wall -Wextra)
endif()

add_executable(divo
    divo/main.cpp
)
target_link_libraries(divo PRIVATE gbpp_lib)

add_executable(gbpp-lsp
    lsp/main.cpp
    lsp/server.cpp
)
target_link_libraries(gbpp-lsp PRIVATE gbpp_lib)

if(MSVC)
    target_compile_options(divo PRIVATE /W4)
else()
    target_compile_options(divo PRIVATE -Wall -Wextra)
endif()