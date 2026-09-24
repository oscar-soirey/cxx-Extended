# Correction and Validation Report — CXXE

## 1. Scope and Method

The work was performed based on the actual contents of `cxxe.c` and the provided README, without using another implementation as a reference and without inferring syntax or behavior from another language.

The compiler itself states that it is self-contained : it does not compile C++, nor does it invoke clang or LLVM/libclang, and preserves a lossless lexical representation in the CPIR before emitting C++. The correction work therefore focused directly on this file's lexer/parser/IR/resolution/emission stages.

Applied method:

1. Build deliberately large and mixed `.cppe` files instead of small artificial examples.
2. Reproduce each failure separately.
3. Identify the exact context in which it occurs and the cause in the compiler code.
4. Fix only the relevant cause.
5. Recompile the compiler.
6. Rerun the targeted case.
7. Then rerun a regression suite covering classes, methods, decorators, structs, unions, namespaces, enums, properties, named calls, reflection/factory, `dynamic`, `match`, templates, and ordinary C++.
8. Finish with a strict compilation `-Wall -Wextra -Werror` followed by ASan/UBSan validation with leak detection.

The final set contains 18 `.cppe` corpora actually used for validation.

## 2. What Was Found and Fixed

### 2.1. `@register` could be associated with the wrong element after a previous block

**Reproduction:** `mega.cppe`.

**Initial state:** le parseur s'arrêtait sur le décorateur avec l'erreur :

`cxxe: error: @register can only be applied to a class, struct, union, or enum (token 181)`

**Cause:** la détection des déclarations utilisant des décorateurs pouvait continuer à considérer une zone précédente comme ouverte au lieu de respecter la fin réelle de la déclaration/bloc précédent. The decorator was therefore attached to a context that was no longer the correct one.

**Fix:** tightening of the recognition boundary for decorated declarations so that a declaration is properly completed before consuming the next decorator.

**Validation:** `mega.cppe` then passes parsing, emission, C++ compilation, and execution.

---

### 2.2. A class method with a body could skip subsequent members

**Reproduction:** large corpus with several fields, properties, and methods in a class.

**Cause:** le corps d'une méthode était ensuite consommé avec `find_stmt_end()`, helper général prévu pour les statements/déclarations terminés par `;`. For a member with its own `{ ... }` block, this consumption went beyond the correct boundary and the `parse_class_body()` loop skipped elements located after the method.

**Fix:** when a member is recognized as a function/method with a body, the body boundary is determined by its matching brace instead of reusing the generic statement end.

**Validation:** the multi-member classes in the corpora `mega`, `full_integration`, `method_decorator` et `complex_cpp` sont correctement parcourues.

---

### 2.3. `add_expr_nodes()` could create a huge `N_ASSIGN_EXPR` covering an entire function

**Reproduction:** function containing several statements, only one of which contains `=`.

**Cause:** `add_expr_nodes()` could receive the entire contents of a function body. An assignment search over this large range could then create a node whose `first_tok/last_tok` encompassed unrelated statements. During emission, this native node advanced the cursor past several statements, which were then no longer emitted correctly.

**Fix:** un `N_ASSIGN_EXPR` n'est créé sur une plage large que lorsqu'elle correspond réellement à une petite expression/statement : pas de bloc de niveau supérieur et pas de multiplicité de terminateurs de statement.

**Validation:** les corps contenant plusieurs instructions continuent à être émis dans `mega`, `full_integration`, `decorator_body_features` et `method_decorator`.

---

### 2.4. Emission order depended on IR node creation order

**Reproduction:** property + property assignment + named call in the same scope.

**Cause:** several passes create native nodes at different times. A property assignment could therefore be created before an expression node located earlier in the file. The emitter traversed nodes in creation order, whereas its source cursor requires ordering by position in the text.

**Fix:** collect native-emission nodes and sort them by `first_tok`, then `last_tok` in case of a tie. The same principle is applied when emitting recursive ranges.

**Validation:** `mega.cppe`, `named_order.cppe`, `named_property.cppe`, `decorator_body_features.cppe` et `full_integration.cppe` passent.

