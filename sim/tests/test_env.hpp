#pragma once

// Test-only helper: is an environment variable set? (MSVC deprecates std::getenv.)

#include <cstdlib>

inline bool testEnvSet(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t len = 0;
    const bool set = _dupenv_s(&value, &len, name) == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}
