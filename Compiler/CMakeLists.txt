cmake_minimum_required(VERSION 3.16)
project(cxxe VERSION 0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(cxxe
    Src/Cli/Cli.c
    Src/Cli/Main.c
    Src/CodeGen/Decorators/EmitDecorators.c
    Src/CodeGen/EmitCalls.c
    Src/CodeGen/EmitMatch.c
    Src/CodeGen/EmitRegistry.c
    Src/CodeGen/EmitSource.c
    Src/CodeGen/Emitter.c
    Src/CodeGen/Properties/EmitProperties.c
    Src/Core/FileIo.c
    Src/Core/Memory.c
    Src/Core/Path.c
    Src/Core/StrBuf.c
    Src/Driver/Pipeline.c
    Src/Frontend/IncludeResolver.c
    Src/Ir/Ir.c
    Src/Ir/IrDump.c
    Src/Ir/IrSerialize.c
    Src/Lexer/Lexer.c
    Src/Parser/ParseClasses.c
    Src/Parser/ParseCustomDecorators.c
    Src/Parser/ParseDeclarations.c
    Src/Parser/ParseDecorators.c
    Src/Parser/ParseExpressions.c
    Src/Parser/ParsePreprocessor.c
    Src/Parser/Parser.c
    Src/Parser/ParserCore.c
    Src/Runtime/RuntimeExport.c
    Src/Runtime/RuntimeSources.c
    Src/Sema/Resolve.c
    Src/Sema/TypeLookup.c
)

target_include_directories(cxxe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Src)

if(MSVC)
    target_compile_options(cxxe PRIVATE /W4)
else()
    target_compile_options(cxxe PRIVATE -Wall -Wextra)
endif()
