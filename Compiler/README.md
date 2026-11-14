# CXXE Compiler

Sources modulaires du convertisseur `cxxe` (C++ Extended -> CPIR -> C++),
issues du découpage de l'ancien `cxxe.c` monolithique. Le comportement est
strictement identique (sorties CPIR / C++ comparées octet par octet).

## Build

```bat
Compiler\Build.bat            :: MinGW gcc -> cxxe.exe à la racine du dépôt
```

```sh
cmake -S Compiler -B Compiler/build && cmake --build Compiler/build
```

## Pipeline

```
source .cppe ──► Lexer ──► Parser ──► Frontend (includes) ──► Sema ──► CodeGen ──► .cpp
                              │                                  │
                              └────────── Ir (CPIR) ◄────────────┘──► IrSerialize (.cpir)
```

## Architecture (`Src/`)

| Dossier     | Rôle |
|-------------|------|
| `Core/`     | Socle commun : includes système (`Common.h`), allocation/diagnostics (`Memory`), buffer `Str` (`StrBuf`), E/S fichiers (`FileIo`), chemins et dossier de l'exécutable (`Path`). |
| `Lexer/`    | Modèle de token sans perte (`Token.h`) et lexer C++/CXXE (`Lexer`). |
| `Ir/`       | Représentation CPIR : `NodeKind`, flags, arbre (`Ir`), format binaire `.cpir` (`IrSerialize`), commande `dump` (`IrDump`). |
| `Parser/`   | Tokens -> arbre IR. `Parser.h` = API publique (`parse_source`), `ParserInternal.h` = état `Parser` partagé. Un fichier par famille : décorateurs, expressions, déclarations, préprocesseur, classes/propriétés, décorateurs personnalisés. |
| `Frontend/` | Résolution des `#include` et import des décorateurs déclarés dans les en-têtes (`stde` inclus) : `IncludeResolver`. |
| `Sema/`     | Requêtes sur l'IR (types locaux, propriétés : `TypeLookup`) et passes sémantiques (fonctionnalités natives, arguments nommés : `Resolve`). |
| `CodeGen/`  | Génération C++ : `CodeGen.h` = API publique (`emit_exact`, `emit_cpp`), un fichier par fonctionnalité (appels nommés, registre runtime, décorateurs, propriétés, `match`, sources). |
| `Runtime/`  | Runtime CXXE embarqué (`RuntimeSources`) et commande `cxxe runtime ...` (`RuntimeExport`). |
| `Driver/`   | Enchaînement des étapes de compilation (`Pipeline` : `build_ir_from_cpp`). |
| `Cli/`      | Aide/erreurs de ligne de commande (`Cli`) et point d'entrée `main` (`Main`). |

Règles : les fonctions utilisées par un seul fichier restent `static` ; chaque
fonction partagée est déclarée dans l'en-tête de son module. Les includes se
font depuis `Src/` (`#include "Ir/Ir.h"`). Fichiers et dossiers en PascalCase.