---

### 2.5. A property was resolved only by name, independently of the receiver type

**Reproduction:** `property_scope.cppe`.

The corpus contains two types that each have a `health` property, as well as an ordinary type with only a `health` field and no property.

**Initial state:** le code émis pour le type ordinaire contenait par exemple `p.set_health(5)` et `p.get_health()`, ce qui provoquait :

`'struct Plain' has no member named 'set_health'`

et

`'struct Plain' has no member named 'get_health'`

**Cause:** une occurrence `receiver.member` était suffisamment proche d'une propriété portant le même nom pour être considérée comme un accès à cette propriété. The actual receiver type was not sufficiently constraining.

**Fix:** contextual resolution based on the receiver type. La résolution cherche maintenant le type déclaré du paramètre/local ou le type du champ utilisé comme récepteur, puis exige une propriété appartenant au type correspondant.

**Validation:** `property_scope.cppe` passe avec plusieurs `health` homonymes. Sortie finale : `6 7 5 7`.

---

### 2.6. Contextual resolution also needed to know class fields

**Reproduction:** `property_field.cppe`.

Exemples : un champ `child` de type classe et un champ pointeur `ptr` sont utilisés comme récepteurs de propriété.

**Cause:** la première correction couvrait les paramètres et variables locales, mais pas tous les champs de la classe englobante.

**Fix:** extend the resolver to `N_FIELD_DECL` nodes of the enclosing type, including pointer fields.

**Validation:** `child.health` et `ptr->health` sont correctement transformés. `property_field.cppe` s'exécute et renvoie `10`.

---

### 2.7. Property assignment RHS expressions were copied without native transformation

**Reproduction:** expression de setter contenant elle-même un accès de propriété.

**Cause:** le générateur du setter recopiait le RHS comme des tokens bruts. Une propriété imbriquée dans ce RHS restait donc sous sa forme CXXE.

**Fix:** les RHS concernés passent maintenant par l'émetteur récursif de plage native, which can itself transform properties, named calls, factory/reflection, and other already-recognized native constructs.

**Validation:** `named_property.cppe` et `decorator_body_features.cppe`.

---

### 2.8. Named calls were sometimes recognized in the IR but their translation did not reach the backend

**Reproduction:** `named_property.cppe` et `decorator_body_features.cppe`.

**Cause:** the named-call fallback path reset the accumulated output. In a nested construction, this could therefore erase text that had already been produced before the call.

**Fix:** generate the fallback into a temporary `Str` and then concatenate it with the existing output.

**Validation:** appels nommés normaux, appels surchargés et appels nommés contenant une propriété passent dans `named_order`, `named_overloads`, `named_property` et `full_integration`.

---

### 2.9. Named-argument resolution had local state that could affect the next call

**Cause:** the modification flag used by named-argument resolution was not strictly reset for each function being examined. State from a previous resolution could therefore affect the current call.

**Fix:** introduce a `call_changed` state strictly local to each call-resolution attempt.

**Validation:** `named_order.cppe` et `named_overloads.cppe`, including reversed arguments, default values, and overloads.

---

### 2.10. The body of a decorated function was emitted as opaque text

**Reproduction:** `decorator_body_features.cppe`.

**Initial state:** le C++ généré contenait encore le CXXE `@trace` et le corps conservait notamment `c.health` et `add(right=2, left=c.health)`. Le compilateur C++ signalait alors le `@` et les éléments CXXE restants.

**Cause:** a function with a custom decorator was transformed through a specialized emission path, but its underlying body was copied without going through the native transformations already available.

**Fix:** recursively emit the decorated function body through `emit_native_token_range()`.

**Validation:** `decorator_body_features.cppe` passe et produces `5 7`.

---

### 2.11. A custom decorator lookup failed in a nested scope

**Reproduction:** `method_decorator.cppe` et `decorator_namespace.cppe`.

**Cause:** the lookup essentially required an exact match of the current qualified name. Pour un décorateur défini au niveau de `Game` et utilisé depuis `Game::Player`, the lookup did not walk up the lexical scopes.

