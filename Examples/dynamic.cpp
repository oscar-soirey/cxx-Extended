#include "cxxe_runtime.hpp"
namespace cxxe_generated { static void register_all(); }
#include <iostream>
#include <vector>

/*

dynamic value = 42;

if (value.is<int>()) {
    int x = value.as<int>();
    printf("int = %d\n", x);
}

dynamic value = 42;

printf("%d\n", value.as<int>());   // OK
printf("%f\n", value.as<float>()); // erreur d'exécution

dynamic value = 42;

printf("%d\n", value.as<int>());

value = 3.14f;

printf("%f\n", value.as<float>());

value = std::string("hello");

printf("%s\n", value.as<std::string>().c_str());



std::vector<dynamic> values;

values.push_back(42);
values.push_back(3.14f);
values.push_back(std::string("hello"));
values.push_back(true);


for (auto& value : values) {
    if (value.is<int>()) {
        printf("int: %d\n", value.as<int>());
    }

    if (value.is<float>()) {
        printf("float: %f\n", value.as<float>());
    }

    if (value.is<std::string>()) {
        printf("string: %s\n", value.as<std::string>().c_str());
    }

    if (value.is<bool>()) {
        printf("bool: %d\n", value.as<bool>());
    }
}

*/


void Show(stde::DynamicValue value)
{
    { auto&& __cxxe_match_value = (value);
    if (__cxxe_match_value.is<int>()) {
        auto&& value = __cxxe_match_value.as<int>();
std::cout << "Entier : " << value << std::endl;
    }
    else if (__cxxe_match_value.is<float>()) {
        auto&& value = __cxxe_match_value.as<float>();
std::cout << "Float : " << value << std::endl;
    }
    else if (__cxxe_match_value.is<const char*>()) {
        auto&& value = __cxxe_match_value.as<const char*>();
std::cout << "String : " << value << std::endl;
    }
    else {
std::cout << "Type inconnu" << std::endl;
    }
}

}


int main(int argc, char** argv)
{
	stde::DynamicValue value = 42;
	value.is<int>();
	value.as<int>();

	std::vector<stde::DynamicValue> dv = {42, 3.14f, "hello"};

	for (const auto& v : dv) {
		if (v.is<int>())
			std::cout << v.as<int>() << std::endl;
		else if (v.is<float>())
			std::cout << v.as<float>() << std::endl;
		else if (v.is<const char*>())
			std::cout << v.as<const char*>() << std::endl;
	}


	for(const auto& v: dv) 
	{
		Show(v);
	}
	
	return 0;
}

namespace cxxe_generated {
static void register_all() {
    static const bool initialized = []() {
        return true;
    }();
    (void)initialized;
}
}
namespace {
struct CxxeAutoRegistration {
    CxxeAutoRegistration() { cxxe_generated::register_all(); }
};
static CxxeAutoRegistration cxxe_auto_registration;
}
