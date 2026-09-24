# CXXE + CMake

Ce package contient le frontend CXXE et son intégration CMake.

## Runtime

Le runtime CXXE est embarqué directement dans `cxxe.exe`. Le toolchain CMake ne recherche donc aucun `cxxe_runtime.cpp` ou `cxxe_runtime.hpp` installé à côté du module.

CXXE fournit :

```text
cxxe runtime export <directory>
cxxe runtime header <output.hpp|->
cxxe runtime source <output.cpp|->
```

`runtime export` génère les deux fichiers. Avec `-`, le contenu est envoyé sur stdout.

## CMake

```cmake
include(CXXE)

cxxe_add_executable(MyGame
    src/main.cpp
    src/player.cppe
)
```

Les `.cppe` passent par CXXE puis le C++ généré est compilé par le compilateur C++ choisi par CMake.

Pour utiliser le toolchain :

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/CXXEToolchain.cmake \
  -DCXXE_EXECUTABLE=/path/to/cxxe.exe
cmake --build build
```
