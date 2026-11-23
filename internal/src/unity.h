#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace unity {

bool valid_ptr(uintptr_t p);
uintptr_t klass(uintptr_t obj);
std::string class_name(uintptr_t k);
bool is_class(uintptr_t obj, const char* name);
uintptr_t static_fields(uintptr_t k);
std::string read_string(uintptr_t str);
uintptr_t chain(uintptr_t base, std::initializer_list<uint32_t> offsets);

template<typename T>
T read(uintptr_t addr) {
    if (!valid_ptr(addr)) return T{};
    return *reinterpret_cast<T*>(addr);
}

template<typename T>
void write(uintptr_t addr, T val) {
    if (!valid_ptr(addr)) return;
    *reinterpret_cast<T*>(addr) = val;
}

struct DictEntry {
    int key;
    uintptr_t value;
};
std::vector<DictEntry> dict_iterate(uintptr_t dict, int max_entries = 512);

// il2cpp runtime resolution
bool init_il2cpp();
uintptr_t find_class(const char* ns, const char* name);
uintptr_t get_static_instance(const char* ns, const char* name);

} // namespace unity