# CXXE Tools

CXXE comes with a collection of tools designed to make development with CXXE easier, faster, and more enjoyable.

## Language Server — `cxxe\_ls.exe`

The CXXE Language Server provides intelligent editor support for CXXE-based projects.

It is designed to work with modern editors and IDEs through the **Language Server Protocol (LSP)**, providing features such as:

* Syntax highlighting and semantic analysis
* Real-time diagnostics and compiler errors
* Code completion and suggestions
* Go to definition / declaration
* Find references
* Symbol and workspace navigation
* Hover information and documentation
* Rename refactoring
* Signature and parameter information
* Code actions and quick fixes

The language server is shared across supported editors, meaning CXXE does not need a separate language-analysis implementation for every IDE.

## CXXE Compiler — `cxxe.exe`

The CXXE compiler is the main toolchain executable.

It is responsible for compiling CXXE source code and providing the core compilation features of the language.

Example:

```bash
cxxe main.cxx
```

It can also be used as part of larger build systems and development workflows.

## CXXE Documentation Tool — `cxxe-doc.exe`

A documentation generator capable of extracting documentation directly from CXXE source code.

It can generate documentation for:

* Functions
* Classes and types
* Namespaces
* APIs
* Modules
* Public interfaces

This can be used to generate HTML or other documentation formats for CXXE libraries.

## CXXE Linter — `cxxe-lint.exe`

A static-analysis tool for detecting potential problems before compilation.

It can identify issues such as:

* Suspicious code
* Unused declarations
* Potential bugs
* API misuse
* Unnecessary constructs
* Style violations

The linter can be used independently or integrated into the CXXE Language Server.

## Toolchain Overview

|Tool|Purpose|
|-|-|
|`cxxe.exe`|Compiler and main project CLI|
|`cxxe\_ls.exe`|Language Server / LSP|
|`cxxe-lint.exe`|Static analyzer / linter|
|`cxxe-doc.exe`|Documentation generator|

Together, these tools form the CXXE development ecosystem, covering the complete workflow from writing and analyzing source code to building, testing, documenting, and distributing applications.

