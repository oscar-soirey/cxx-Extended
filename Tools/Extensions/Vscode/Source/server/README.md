# C++Extended language server

Native executable started by the extension and spoken to over stdio (JSON-RPC / LSP). Its sources are in
`Tools/Ls/Src` and it links the compiler modules of `Compiler/Src`.

Build it and copy it here (under `<platform>-<arch>/`):

```text
Tools\Ls\Build.bat          Windows, MinGW-w64 gcc   -> server/win32-x64/cxxe-language-server.exe
Tools/Ls/build.sh           Linux / macOS            -> server/linux-x64/ or darwin-*/cxxe-language-server
cmake -S Tools/Ls -B build-ls && cmake --build build-ls
```

`cxxe-language-server --version` prints the version; otherwise it reads LSP messages from stdin.
