!\[C++Extended logo](./assets/icon.png)

# C++Extended (cxxe)

## Introduction

C++ is powerful, but it carries decades of ceremony: plumbing to observe a variable, boilerplate for getters, fragile macros to convert an `enum` to a string, destructors repurposed to emulate `finally`.

**C++Extended** (compiler: `cxxe`) is a superset of C++ that keeps **100% of C++ features** and adds a modern layer of ergonomics on top: decorators, signals, `defer`, native properties, runtime reflection and more. The principle is simple: `cxxe` transforms your extended code into standard C++, without removing anything from the original language.

There is no new language to learn. You extend the one you already know.

## Table of contents

* [Feature status](#feature-status)
* [C++ compatibility](#c-compatibility)
* [Named parameters](#named-parameters)
* [Native decorators and reflection](#native-decorators-and-reflection)
* [Custom decorators](#custom-decorators)
* [Lifecycle decorators](#lifecycle-decorators)
* [Signals](#signals)
* [defer](#defer)
* [Introspectable enums](#introspectable-enums)
* [Properties (native getters/setters)](#properties-native-getterssetters)
* [Output redirection (IOutput)](#output-redirection-ioutput)
* [CMake toolchain](#cmake-toolchain)
* [Concepts (roadmap)](#concepts-roadmap)
* [Notes and limitations](#notes-and-limitations)

## Feature status

|Feature|Status|
|-|-|
|All C++ features|✅ Available|
|Named parameters|✅ Available|
|Native decorators (`@register`, `@exposed`, …)|✅ Available|
|Custom decorators|✅ Available|
|`@deprecated`, `@since`, `@experimental`|✅ Available|
|Signals (`signal:`)|✅ Available|
|`defer`|✅ Available|
|Introspectable enums|✅ Available|
|Properties (`property`)|✅ Available|
|Output redirection (`IOutput`)|✅ Available|
|CMake toolchain|✅ Available|
|Templates on decorators|🛠 Planned|
|`dynamic` type|💡 Concept|
|Pattern matching (`match`)|💡 Concept|
|JSON / XML / other formats (native controls)|💡 Concept|
|Automatic serialization (`@serializable`)|💡 Concept|

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

|Decorator|Purpose|
|-|-|
|`@register`|Registers the class in the factory.|
|`@exposed`|Exposes a member to reflection. It must be `public`.|

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
A\* obj = stde::factory\_new("A");
// transformed by cxxe into: new A()

// Retrieve the type by name
A\* obj2 = new stde::factory\_find("A")();
// transformed by cxxe into: new A()
```

### Accessing exposed members

```cpp
obj->get\_member("health") = 4;
// transformed by cxxe into: obj->health = 4;
```

### Dynamic usage

All of these are resolved **at runtime**, so the name can come from a source that is unknown at compile time.

```cpp
stde::factory\_new(unknown\_string);
```

> \[!IMPORTANT]
> An `@exposed` member must be `public`.

## Custom decorators

You can declare your own decorators. A decorator receives the decorated function (`func`) and wraps it as needed.

```cpp
void @my\_decorator() -> func(int a) {
    before();
    func(a);
    after();
}
```

A decorator can also take **parameters**:

```cpp
void @my\_decorator(/\* decorator parameters \*/) -> func(int a) {
    before();
    func(a);
    after();
}
```

Usage:

```cpp
@my\_decorator
void work(int a) { /\* ... \*/ }
```

> \[!NOTE]
> Support for \*\*templates\*\* on decorators is planned for a future release.

## Lifecycle decorators

Three standard decorators document the evolution of your API.

|Decorator|Description|
|-|-|
|`@deprecated(msg)`|Marks an element as obsolete, with a message.|
|`@since(version)`|Indicates the version in which the element was introduced.|
|`@experimental(msg)`|Flags an unstable API that may change.|

```cpp
@since("1.2")
@deprecated("Use load\_v2() instead")
void load();

@experimental("The interface may change")
void new\_feature();
```

## Signals

A `signal` detects changes to a variable and runs a block of code whenever it is modified. It works anywhere: global scope, classes, structs, and so on.

```cpp
int my\_int = 0;

signal: my\_int {
    var\_changed();
}
```

Inside a class:

```cpp
class Player {
public:
    int score = 0;

    signal: score {
        update\_ui();
    }
};
```

## defer

`defer` runs a block when the **current scope exits**, regardless of the exit path.

```cpp
void process() {
    FILE\* f = fopen("data.txt", "r");
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

* `enum → string` conversion;
* `string → enum` conversion;
* other reflection utilities on the enum's values.

## Properties (native getters/setters)

Classes and structs natively support properties with `get` and `set`, without boilerplate.

```cpp
class Character {
    int health;

public:
    property Health {
        get { return health; }
        set { health = clamp(value, 0, 100); }
    }
};
```

Usage:

```cpp
Character c;
c.Health = 150;    // calls the setter: health = 100
int h = c.Health;  // calls the getter
```

## Output redirection (IOutput)

All outputs can be redirected natively, regardless of the mechanism used in the code:

* `std::cout`
* `printf`
* `std::print`
* and other standard outputs

Redirection is based on the **`IOutput`** interface: implement it to send output to a file, an editor console, the network, a log, and so on.

## CMake toolchain

C++Extended integrates with **CMake** through a dedicated toolchain. Extended sources are processed by `cxxe` before the regular C++ compilation, so the language can be adopted in existing projects without changing the build system.

## Concepts (roadmap)

> \[!WARNING]
> The following items are \*\*concepts\*\* under consideration. Their syntax and behavior are not final, and they are not yet implemented.

### dynamic type

A type whose actual type is known only at **runtime**, Python-style. It allows, for example, storing values of different types in a single container.

```cpp
std::vector<dynamic> items = { 42, "hello", 3.14 };
```

### Pattern matching

A `match` statement handles a value according to its type. Inside each branch, the value is already cast to the matching type.

```cpp
match value {
    int: printf("%d\\n", value);  // value is already an int here
}
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

* The examples in this documentation illustrate the syntax; the exact names of `stde` library functions may vary between versions.
* An `@exposed` member must be `public`.
* Features marked "Concept" are not yet implemented.

