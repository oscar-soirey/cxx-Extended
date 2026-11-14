@echo off
REM Build cxxe from the modular sources (MinGW gcc).
REM Usage: Compiler\Build.bat [output]   (default: ..\cxxe.exe)
setlocal
set "ROOT=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%..\cxxe.exe"
gcc -std=c11 -O2 -Wall -Wextra -I"%ROOT%Src" ^
  "%ROOT%Src\Cli\Cli.c" ^
  "%ROOT%Src\Cli\Main.c" ^
  "%ROOT%Src\CodeGen\EmitCalls.c" ^
  "%ROOT%Src\CodeGen\EmitDecorators.c" ^
  "%ROOT%Src\CodeGen\EmitMatch.c" ^
  "%ROOT%Src\CodeGen\EmitProperties.c" ^
  "%ROOT%Src\CodeGen\EmitRegistry.c" ^
  "%ROOT%Src\CodeGen\EmitSource.c" ^
  "%ROOT%Src\CodeGen\Emitter.c" ^
  "%ROOT%Src\Core\FileIo.c" ^
  "%ROOT%Src\Core\Memory.c" ^
  "%ROOT%Src\Core\Path.c" ^
  "%ROOT%Src\Core\StrBuf.c" ^
  "%ROOT%Src\Driver\Pipeline.c" ^
  "%ROOT%Src\Frontend\IncludeResolver.c" ^
  "%ROOT%Src\Ir\Ir.c" ^
  "%ROOT%Src\Ir\IrDump.c" ^
  "%ROOT%Src\Ir\IrSerialize.c" ^
  "%ROOT%Src\Lexer\Lexer.c" ^
  "%ROOT%Src\Parser\ParseClasses.c" ^
  "%ROOT%Src\Parser\ParseCustomDecorators.c" ^
  "%ROOT%Src\Parser\ParseDeclarations.c" ^
  "%ROOT%Src\Parser\ParseDecorators.c" ^
  "%ROOT%Src\Parser\ParseExpressions.c" ^
  "%ROOT%Src\Parser\ParsePreprocessor.c" ^
  "%ROOT%Src\Parser\Parser.c" ^
  "%ROOT%Src\Parser\ParserCore.c" ^
  "%ROOT%Src\Runtime\RuntimeExport.c" ^
  "%ROOT%Src\Runtime\RuntimeSources.c" ^
  "%ROOT%Src\Sema\Resolve.c" ^
  "%ROOT%Src\Sema\TypeLookup.c" ^
  -o "%OUT%"
exit /b %ERRORLEVEL%
