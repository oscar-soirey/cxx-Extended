#ifndef CXXE_RUNTIME_HPP
#define CXXE_RUNTIME_HPP

#include <any>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace stde {

class DynamicValue {
    std::any value_;
public:
    DynamicValue() = default;
    explicit DynamicValue(std::any value) : value_(std::move(value)) {}

    template <class T>
    static DynamicValue from(T&& value) {
        return DynamicValue(std::any(std::forward<T>(value)));
    }

    bool has_value() const noexcept { return value_.has_value(); }

    template <class T>
    T get() const { return std::any_cast<T>(value_); }

    template <class T>
    operator T() const { return std::any_cast<T>(value_); }
};

namespace detail {

template <class T>
std::any pack_argument(T&& value) {
    if constexpr (std::is_lvalue_reference_v<T&&>) {
        using U = std::remove_reference_t<T>;
        if constexpr (std::is_const_v<U>) return std::any(std::cref(value));
        else return std::any(std::ref(value));
    } else {
        return std::any(std::forward<T>(value));
    }
}

template <class T>
decltype(auto) unpack_argument(const std::any& value) {
    using U = std::remove_reference_t<T>;
    using V = std::remove_cv_t<U>;

    if constexpr (std::is_lvalue_reference_v<T>) {
        if constexpr (std::is_const_v<U>) {
            if (value.type() == typeid(std::reference_wrapper<const V>))
                return std::any_cast<std::reference_wrapper<const V>>(value).get();
            return std::any_cast<std::reference_wrapper<V>>(value).get();
        } else {
            return std::any_cast<std::reference_wrapper<V>>(value).get();
        }
    } else if constexpr (std::is_rvalue_reference_v<T>) {
        return std::move(std::any_cast<V&>(const_cast<std::any&>(value)));
    } else {
        return std::any_cast<std::decay_t<T>>(value);
    }
}

} // namespace detail

struct MemberInfo {
    enum class Kind { Field, Method };
    using Getter = DynamicValue (*)(void*);
    using Setter = void (*)(void*, const std::any&);
    using Invoker = DynamicValue (*)(void*, const std::vector<std::any>&);

    Kind kind = Kind::Field;
    std::string name;
    Getter getter = nullptr;
    Setter setter = nullptr;
    Invoker invoker = nullptr;
};

struct ClassInfo {
    using Create = void* (*)();
    using Destroy = void (*)(void*);

    std::string name;
    std::type_index type = typeid(void);
    Create create = nullptr;
    Destroy destroy = nullptr;
    std::vector<std::shared_ptr<MemberInfo>> members;
};

struct EnumValueInfo {
    std::string name;
    std::any value;
};

struct EnumInfo {
    std::string name;
    std::type_index type = typeid(void);
    std::vector<EnumValueInfo> values;
};

class EnumRegistry {
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<EnumInfo>> enums_;

    EnumRegistry() = default;
public:
    EnumRegistry(const EnumRegistry&) = delete;
    EnumRegistry& operator=(const EnumRegistry&) = delete;

    static EnumRegistry& instance() {
        static EnumRegistry registry;
        return registry;
    }

    template <class E>
    std::shared_ptr<EnumInfo> register_enum(std::string_view name) {
        static_assert(std::is_enum_v<E>, "stde::register_enum requires an enum type");
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : enums_) {
            if (item->name == name && item->type == std::type_index(typeid(E))) {
                item->values.clear();
                return item;
            }
        }
        auto item = std::make_shared<EnumInfo>();
        item->name = std::string(name);
        item->type = std::type_index(typeid(E));
        enums_.push_back(item);
        return item;
    }

    std::shared_ptr<EnumInfo> find(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : enums_) if (item->name == name) return item;
        return {};
    }

    std::shared_ptr<EnumInfo> find_type(std::type_index type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : enums_) if (item->type == type) return item;
        return {};
    }
};

class Registry {
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ClassInfo>> classes_;

