#pragma once

#include <map>
#include <unordered_map>

#include "anduefker/ir/ReflectionIR.hpp"

namespace anduefker::generation
{
    struct CppTypeInfo
    {
        std::string name;
        int32_t size = 0;
    };

    struct CppEnumInfo
    {
        std::string name;
        ir::EnumUnderlyingType underlyingType = ir::EnumUnderlyingType::Unknown;
        int32_t size = 0;
        std::vector<std::string> values;
    };

    struct CppSymbols
    {
        std::unordered_map<uintptr_t, CppTypeInfo> types;
        std::unordered_map<uintptr_t, CppEnumInfo> enums;
        std::map<std::pair<uintptr_t, uintptr_t>, std::string> functions;
    };

    [[nodiscard]] std::string SanitizeIdentifier(std::string value, const char *fallback);
    [[nodiscard]] CppSymbols BuildCppSymbols(const ir::ReflectionIR &reflection);
    [[nodiscard]] const char *EnumUnderlyingName(ir::EnumUnderlyingType type);
    [[nodiscard]] std::string PropertyType(const ir::TypeReferenceIR &reference, const CppSymbols &symbols,
                                           int32_t pointerWidth, int32_t nameSize, size_t depth = 0);
} // namespace anduefker::generation
