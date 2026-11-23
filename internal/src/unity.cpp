#include "unity.h"
#include "offsets.h"
#include <windows.h>
#include <cstring>

namespace unity {

static HMODULE g_ga = nullptr;

// il2cpp function typedefs
using FnDomainGet = void*(*)();
using FnDomainGetAssemblies = void**(*)(void*, size_t*);
using FnAssemblyGetImage = void*(*)(void*);
using FnClassFromName = void*(*)(void*, const char*, const char*);
using FnClassGetStaticFieldData = void*(*)(void*);
using FnImageGetName = const char*(*)(void*);

static FnDomainGet pDomainGet = nullptr;
static FnDomainGetAssemblies pDomainGetAssemblies = nullptr;
static FnAssemblyGetImage pAssemblyGetImage = nullptr;
static FnClassFromName pClassFromName = nullptr;
static FnClassGetStaticFieldData pClassGetStaticFieldData = nullptr;
static FnImageGetName pImageGetName = nullptr;

bool valid_ptr(uintptr_t p) {
    return p >= 0x10000ULL && p < 0x00007FFFFFFFFFFFULL && (p & 7) == 0;
}

uintptr_t klass(uintptr_t obj) {
    if (!valid_ptr(obj)) return 0;
    uintptr_t k = read<uintptr_t>(obj);
    return valid_ptr(k) ? k : 0;
}

std::string class_name(uintptr_t k) {
    if (!valid_ptr(k)) return "";
    uintptr_t np = read<uintptr_t>(k + off::class_name);
    if (!valid_ptr(np)) return "";
    char buf[160]{};
    __try { memcpy(buf, reinterpret_cast<void*>(np), 159); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return ""; }
    buf[159] = 0;
    return std::string(buf);
}

bool is_class(uintptr_t obj, const char* name) {
    uintptr_t k = klass(obj);
    if (!k) return false;
    std::string cn = class_name(k);
    if (cn == name) return true;
    const char* dot = strrchr(name, '.');
    if (dot && cn == dot + 1) return true;
    return false;
}

uintptr_t static_fields(uintptr_t k) {
    if (!valid_ptr(k)) return 0;
    uintptr_t sf = read<uintptr_t>(k + off::class_static);
    return valid_ptr(sf) ? sf : 0;
}

std::string read_string(uintptr_t str) {
    if (!valid_ptr(str)) return "";
    int32_t len = read<int32_t>(str + off::str_len);
    if (len <= 0 || len > 4096) return "";
    if (len > 1024) len = 1024;
    wchar_t wbuf[1025]{};
    __try { memcpy(wbuf, reinterpret_cast<void*>(str + off::str_chars), len * 2); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return ""; }
    wbuf[len] = 0;
    char out[2048]{};
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, out, sizeof(out) - 1, nullptr, nullptr);
    if (n <= 0) return "";
    out[n] = 0;
    return std::string(out);
}

uintptr_t chain(uintptr_t base, std::initializer_list<uint32_t> offsets) {
    uintptr_t p = base;
    for (uint32_t o : offsets) {
        if (!valid_ptr(p)) return 0;
        p = read<uintptr_t>(p + o);
    }
    return valid_ptr(p) ? p : 0;
}

std::vector<DictEntry> dict_iterate(uintptr_t dict, int max_entries) {
    std::vector<DictEntry> result;
    if (!valid_ptr(dict)) return result;
    int32_t count = read<int32_t>(dict + off::dict_count);
    uintptr_t entries = read<uintptr_t>(dict + off::dict_entries);
    if (count <= 0 || count > 100000 || !valid_ptr(entries)) return result;
    if (count > max_entries) count = max_entries;
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        uintptr_t e = entries + off::arr_data + (uintptr_t)i * off::dict_entry_stride;
        int32_t hash = read<int32_t>(e);
        if (hash < 0) continue;
        int key = read<int32_t>(e + 0x08);
        uintptr_t val = read<uintptr_t>(e + off::dict_value_off);
        if (valid_ptr(val))
            result.push_back({key, val});
    }
    return result;
}

bool init_il2cpp() {
    g_ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!g_ga) return false;

    pDomainGet = (FnDomainGet)GetProcAddress(g_ga, "il2cpp_domain_get");
    pDomainGetAssemblies = (FnDomainGetAssemblies)GetProcAddress(g_ga, "il2cpp_domain_get_assemblies");
    pAssemblyGetImage = (FnAssemblyGetImage)GetProcAddress(g_ga, "il2cpp_assembly_get_image");
    pClassFromName = (FnClassFromName)GetProcAddress(g_ga, "il2cpp_class_from_name");
    pClassGetStaticFieldData = (FnClassGetStaticFieldData)GetProcAddress(g_ga, "il2cpp_class_get_static_field_data");
    pImageGetName = (FnImageGetName)GetProcAddress(g_ga, "il2cpp_image_get_name");

    return pDomainGet && pDomainGetAssemblies && pAssemblyGetImage &&
           pClassFromName && pClassGetStaticFieldData;
}

uintptr_t find_class(const char* ns, const char* name) {
    if (!pDomainGet) return 0;
    void* domain = pDomainGet();
    if (!domain) return 0;
    size_t count = 0;
    void** assemblies = pDomainGetAssemblies(domain, &count);
    if (!assemblies) return 0;
    for (size_t i = 0; i < count; ++i) {
        void* image = pAssemblyGetImage(assemblies[i]);
        if (!image) continue;
        void* k = pClassFromName(image, ns, name);
        if (k) return reinterpret_cast<uintptr_t>(k);
    }
    return 0;
}

uintptr_t get_static_instance(const char* ns, const char* name) {
    uintptr_t k = find_class(ns, name);
    if (!k) return 0;
    void* sf = pClassGetStaticFieldData(reinterpret_cast<void*>(k));
    if (!sf) return 0;
    uintptr_t inst = read<uintptr_t>(reinterpret_cast<uintptr_t>(sf));
    return valid_ptr(inst) ? inst : 0;
}

} // namespace unity