    Registry() = default;

public:
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    template <class T>
    std::shared_ptr<ClassInfo> register_class(std::string_view name) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : classes_) {
            if (item->name == name && item->type == std::type_index(typeid(T))) {
                item->create = make_create<T>();
                item->destroy = make_destroy<T>();
                item->members.clear();
                return item;
            }
        }

        auto item = std::make_shared<ClassInfo>();
        item->name = std::string(name);
        item->type = std::type_index(typeid(T));
        item->create = make_create<T>();
        item->destroy = make_destroy<T>();
        classes_.push_back(item);
        return item;
    }

    std::shared_ptr<ClassInfo> find(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : classes_)
            if (item->name == name) return item;
        return {};
    }

    std::shared_ptr<ClassInfo> find_type(std::type_index type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : classes_)
            if (item->type == type) return item;
        return {};
    }

private:
    template <class T>
    static ClassInfo::Create make_create() {
        return []() -> void* {
            if constexpr (std::is_default_constructible_v<T>) return static_cast<void*>(new T());
            else return nullptr;
        };
    }

    template <class T>
    static ClassInfo::Destroy make_destroy() {
        return [](void* p) { delete static_cast<T*>(p); };
    }
};

template <class T>
class ClassBuilder {
    std::shared_ptr<ClassInfo> info_;
public:
    explicit ClassBuilder(std::shared_ptr<ClassInfo> info) : info_(std::move(info)) {}

    ClassBuilder& expose_field(std::string_view name, MemberInfo::Getter getter, MemberInfo::Setter setter = nullptr) {
        auto member = std::make_shared<MemberInfo>();
        member->kind = MemberInfo::Kind::Field;
        member->name = std::string(name);
        member->getter = getter;
        member->setter = setter;
        info_->members.push_back(std::move(member));
        return *this;
    }

    ClassBuilder& expose_method(std::string_view name, MemberInfo::Invoker invoker) {
        auto member = std::make_shared<MemberInfo>();
        member->kind = MemberInfo::Kind::Method;
        member->name = std::string(name);
        member->invoker = invoker;
        info_->members.push_back(std::move(member));
        return *this;
    }
};

template <class T>
ClassBuilder<T> register_class(std::string_view name) {
    return ClassBuilder<T>(Registry::instance().register_class<T>(name));
}


template <class E>
class EnumBuilder {
    std::shared_ptr<EnumInfo> info_;
public:
    explicit EnumBuilder(std::shared_ptr<EnumInfo> info) : info_(std::move(info)) {}

    EnumBuilder& value(std::string_view name, E value) {
        if (!info_) throw std::runtime_error("CXXE: invalid enum builder");
        EnumValueInfo item;
        item.name = std::string(name);
        item.value = value;
        info_->values.push_back(std::move(item));
        return *this;
    }
};

template <class E>
EnumBuilder<E> register_enum(std::string_view name) {
    return EnumBuilder<E>(EnumRegistry::instance().register_enum<E>(name));
}

template <class E>
std::shared_ptr<EnumInfo> get_enum_info() {
    static_assert(std::is_enum_v<E>, "stde::get_enum_info requires an enum type");
    return EnumRegistry::instance().find_type(std::type_index(typeid(E)));
}

template <class E>
std::string enum_to_string(E value) {
    static_assert(std::is_enum_v<E>, "stde::enum_to_string requires an enum type");
    auto info = get_enum_info<E>();
    if (!info) throw std::runtime_error("CXXE: enum type is not registered");
    for (const auto& item : info->values) {
        if (std::any_cast<E>(item.value) == value) return item.name;
    }
    throw std::runtime_error("CXXE: enum value is not registered: " + std::to_string(static_cast<long long>(value)));
}

template <class E>
E string_to_enum(std::string_view name) {
    static_assert(std::is_enum_v<E>, "stde::string_to_enum requires an enum type");
    auto info = get_enum_info<E>();
    if (!info) throw std::runtime_error("CXXE: enum type is not registered");
    for (const auto& item : info->values) {
        if (item.name == name) return std::any_cast<E>(item.value);
    }
    throw std::runtime_error("CXXE: enum value not found: " + std::string(name));
}

template <class E>
bool enum_has_value(E value) {
    static_assert(std::is_enum_v<E>, "stde::enum_has_value requires an enum type");
    auto info = get_enum_info<E>();
    if (!info) return false;
    for (const auto& item : info->values) if (std::any_cast<E>(item.value) == value) return true;
    return false;
}

template <class E>
bool enum_has_name(std::string_view name) {
    static_assert(std::is_enum_v<E>, "stde::enum_has_name requires an enum type");
    auto info = get_enum_info<E>();
    if (!info) return false;
    for (const auto& item : info->values) if (item.name == name) return true;
    return false;
}

