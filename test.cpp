#include "cxxe_runtime.hpp"
namespace cxxe_generated { static void register_all(); }
#include <iostream>
#include "test.he"


namespace test {
	class A {
	public:
		int health=10;

		decltype(auto) get_health() { 
				std::cout << "health get\n";
				return health;}
template <typename CXXEPropertyValue>
void set_health(CXXEPropertyValue&& value) {
				std::cout << "health set to: " << health << std::endl;
				health = value;}

	};

	 enum class Color {
		Red,
		Green = 4,
		Blue
	};
}


int main(int argc, char** argv)
{
	using namespace test;
	A mya{};
	std::cout << mya.get_health() << std::endl;
	mya.set_health(5);
	
	std::cout << stde::enum_to_string(Color::Green) << std::endl;
	Color c = stde::string_to_enum<Color>("Green");
	std::cout << stde::enum_has_value(c) << std::endl;
	std::cout << stde::enum_has_name<Color>("Green") << std::endl;
	std::vector<std::string> names = stde::enum_names<Color>();
	auto info = stde::get_enum_info<Color>();
	
	return 0;
}

namespace cxxe_generated {
static void register_all() {
    static const bool initialized = []() {
        auto cxxe_enum_11 = stde::register_enum<test::Color>("test::Color");
        cxxe_enum_11.value("Red", test::Color::Red);
        cxxe_enum_11.value("Green", test::Color::Green);
        cxxe_enum_11.value("Blue", test::Color::Blue);
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
