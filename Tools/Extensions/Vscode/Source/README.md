# C++Extended for VS Code

Language support for **C++Extended** (`.cppe` sources, `.hppe` / `.he` headers), powered by the native CXXE
language server. The server is built on the compiler's own modules, so its diagnostics are exactly those of
`cxxe`.

## Features

- **Highlighting**: the full C++ grammar of VS Code plus CXXE syntax (`@decorators`, `property` / `get` / `set`,
  `match`, `dynamic`, `-> func`), and semantic highlighting (properties, parameters, named-argument labels,
  locals, types, decorators).
- **Diagnostics while typing**: CXXE errors at the right `line:column` (unknown named argument, invalid
  decorator, property without setter, unbalanced brackets...).
- **C++ check on save**: the generated C++ is compiled with `g++ -fsyntax-only` (or `clang++`) and its
  errors are shown in the `.cppe` file, thanks to the `#line` directives emitted by `cxxe`.
- **Completion**: members after `.` / `->` (properties, fields, methods, inherited members), `Type::` and
  `namespace::`, `name=` labels for named arguments (only parameters not passed yet), the real decorators
  after `@` (`@register`, `@exposed` and the custom decorators that are declared or included), locals,
  keywords, `stde::` runtime functions, snippets (`property`, `decorator`, `register`, `match`...).
- **Signature help**, following named arguments (`f(b=1, a=|)` highlights `a`).
- **Navigation**: hover, go to definition (also across `.hppe` headers and on `#include`), find references,
  document highlights, outline, workspace symbols, clickable includes.
- **Rename** across files; renaming a parameter also renames the `name=` labels at its call sites.
- **Show Generated C++** (editor title button): live, read-only view of the C++ produced by `cxxe`.
- **Run Current File** (`Ctrl+F5`): translates the file (and its sibling `.hppe` / `.cppe` without `main`),
  compiles it with the runtime and runs it.
- **Build Project (CMake)**: configures and builds a project using `CMake/CXXE.cmake`; errors are linked
  back to the `.cppe` sources.

## Requirements

- The language server executable. It is bundled under `server/<platform>-<arch>/` when the extension is
  packaged; build it with `Tools/Ls/Build.bat` (Windows, MinGW gcc) or `Tools/Ls/build.sh` (Linux/macOS),
  which also copy it there.
- `cxxe` for Run/Build (setting `cxxe.compilerPath`, otherwise `cxxe` on `PATH`). Its directory also provides
  the standard CXXE headers (`<cxxe dir>/stde`).
- `g++` or `clang++` for the C++ check and Run (setting `cxxe.cppCompiler`).

## Settings

| Setting | Default | |
|---|---|---|
| `cxxe.languageServer.path` | bundled, then `PATH` | language server executable |
| `cxxe.compilerPath` | `cxxe` on `PATH` | compiler used by Run/Build and for `stde` |
| `cxxe.includePaths` | `[]` | extra `-I` directories (relative to the workspace or absolute) |
| `cxxe.cppCompiler` | `g++` | C++ compiler for the check and Run (empty disables the check) |
| `cxxe.cppStandard` | `c++20` | standard of the generated C++ |
| `cxxe.cppCheck` | `onSave` | `onSave` or `off` |
| `cxxe.build.directory` | `build` | CMake build directory |

The status bar item **CXXE** shows the server state; click it for the log.

## Development

Open this folder in VS Code and press `F5` to start an Extension Development Host. Package with
`npx @vscode/vsce package` after building the server.
