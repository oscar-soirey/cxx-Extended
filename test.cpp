#include "cxxe_runtime.hpp"
namespace cxxe_generated { static void register_all(); }
#include <iostream>

 class A {
public:
	int life;
	 int elife;
	 void EShow() { std::cout << "Hello" << std::endl; }
	void Show() { std::cout << "Hello" << std::endl; }
};

int main(int argc, char** argv) {
	A* myclass = (cxxe_generated::register_all(), stde::factory_new(argv[1]));
	(cxxe_generated::register_all(), stde::get_member(myclass, argv[2]))();
	return 0;
}

namespace cxxe_generated {
static void register_all() {
    static const bool initialized = []() {
        auto cxxe_class_3 = stde::register_class<A>("A");
        cxxe_class_3.expose_field("elife",
        [](void* self) -> stde::DynamicValue {
            auto* obj = static_cast<A*>(self);
            return stde::DynamicValue::from(obj->elife);
        },
        [](void* self, const std::any& value) {
            auto* obj = static_cast<A*>(self);
            using MemberType = std::decay_t<decltype(obj->elife)>;
            obj->elife = std::any_cast<MemberType>(value);
        });
        cxxe_class_3.expose_method("EShow",
        [](void* self, const std::vector<std::any>& args) -> stde::DynamicValue {
            if (args.size() != 0) throw std::runtime_error("CXXE: invalid argument count for exposed method");
            auto* obj = static_cast<A*>(self);
            obj->EShow();
            return stde::DynamicValue();
        });
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