**Fix:** progressively resolve the name from the innermost scope to parent scopes and then to the unqualified name : par exemple `Game::Player::trace`, puis `Game::trace`, puis `trace`.

**Validation:** `decorator_namespace` et `method_decorator` passent, y compris lorsque le décorateur est utilisé sur une méthode de classe.

---

### 2.12. A compound assignment on a property generated `get_property() += ...`

**Reproduction:** `property_compound.cppe`.

**Initial state:**

```cpp
c.get_count() += 4;
++c.get_count();
c.get_count()--;
```

Le compilateur C++ refusait ces formes car le getter n'est pas une destination modifiable.

**Cause:** les opérateurs composés n'étaient pas abaissés au niveau du setter.

**Fix:** reconnaissance de `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`. For a property, the emitter builds the equivalent `set(get() op rhs)` while preserving right-hand-side evaluation through the recursive native emitter.

**Validation:** `property_compound.cppe` renvoie `5`.

---

### 2.13. `++`/`--` on a property produced a getter followed by `++`/`--`

**Reproduction:** `property_unary_expression.cppe` et `property_compound.cppe`.

**Initial state:**

```cpp
int old = c.get_count()++;
int newer = ++c.get_count();
```

ce qui échouait avec `lvalue required as increment operand`.

**Cause:** le getter avait déjà été abaissé, mais la nature mutante de `++/--` n'était pas représentée comme une opération native distincte liée à la propriété.

**Fix:** ajout d'un `N_UNARY_EXPR` dédié pour ces opérateurs lorsqu'ils ciblent une propriété. The backend distinguishes prefix and postfix forms:

- prefix: read, modify through the setter, then read again as the result;
- postfix: preserve the old value, modify through the setter, and return the old value.

**Validation:** `property_unary_expression.cppe` compile/exécute, et le corpus composé passe aussi.

---

### 2.14. The setter parameter `value` could shadow a member with the same name

**Reproduction:** `struct_union_features.cppe`.

The corpus contains a property named `value`, in accordance with the documented rule requiring the property and its underlying variable to have exactly the same name.

**Cause:** the generated setter itself used a parameter named `value`, creating a collision when the underlying member was also named `value`.

**Fix:** use of a distinct internal name for the assigned value (`CXXEPropertyAssignedValue`) and targeted rewriting of the getter/setter body to `this->...` for accesses to the corresponding member.

**Validation:** le struct et l'union du corpus passent ; sortie finale `0 8`.

---

### 2.15. `token_range_text()` results were passed directly to IR setters without being freed

**Reproduction:** validation ASan/LeakSanitizer du compilateur lui-même.

**Cause:** `token_range_text()` returns a new allocation. Direct calls of the form `node_set_value(..., token_range_text(...))` made a second copy into the node and then lost the first allocation. The same issue occurred with some `scope_qualified()` calls and temporary names.

**Fix:** add small ownership helpers for ranges (`node_set_value_range`, `node_set_type_range`, `node_set_return_range`, `node_set_qualified_scope`, etc.) which copy the data into the IR and then explicitly free the temporary buffer. Names produced by `xstrndup0()` are also freed after being passed to the setter. The custom-decorator path that passed a `token_range_text()` result into a `Str` was also fixed.

**Validation:** le compilateur et les programmes générés ont été exécutés avec ASan + UBSan + `detect_leaks=1`. Aucun diagnostic restant sur les 18 corpus.

## 3. Validation of Existing Features

A corpus `cpp_regression.cppe` contains only ordinary C++ : preprocessor, namespace, struct, union, enum, class, alias, anonymous namespace, lambda, and initializer lists.

Its source -> CPIR -> C++ round-trip is **byte-for-byte identical** and it compiles. The program intentionally returns `27`; this value is therefore expected and is not considered a failure.

This verifies the essential point that the targeted fixes to CXXE features did not introduce an observable regression in this subset of ordinary C++.

## 4. Final Corpus Used

