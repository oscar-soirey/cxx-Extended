<p align="center">

&#x20; <img src="./assets/icon-transparent\_white\_line.png" alt="Logo C++Extended" width="160">

</p>



<h1 align="center">C++Extended (cxxe)</h1>



<p align="center">

&#x20; <em>Tout le C++ que vous aimez. Et tout ce qu'il vous manquait.</em>

</p>



\---



\## Introduction



Le C++ est puissant, mais il traîne des décennies de cérémonial : écrire de la

plomberie pour observer une variable, du boilerplate pour un getter, des

macros fragiles pour convertir un `enum` en chaîne, des `try/finally`

simulés à la main avec des destructeurs.



\*\*C++Extended\*\* (compilateur `cxxe`) est un sur-ensemble du C++ qui garde

\*\*100 % des fonctionnalités du C++\*\* et y ajoute une couche moderne

d'ergonomie : décorateurs, signaux, `defer`, propriétés natives, réflexion

à l'exécution et bien plus. Le principe est simple : `cxxe` transforme votre

code étendu en C++ standard, sans surcoût caché et sans rien retirer au

langage d'origine.



> Vous n'apprenez pas un nouveau langage. Vous débloquez celui que vous

> connaissez déjà.



\---



\## Sommaire



1\. \[Statut des fonctionnalités](#statut-des-fonctionnalités)

2\. \[Compatibilité C++](#compatibilité-c)

3\. \[Paramètres nommés](#paramètres-nommés)

4\. \[Décorateurs natifs et réflexion](#décorateurs-natifs-et-réflexion)

5\. \[Décorateurs personnalisés](#décorateurs-personnalisés)

6\. \[Décorateurs de cycle de vie](#décorateurs-de-cycle-de-vie)

7\. \[Signaux](#signaux)

8\. \[`defer`](#defer)

9\. \[Enums introspectables](#enums-introspectables)

10\. \[Propriétés (getter/setter natifs)](#propriétés-gettersetter-natifs)

11\. \[Redirection des sorties (`IOutput`)](#redirection-des-sorties-ioutput)

12\. \[Toolchain CMake](#toolchain-cmake)

13\. \[Concepts (roadmap)](#concepts-roadmap)

14\. \[Notes et limitations](#notes-et-limitations)



\---



\## Statut des fonctionnalités



| Fonctionnalité                                   | Statut          |

|--------------------------------------------------|-----------------|

| Toutes les fonctionnalités du C++                | ✅ Disponible   |

| Paramètres nommés                                | ✅ Disponible   |

| Décorateurs natifs (`@register`, `@exposed`, …)  | ✅ Disponible   |

| Décorateurs personnalisés                        | ✅ Disponible   |

| `@deprecated`, `@since`, `@experimental`         | ✅ Disponible   |

| Signaux (`signal:`)                              | ✅ Disponible   |

| `defer`                                          | ✅ Disponible   |

| Enums introspectables                            | ✅ Disponible   |

| Propriétés (`property`)                          | ✅ Disponible   |

| Redirection des sorties (`IOutput`)              | ✅ Disponible   |

| Toolchain CMake                                  | ✅ Disponible   |

| Templates sur les décorateurs                    | 🛠 Prévu        |

| Type `dynamic`                                   | 💡 Concept      |

| Pattern matching (`match`)                       | 💡 Concept      |

| JSON / XML / autres formats (contrôles natifs)   | 💡 Concept      |

| Sérialisation automatique (`@serializable`)      | 💡 Concept      |



\---



\## Compatibilité C++



C++Extended est un \*\*sur-ensemble\*\* du C++ : tout code C++ valide est un code

C++Extended valide. Vous pouvez migrer un projet existant fichier par fichier.



\---



\## Paramètres nommés



Les arguments peuvent être passés par nom, dans n'importe quel ordre.



```cpp

int add(int a, int b);



int main() {

&#x20;   int r = add(b=5, a=4);

}

```



\---



\## Décorateurs natifs et réflexion



Les décorateurs natifs permettent d'enregistrer des classes et d'exposer des

membres pour y accéder \*\*à l'exécution\*\* par leur nom.



| Décorateur  | Rôle                                                        |

|-------------|-------------------------------------------------------------|

| `@register` | Enregistre la classe dans la fabrique (`factory`).          |

| `@exposed`  | Expose un membre à la réflexion. \*\*Doit être `public`.\*\*    |



```cpp

@register

class A {

public:

&#x20;   @exposed int health;

};

```



\### Création d'instances



```cpp

// Instanciation par nom

A\* obj = stde::factory\_new("A");

// → transformé par cxxe en : new A()



// Récupération du type par nom

A\* obj2 = new stde::factory\_find("A")();

// → transformé par cxxe en : new A()

```



\### Accès aux membres exposés



```cpp

obj->get\_member("health") = 4;

// → transformé par cxxe en : obj->health = 4;

```



\### Utilisation dynamique



Toutes ces utilisations sont résolues \*\*à l'exécution\*\*. Le nom peut donc

provenir d'une source inconnue à la compilation :



```cpp

stde::factory\_new(unknown\_string);

```



> ⚠️ Un membre `@exposed` doit obligatoirement être `public`.



\---



\## Décorateurs personnalisés



Vous pouvez déclarer vos propres décorateurs. Un décorateur reçoit la fonction

décorée (`func`) et l'enveloppe à votre guise.



```cpp

void @my\_decorator() -> func(int a) {

&#x20;   avant();

&#x20;   func(a);

&#x20;   apres();

}

```



Un décorateur peut également prendre des \*\*paramètres\*\* :



```cpp

void @my\_decorator(/\* paramètres du décorateur \*/) -> func(int a) {

&#x20;   avant();

&#x20;   func(a);

&#x20;   apres();

}

```



Utilisation :



```cpp

@my\_decorator

void work(int a) { /\* ... \*/ }

```



> 🛠 Le support des \*\*templates\*\* sur les décorateurs est prévu dans une

> prochaine version.



\---



\## Décorateurs de cycle de vie



Trois décorateurs standard documentent et contrôlent l'évolution de votre API.



| Décorateur              | Description                                                 |

|-------------------------|-------------------------------------------------------------|

| `@deprecated(msg)`      | Marque un élément comme obsolète, avec un message.          |

| `@since(version)`       | Indique la version d'introduction de l'élément.             |

| `@experimental(msg)`    | Signale une API instable, susceptible de changer.           |



```cpp

@since("1.2")

@deprecated("Utilisez load\_v2()")

void load();



@experimental("L'interface peut changer")

void new\_feature();

```



\---



\## Signaux



Un `signal` détecte les changements d'une variable et exécute un bloc de code

lorsqu'elle est modifiée. Il fonctionne \*\*partout\*\* : portée globale, classe,

structure, etc.



```cpp

int my\_int = 0;



signal: my\_int {

&#x20;   var\_changed();

}

```



Dans une classe :



```cpp

class Player {

public:

&#x20;   int score = 0;



&#x20;   signal: score {

&#x20;       update\_ui();

&#x20;   }

};

```



\---



\## `defer`



`defer` exécute un bloc à la \*\*sortie de la portée courante\*\*, quel que soit

le chemin de sortie (retour, exception, etc.).



```cpp

void process() {

&#x20;   FILE\* f = fopen("data.txt", "r");

&#x20;   defer { fclose(f); }



&#x20;   // ... utilisation de f ...

}   // fclose(f) est appelé ici

```



```cpp

defer { call\_functions(); }

```



\---



\## Enums introspectables



Les enums sont introspectables : conversion `enum → string`, `string → enum`,

et autres opérations de réflexion.



```cpp

enum class Color { Red, Green, Blue };

```



Les fonctions d'introspection disponibles couvrent notamment :



\- la conversion `enum → string` ;

\- la conversion `string → enum` ;

\- d'autres utilitaires de réflexion sur les valeurs de l'enum.



> Consultez la référence de la bibliothèque `stde` pour la liste exacte des

> fonctions exposées.



\---



\## Propriétés (getter/setter natifs)



Les classes et les structures supportent nativement les propriétés avec

`get` et `set`, sans boilerplate.



```cpp

class Character {

&#x20;   int health;



public:

&#x20;   property Health {

&#x20;       get { return health; }

&#x20;       set { health = clamp(value, 0, 100); }

&#x20;   }

};

```



Utilisation :



```cpp

Character c;

c.Health = 150;          // appelle le setter → health = 100

int h = c.Health;        // appelle le getter

```



\---



\## Redirection des sorties (`IOutput`)



Toutes les sorties peuvent être redirigées nativement, quel que soit

le mécanisme utilisé dans le code :



\- `std::cout`

\- `printf`

\- `std::print`

\- et les autres sorties standard



La redirection repose sur l'interface \*\*`IOutput`\*\* : implémentez-la pour

envoyer les sorties vers un fichier, une console d'éditeur, un réseau, un

journal, etc.



\---



\## Toolchain CMake



C++Extended s'intègre à \*\*CMake\*\* via une toolchain dédiée. Les sources

étendues sont traitées par `cxxe` avant la compilation C++ classique, ce

qui permet d'adopter le langage dans vos projets existants sans changer votre

système de build.



\---



\## Concepts (roadmap)



Les éléments suivants sont des \*\*concepts\*\* à l'étude. Leur syntaxe et leur

comportement ne sont pas figés.



\### Type `dynamic`



Un type dont seul le \*\*runtime\*\* connaît le type réel, à la manière de Python.

Il permet par exemple de stocker des valeurs de types différents dans un même

conteneur.



```cpp

std::vector<dynamic> items = { 42, "hello", 3.14 };

```



\### Pattern matching



Un `match` permet de traiter une valeur selon son type. Dans chaque branche,

la valeur est déjà convertie vers le type correspondant.



```cpp

match value {

&#x20;   int: printf("%d\\n", value);   // value est déjà un int ici

}

```



\### JSON, XML et autres formats de données



Des contrôles natifs pour manipuler les formats de données courants (JSON, XML,

etc.) directement depuis le langage.



\### Sérialisation automatique



Le décorateur `@serializable` générera automatiquement la sérialisation et la

désérialisation d'une classe.



```cpp

@serializable

class Config {

public:

&#x20;   int width;

&#x20;   int height;

};

```



\---



\## Notes et limitations



\- Les exemples de cette documentation illustrent la syntaxe ; les noms exacts

&#x20; des fonctions de la bibliothèque `stde` peuvent varier selon la version.

\- Un membre `@exposed` doit être `public`.

\- Les fonctionnalités marquées « Concept » ne sont pas encore implémentées.



\---



<p align="center"><sub>C++Extended — Documentation</sub></p>



