# Release:

\-Déclarations de décorateurs customs avec void @my\_decorator(/\*on peut également mettre des parametres au decorateur\*/) -> func(int a) { avant(); func(a); apres (); } (plus tard mettre aussi des templates au décorateurs)



\-Toolchain CMake



\-signal pour detecter les changements de variables ( avec signal: my\_int { var\_changed() } ), possible en global, dans une classe, dans une structure et tout



\-defer (defer { call\_functions(); })



\-enums introspectables (convertion enum -> string et string -> enum, et autre)



\-getter/setter natif dans les classes et structures (    property Health {

&#x20;       get { return health; }

&#x20;       set { health = clamp(value, 0, 100); } }



\-permettre de rediriger tous les outputs nativement (std::cout, printf, std::print, et autres) (donc faire une classe IOutput)



\-décorateurs : @deprecated(msg), @since(version), @experimental(msg)





# Concepts: 

\-type <dynamic> ou seul le runtime connait le type (comme en python quoi), ce qui permet de stocker différents types dans un seul container par exemple

\-pattern matching (match value { int: printf("%d\\n", value); //ici value est déja cast en tant que int



\-json/xml et autres formats de données -> controles natifs

\-serialisation automatique avec @serializable













# Déja implémentés:

\-Toutes les features du C++

\-Parametres nommés, eg. (int add(int a, int b);   add(b=5, a=4);)

\-Décorateurs natifs (@register, @exposed, et autres), utilisation: @register class A { public: @exposed int health; };

A\* myclass = stde::factory\_new("A");   //le compilateur remplace stde::factory\_new("A") par new A()

ou

A\* myclass = new stde::factory\_find("A")();   //le compilateur cxxe remplace stde::factory\_find("A") par A

myclass.get\_member("health") = 4;	//le compilateur remplace directement myclass->get\_member("health") par myclass->health

Il faut donc pour un member exposed qu'il soit public obligatoirement

Toutes les utilisation se font au runtime bien sur, on peut donc ecrire

stde::factory\_new(unkonwn\_string)