| Fichier | Main coverage |
|---|---|
| `mega.cppe` | classes, struct/union, `@register`, `@exposed`, properties, custom decorator, named args, factory/reflection |
| `full_integration.cppe` | full combination of the previous constructs + `dynamic`/`match`, overloading, templates, operators |
| `property_scope.cppe` | same-named properties in multiple types + ordinary type without a property |
| `property_field.cppe` | property through a class field and pointer field |
| `property_compound.cppe` | `+=` and property mutations |
| `property_unary_expression.cppe` | prefix/postfix `++/--` in expressions |
| `struct_union_features.cppe` | struct, union, `value` property, reflection |
| `decorator_body_features.cppe` | custom decorator + property + named call in its body |
| `decorator_namespace.cppe` | qualified custom decorator |
| `method_decorator.cppe` | namespace decorator applied to a class method |
| `named_order.cppe` | reversed named args + defaults |
| `named_overloads.cppe` | named args + overloaded functions |
| `named_property.cppe` | named arg whose RHS contains a property |
| `runtime_enum_factory.cppe` | enum reflection + factory + `get_member` |
| `runtime_reflection.cppe` | `@register`, `@exposed`, runtime reflection |
| `match_control.cppe` | `dynamic` + `match` |
| `complex_cpp.cppe` | complex C++ without specific CXXE transformation |
| `cpp_regression.cppe` | ordinary C++ regression and lossless round-trip |

## 5. Final Results

### Strict Compiler Compilation

Command:

```text
cc -std=c11 -Wall -Wextra -Werror -O2 cxxe_fixed.c -o cxxe_fixed_strict
```

Result: **PASS**.

### Functional Suite

- 18/18 corpora: **PASS**
- each corpus: parse = 0, emission = 0, C++ compilation = 0
- 17 programs have an expected return code of `0`
- `cpp_regression.cppe` a un code retour volontairement attendu `27`
- round-trip of `cpp_regression.cppe`: **byte-for-byte identical**

### ASan / UBSan

- compiler built with `-fsanitize=address,undefined`
- generated C++ + runtime built with the same sanitizers
- `ASAN_OPTIONS=detect_leaks=1`
- `UBSAN_OPTIONS=halt_on_error=1`
- 18/18 corpora: **PASS**
- no memory/sanitizer diagnostics in the final validation

## 6. Important Point About the README

The provided README states that several features are already available, notamment `signal`, `defer` et les décorateurs `@deprecated`, `@since`, `@experimental`. However, the provided `cxxe.c` source contains no occurrence of these constructs, and no corresponding parsing/IR/emission path could be identified in this file.

I therefore deliberately **did not invent or approximately implement them**. Claiming them as “fixed” would have produced a false report.

Conversely, le source contient bien un `N_MATCH_STMT` et un chemin d'émission dédié, while the README still classifies `match` as a concept. `match` was therefore tested according to the behavior actually present in the compiler, rather than according to syntax extrapolated from the README.

The README must therefore be synchronized with the source before these documented features can be considered guaranteed.

## 7. Intentionally Preserved Limitation

A custom-decorator case with a non-`void` decorated function was investigated during testing. The wrapper's automatic return behavior is not defined clearly enough by the provided source/README : the documented example is a `void` function, and the current generation does not specify automatic propagation of a result.

This case was not artificially “fixed” by inventing semantics. The final corpus therefore uses forms whose behavior is established by the provided compiler.

## 8. Deliverables

The package contains:

- `cxxe_fixed.c` : fixed source ;
- `examples/*.cppe` : the 18 `.cppe` files actually used ;
- `results/normal_matrix.txt` : complete parse/emit/compile/runtime matrix ;
- `results/asan_matrix.txt` : ASan/UBSan matrix ;
- `results/baseline_matrix.txt` : reproduction results on the original compiler ;
- `results/cxxe_changes.diff` : original -> fixed unified diff ;
- `results/build_status.txt` : strict/sanitizer build status ;
- `results/SHA256SUMS.txt` : file hashes ;
- `TEST_COMMANDS.txt` : validation commands used.

The final diff contains 1,511 patch lines compared with the provided original `cxxe.c`. It mainly contains the parsing, property-resolution, native-emission, and ownership-management fixes described above.