template <class E>
std::vector<std::string> enum_names() {
    static_assert(std::is_enum_v<E>, "stde::enum_names requires an enum type");
    auto info = get_enum_info<E>();
    if (!info) throw std::runtime_error("CXXE: enum type is not registered");
    std::vector<std::string> result;
    result.reserve(info->values.size());
    for (const auto& item : info->values) result.push_back(item.name);
    return result;
}

class FactoryResult {    void* pointer_ = nullptr;
    std::shared_ptr<ClassInfo> info_;
public:
    FactoryResult() = default;
    FactoryResult(void* pointer, std::shared_ptr<ClassInfo> info)
        : pointer_(pointer), info_(std::move(info)) {}

    void* raw() const noexcept { return pointer_; }

    template <class T>
    T* as() const {
        if (!pointer_) return nullptr;
        if (!info_ || info_->type != std::type_index(typeid(T)))
            throw std::runtime_error("CXXE factory type mismatch");
        return static_cast<T*>(pointer_);
    }

    template <class T>
    operator T*() const { return as<T>(); }
};

inline FactoryResult factory_new(std::string_view name) {
    auto info = Registry::instance().find(name);
    if (!info) throw std::runtime_error("CXXE: factory class not registered: " + std::string(name));
    if (!info->create) throw std::runtime_error("CXXE: class cannot be constructed: " + std::string(name));
    void* object = info->create();
    if (!object) throw std::runtime_error("CXXE: class has no default constructor: " + std::string(name));
    return FactoryResult(object, std::move(info));
}

inline std::shared_ptr<ClassInfo> factory_find(std::string_view name) {
    auto info = Registry::instance().find(name);
    if (!info) throw std::runtime_error("CXXE: factory class not registered: " + std::string(name));
    return info;
}

class MemberProxy {
    void* object_ = nullptr;
    std::type_index type_ = typeid(void);
    std::string name_;

    std::shared_ptr<ClassInfo> class_info() const {
        return Registry::instance().find_type(type_);
    }

    std::shared_ptr<MemberInfo> find_member() const {
        auto info = class_info();
        if (!info) throw std::runtime_error("CXXE: object class is not registered");
        for (const auto& member : info->members)
            if (member->name == name_) return member;
        throw std::runtime_error("CXXE: exposed member not found: " + name_);
    }

public:
    MemberProxy() = default;
    MemberProxy(void* object, std::type_index type, std::string_view name)
        : object_(object), type_(type), name_(name) {}

    template <class T>
    operator T() const {
        auto member = find_member();
        if (member->kind != MemberInfo::Kind::Field || !member->getter)
            throw std::runtime_error("CXXE: exposed member is not a field: " + name_);
        return member->getter(object_).template get<T>();
    }

    template <class T>
    MemberProxy& operator=(T&& value) {
        auto member = find_member();
        if (member->kind != MemberInfo::Kind::Field)
            throw std::runtime_error("CXXE: exposed member is not a field: " + name_);
        if (!member->setter)
            throw std::runtime_error("CXXE: exposed field is read-only: " + name_);
        member->setter(object_, std::any(std::forward<T>(value)));
        return *this;
    }

    template <class... Args>
    DynamicValue operator()(Args&&... args) const {
        auto info = class_info();
        if (!info) throw std::runtime_error("CXXE: object class is not registered");
        std::vector<std::any> packed;
        packed.reserve(sizeof...(Args));
        (packed.emplace_back(detail::pack_argument(std::forward<Args>(args))), ...);

        auto member = find_member();
        if (member->kind != MemberInfo::Kind::Method || !member->invoker)
            throw std::runtime_error("CXXE: exposed member is not a method: " + name_);

        try {
            return member->invoker(object_, packed);
        } catch (const std::bad_any_cast&) {
            throw;
        }
    }
};

template <class T>
MemberProxy get_member(T* object, std::string_view name) {
    if (!object) throw std::runtime_error("CXXE: get_member called with null object");
    return MemberProxy(static_cast<void*>(object), std::type_index(typeid(T)), name);
}

template <class T>
MemberProxy get_member(T& object, std::string_view name) {
    return MemberProxy(static_cast<void*>(std::addressof(object)), std::type_index(typeid(T)), name);
}

} // namespace stde

#endif
