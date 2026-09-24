# CXXE Language Server

## Objectif

`cxxe-language-server` est le serveur LSP commun de C++Extended. Les extensions VS Code, Visual Studio, CLion et autres IDE n'ont donc pas à réimplémenter le parsing C++E : elles lancent toutes le même processus et communiquent avec lui via LSP/JSON-RPC sur `stdin/stdout`.

## Décision d'architecture : ne pas réécrire C++

Le serveur ne réécrit pas un second compilateur C++.

Le cœur est repris directement du compilateur CXXE actuel : lexer lossless, parser, IR, résolution des décorateurs, propriétés, arguments nommés et mécanismes C++E. C'est cohérent avec le design du compilateur : il conserve les tokens source et construit un IR qui décrit à la fois la syntaxe générique et les éléments sémantiques orientés C++E.

La couche LSP exploite cet IR pour fournir les fonctionnalités IDE.

Pour une analyse sémantique C++ complète à long terme (résolution avancée des templates, overload resolution complète, instanciation, index global très riche, diagnostics de type identiques à un compilateur C++), il vaut mieux ajouter un backend optionnel basé sur `clangd` plutôt que recréer toutes ces règles dans CXXE. Le serveur actuel est volontairement indépendant de `clangd` : il reste fonctionnel seul et connaît directement la syntaxe C++E.

## Fonctionnalités actuelles

Le serveur implémente :

- `initialize`, `shutdown`, `exit` et transport JSON-RPC LSP sur stdio ;
- synchronisation complète des documents (`didOpen`, `didChange`, `didClose`) ;
- diagnostics syntaxiques CXXE et quelques diagnostics structurels ;
- diagnostics des décorateurs CXXE invalides ;
- diagnostics des propriétés incohérentes ;
- `documentSymbol` ;
- `workspace/symbol` sur les documents ouverts ;
- `hover` ;
- `definition` ;
- `references` textuelles dans le document courant ;
- `completion` générale ;
- completion spécifique aux décorateurs C++E ;
- completion des membres C++E après `.` / `->` ;
- completion des arguments nommés ;
- `signatureHelp` ;
- `semanticTokens/full`.

Les constructions reconnues par le compilateur restent celles de son IR réel : namespaces, classes, structs, unions, enums, fonctions/méthodes, décorateurs, propriétés, arguments nommés, `match`, etc.

## Extensions IDE

Chaque extension IDE n'a qu'un rôle d'adaptateur :

1. enregistrer le langage `.cppe` ;
2. démarrer `cxxe-language-server` ;
3. connecter le processus en LSP stdio ;
4. fournir l'icône, la configuration et les commandes propres à l'IDE.

Le parsing, les symboles, les diagnostics et les complétions restent communs.

## Compilation

### GCC

```bash
gcc -std=c11 -O2 -Wall -Wextra -Werror cxxe_language_server.c -o cxxe-language-server
```

### Clang

Le cœur historique de CXXE contient quelques variables marquées « set but unused » par Clang alors qu'elles sont déjà présentes dans le compilateur. Elles ne concernent pas la couche LSP. La compilation stricte validée est :

```bash
clang -std=c11 -O2 -Wall -Wextra -Werror \
  -Wno-unused-but-set-variable \
  cxxe_language_server.c -o cxxe-language-server
```

## Validation effectuée

La version livrée a été testée avec :

- 18 fichiers `.cppe` complexes issus du corpus CXXE ;
- classes, méthodes, décorateurs, structures, unions, namespaces, enums, templates, propriétés, arguments nommés et `match` ;
- diagnostics volontairement invalides : décorateur inconnu, propriété invalide, accolades non fermées ;
- requêtes LSP : symboles, hover, définition, références, completion, completion décorateurs, completion membres, arguments nommés, signature help et semantic tokens ;
- compilation GCC stricte ;
- compilation Clang stricte ;
- exécution sous AddressSanitizer + UndefinedBehaviorSanitizer avec détection des fuites.

Résultats :

- 18/18 documents sans diagnostic inattendu ;
- 18/18 corpus sous ASan/UBSan sans erreur ;
- les cas invalides produisent bien des diagnostics sans arrêter le serveur ;
- le binaire répond correctement à `initialize`, `shutdown` et `exit`.

## Limite volontaire

Le serveur actuel n'essaie pas de devenir un remplaçant complet d'un moteur sémantique C++ comme `clangd`. Son but immédiat est de fournir une base LSP commune centrée sur C++Extended, en réutilisant directement le vrai frontend CXXE.

L'étape logique suivante est un backend sémantique optionnel : CXXE génère une représentation C++ compilable, le serveur transmet cette représentation à `clangd`, puis un source-map CXXE permet de remapper diagnostics, définitions, hovers et références vers le `.cppe` d'origine.
