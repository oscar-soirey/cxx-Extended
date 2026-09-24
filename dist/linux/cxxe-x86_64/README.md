<img src="./docs/assets/icon-transparent_white_line.png" alt="C++Extended logo" width="110">

# C++Extended (cxxe)

## Introduction

C++ is powerful, but it carries decades of ceremony: plumbing to observe a variable, boilerplate for getters, fragile macros to convert an `enum` to a string, destructors repurposed to emulate `finally`.

**C++Extended** (compiler: `cxxe`) is a superset of C++ that keeps **100% of C++ features** and adds a modern layer of ergonomics on top: decorators, signals, `defer`, native properties, runtime reflection and more. The principle is simple: `cxxe` transforms your extended code into standard C++, without removing anything from the original language.

There is no new language to learn. You extend the one you already know.

## Table of contents

- [Feature status](#feature-status)
- [C++ compatibility](#c-compatibility)
- [Named parameters](#named-parameters)
- [Native decorators and reflection](#native-decorators-and-reflection)
- [Custom decorators](#custom-decorators)
- [Lifecycle decorators](#lifecycle-decorators)
- [Signals](#signals)
- [defer](#defer)
- [Introspectable enums](#introspectable-enums)
- [Properties (native getters/setters)](#properties-native-getterssetters)
- [Output redirection (IOutput)](#output-redirection-ioutput)
- [Standard library](#standard-library)
- [CMake toolchain](#cmake-toolchain)
- [Concepts (roadmap)](#concepts-roadmap)
- [Notes and limitations](#notes-and-limitations)

## Feature status

| Feature | Status |
|---|---|
| All C++ features | ✅ Available |
| Named parameters | ✅ Available |
| Native decorators (`@register`, `@exposed`, …) | ✅ Available |
| Custom decorators | ✅ Available |
| `@deprecated`, `@since`, `@experimental` | ✅ Available |
| Signals (`signal:`) | ✅ Available |
| `defer` | ✅ Available |
| Introspectable enums | ✅ Available |
| Properties (`property`) | ✅ Available |
| Output redirection (`IOutput`) | ✅ Available |
| `dynamic_list` | ✅ Available |
| CMake toolchain | ✅ Available |
| Templates on decorators | 🛠 Planned |
| `dynamic` type | 💡 Concept |
| Pattern matching (`match`) | 💡 Concept |
| Null safety (`?->`) | 💡 Concept |
| JSON / XML / other formats (native controls) | 💡 Concept |
| Automatic serialization (`@serializable`) | 💡 Concept |

## C++ compatibility

C++Extended is a **superset** of C++: any valid C++ code is valid C++Extended code. An existing project can therefore be migrated file by file.

## Named parameters

Arguments can be passed by name, in any order.

```cpp
int add(int a, int b);

int main() {
    int r = add(b=5, a=4);
}
```

## Native decorators and reflection

Native decorators let you register classes and expose members so they can be accessed **at runtime** by name.

| Decorator | Purpose |
|---|---|
| `@register` | Registers the class in the factory. |
| `@exposed` | Exposes a member to reflection. It must be `public`. |

```cpp
@register
class A {
public:
    @exposed int health;
};
```

### Creating instances

```cpp
// Instantiate by name
A* obj = stde::factory_new("A");
// transformed by cxxe into: new A()

// Retrieve the type by name
A* obj2 = new stde::factory_find("A")();
// transformed by cxxe into: new A()
```

### Accessing exposed members

```cpp
obj->get_member("health") = 4;
// transformed by cxxe into: obj->health = 4;
```

### Dynamic usage

All of these are resolved **at runtime**, so the name can come from a source that is unknown at compile time.

```cpp
stde::factory_new(unknown_string);
```

> [!IMPORTANT]
> An `@exposed` member must be `public`.

## Custom decorators

You can declare your own decorators. A decorator receives the decorated function (`func`) and wraps it as needed.

```cpp
void @my_decorator() -> func(int a) {
    before();
    func(a);
    after();
}
```

A decorator can also take **parameters**:

```cpp
void @my_decorator(/* decorator parameters */) -> func(int a) {
    before();
    func(a);
    after();
}
```

Usage:

```cpp
@my_decorator
void work(int a) { /* ... */ }
```

> [!NOTE]
> Support for **templates** on decorators is planned for a future release.

## Lifecycle decorators

Three standard decorators document the evolution of your API.

| Decorator | Description |
|---|---|
| `@deprecated(msg)` | Marks an element as obsolete, with a message. |
| `@since(version)` | Indicates the version in which the element was introduced. |
| `@experimental(msg)` | Flags an unstable API that may change. |

```cpp
@since("1.2")
@deprecated("Use load_v2() instead")
void load();

@experimental("The interface may change")
void new_feature();
```

## Signals

A `signal` detects changes to a variable and runs a block of code whenever it is modified. It works anywhere: global scope, classes, structs, and so on.

```cpp
int my_int = 0;

signal: my_int {
    var_changed();
}
```

Inside a class:

```cpp
class Player {
public:
    int score = 0;

    signal: score {
        update_ui();
    }
};
```

## defer

`defer` runs a block when the **current scope exits**, regardless of the exit path.

```cpp
void process() {
    FILE* f = fopen("data.txt", "r");
    defer { fclose(f); }

    // ... use f ...
}   // fclose(f) is called here
```

## Introspectable enums

Enums are introspectable: `enum → string` conversion, `string → enum` conversion, and other reflection operations.

```cpp
enum class Color { Red, Green, Blue };
```

The available introspection functions notably cover:

- `enum → string` conversion;
- `string → enum` conversion;
- other reflection utilities on the enum's values.

## Properties (native getters/setters)

Classes, structs and unions natively support properties with `get` and `set`, without boilerplate.

The property and the variable are the same entity: they must use **exactly the same name**. Inside the `get` and `set` blocks, that name refers to the underlying variable, and `value` holds the assigned value.

```cpp
class Character {
public:
    int health = 100;

    property health {
        get { return health; }
        set { health = clamp(value, 0, 100); }
    }
};
```

Usage:

```cpp
Character c;
c.health = 150;    // calls the setter: health = 100
int h = c.health;  // calls the getter
```

> [!IMPORTANT]
> The property and the variable it wraps must share the same name.

## Output redirection (IOutput)

All outputs can be redirected natively, regardless of the mechanism used in the code:

- `std::cout`
- `printf`
- `std::print`
- and other standard outputs

Redirection is based on the **`IOutput`** interface: implement it to send output to a file, an editor console, the network, a log, and so on.

```cpp
#include <ioutput>
```

## Standard library

C++Extended ships a small standard library, available through dedicated headers.

| Header | Purpose |
|---|---|
| `<ioutput>` | Redirects `std::cout`, `printf` and other outputs (see [Output redirection](#output-redirection-ioutput)). |
| `<dynamic_list>` | Stores objects of different types in a single list. |

```cpp
#include <ioutput>
#include <dynamic_list>
```

## CMake toolchain

C++Extended integrates with **CMake** through a dedicated toolchain. Extended sources are processed by `cxxe` before the regular C++ compilation, so the language can be adopted in existing projects without changing the build system.

### Runtime

The CXXE runtime is embedded directly in `cxxe.exe`. The CMake toolchain therefore does not look for any `cxxe_runtime.cpp` or `cxxe_runtime.hpp` installed next to the module.

`cxxe` provides the following commands to get the runtime files:

```text
cxxe runtime export <directory>
cxxe runtime header <output.hpp|->
cxxe runtime source <output.cpp|->
```

`runtime export` generates both files. With `-`, the content is written to stdout.

### Usage

```cmake
include(CXXE)

cxxe_add_executable(MyGame
    src/main.cpp
    src/player.cppe
)
```

`.cppe` files go through `cxxe`, then the generated C++ is compiled by the C++ compiler chosen by CMake. Regular `.cpp` files are listed alongside them as usual.

### Using the toolchain

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/CXXEToolchain.cmake \
  -DCXXE_EXECUTABLE=/path/to/cxxe.exe
cmake --build build
```

| Variable | Purpose |
|---|---|
| `CMAKE_TOOLCHAIN_FILE` | Path to `CXXEToolchain.cmake`. |
| `CXXE_EXECUTABLE` | Path to the `cxxe` executable. |

## Concepts (roadmap)

> [!WARNING]
> The following items are **concepts** under consideration. Their syntax and behavior are not final, and they are not yet implemented.

### dynamic type

A type whose actual type is known only at **runtime**, Python-style. It allows, for example, storing values of different types in a single container.

```cpp
std::vector<dynamic> items = { 42, "hello", 3.14 };
```

### Pattern matching

A `match` statement handles a value according to its type. Inside each branch, the value is already cast to the matching type.

```cpp
match value {
    int: printf("%d\n", value);  // value is already an int here
}
```

### Null safety

The `?->` operator replaces the manual null check before a member access.

```cpp
// Today
if (player) player->update();

// With null safety
player?->update();
```

### JSON, XML and other data formats

Native controls to work with common data formats (JSON, XML, etc.) directly from the language.

### Automatic serialization

The `@serializable` decorator will automatically generate serialization and deserialization for a class.

```cpp
@serializable
class Config {
public:
    int width;
    int height;
};
```

## Notes and limitations

- The examples in this documentation illustrate the syntax; the exact names of `stde` library functions may vary between versions.
- An `@exposed` member must be `public`.
- A `property` and the variable it wraps must share the same name.
- Features marked "Concept" are not yet implemented.
