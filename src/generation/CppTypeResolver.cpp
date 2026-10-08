#include "anduefker/generation/CppTypeResolver.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace anduefker::generation
{
    using ir::EnumUnderlyingType;
    using ir::PropertyKind;

    namespace
    {
        int32_t EnumSize(EnumUnderlyingType type)
        {
            switch (type)
            {
            case EnumUnderlyingType::Int8:
            case EnumUnderlyingType::UInt8:
                return 1;
            case EnumUnderlyingType::Int16:
            case EnumUnderlyingType::UInt16:
                return 2;
            case EnumUnderlyingType::Int32:
            case EnumUnderlyingType::UInt32:
                return 4;
            default:
                return 8;
            }
        }

        EnumUnderlyingType EnumType(const ir::EnumIR &enumeration)
        {
            const auto declared = enumeration.underlyingType;
            if (declared == EnumUnderlyingType::UInt64)
                return declared;
            if (enumeration.values.empty() || declared == EnumUnderlyingType::Unknown)
                return declared;
            int64_t low = enumeration.values.front().value;
            int64_t high = low;
            for (const auto &value : enumeration.values)
            {
                low = std::min(low, value.value);
                high = std::max(high, value.value);
            }
            const bool unsignedType = declared == EnumUnderlyingType::UInt8 || declared == EnumUnderlyingType::UInt16 ||
                                      declared == EnumUnderlyingType::UInt32 || declared == EnumUnderlyingType::UInt64;
            if (unsignedType && low < 0)
                return EnumUnderlyingType::Int64;
            const int32_t initialSize = EnumSize(declared);
            if (unsignedType)
            {
                if (initialSize <= 1 && static_cast<uint64_t>(high) <= UINT8_MAX)
                    return EnumUnderlyingType::UInt8;
                if (initialSize <= 2 && static_cast<uint64_t>(high) <= UINT16_MAX)
                    return EnumUnderlyingType::UInt16;
                if (initialSize <= 4 && static_cast<uint64_t>(high) <= UINT32_MAX)
                    return EnumUnderlyingType::UInt32;
                return EnumUnderlyingType::UInt64;
            }
            if (initialSize <= 1 && low >= INT8_MIN && high <= INT8_MAX)
                return EnumUnderlyingType::Int8;
            if (initialSize <= 2 && low >= INT16_MIN && high <= INT16_MAX)
                return EnumUnderlyingType::Int16;
            if (initialSize <= 4 && low >= INT32_MIN && high <= INT32_MAX)
                return EnumUnderlyingType::Int32;
            return EnumUnderlyingType::Int64;
        }

        std::string UniqueName(std::unordered_set<std::string> &used, std::string stem)
        {
            std::string name = stem;
            size_t suffix = 0;
            while (!used.insert(name).second)
                name = stem + "_" + std::to_string(++suffix);
            return name;
        }
    } // namespace

    std::string SanitizeIdentifier(std::string value, const char *fallback)
    {
        for (char &character : value)
        {
            const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') || character == '_';
            if (!valid)
                character = '_';
        }
        static const std::unordered_set<std::string> keywords = {
            "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
            "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
            "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
            "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
            "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
            "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
            "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
            "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
            "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
        if (value.empty() || (value.front() >= '0' && value.front() <= '9') || keywords.contains(value))
            value = std::string(fallback) + value;
        return value;
    }

    CppSymbols BuildCppSymbols(const ir::ReflectionIR &reflection)
    {
        CppSymbols result;
        std::unordered_set<std::string> used = {"FName", "FString", "FScriptInterface", "TargetAddress", "TArray", "TSet", "TMap"};
        // 所有声明和引用共用同一个符号表；重名后缀不依赖进程地址。
        for (const auto &type : reflection.types)
            result.types.emplace(type.address, CppTypeInfo{UniqueName(used, SanitizeIdentifier(type.name, "Type_")), type.size});
        for (const auto &enumeration : reflection.enums)
        {
            CppEnumInfo info;
            info.name = UniqueName(used, SanitizeIdentifier(enumeration.name, "Enum_"));
            info.underlyingType = EnumType(enumeration);
            info.size = EnumSize(info.underlyingType);
            std::unordered_set<std::string> valueNames;
            for (const auto &value : enumeration.values)
                info.values.push_back(UniqueName(valueNames, SanitizeIdentifier(value.name, "Value_")));
            result.enums.emplace(enumeration.address, std::move(info));
        }
        for (const auto &type : reflection.types)
        {
            for (const auto &function : type.functions)
            {
                const std::string stem = SanitizeIdentifier(result.types.at(type.address).name + "_" + function.name, "Function_");
                std::string name = stem;
                size_t suffix = 0;
                while (used.contains(name + "_Params") || used.contains(name + "_NativeRva") || used.contains(name + "_ParamsSize"))
                    name = stem + "_" + std::to_string(++suffix);
                used.insert(name + "_Params");
                used.insert(name + "_NativeRva");
                used.insert(name + "_ParamsSize");
                result.functions.emplace(std::pair{type.address, function.address}, std::move(name));
            }
        }
        return result;
    }

    const char *EnumUnderlyingName(EnumUnderlyingType type)
    {
        switch (type)
        {
        case EnumUnderlyingType::Int8:
            return "std::int8_t";
        case EnumUnderlyingType::UInt8:
            return "std::uint8_t";
        case EnumUnderlyingType::Int16:
            return "std::int16_t";
        case EnumUnderlyingType::UInt16:
            return "std::uint16_t";
        case EnumUnderlyingType::Int32:
            return "std::int32_t";
        case EnumUnderlyingType::UInt32:
            return "std::uint32_t";
        case EnumUnderlyingType::UInt64:
            return "std::uint64_t";
        default:
            return "std::int64_t";
        }
    }

    std::string PropertyType(const ir::TypeReferenceIR &reference, const CppSymbols &symbols,
                             int32_t pointerWidth, int32_t nameSize, size_t depth)
    {
        if (!reference.detailsResolved || reference.elementSize <= 0 || depth >= 32)
            return {};
        const auto sized = [&](const std::string &name, int32_t size)
        { return reference.elementSize == size ? name : std::string{}; };
        switch (reference.kind)
        {
        case PropertyKind::Int8:
            return sized("std::int8_t", 1);
        case PropertyKind::Int16:
            return sized("std::int16_t", 2);
        case PropertyKind::Int32:
            return sized("std::int32_t", 4);
        case PropertyKind::Int64:
            return sized("std::int64_t", 8);
        case PropertyKind::UInt16:
            return sized("std::uint16_t", 2);
        case PropertyKind::UInt32:
            return sized("std::uint32_t", 4);
        case PropertyKind::UInt64:
            return sized("std::uint64_t", 8);
        case PropertyKind::Bool:
        case PropertyKind::Byte:
            return sized("std::uint8_t", 1);
        case PropertyKind::Float:
            return sized("float", 4);
        case PropertyKind::Double:
            return sized("double", 8);
        case PropertyKind::Name:
            return sized("FName", nameSize);
        case PropertyKind::String:
            return sized("FString", pointerWidth + 8);
        case PropertyKind::Object:
        case PropertyKind::Class:
            if (const auto found = symbols.types.find(reference.referencedObject); found != symbols.types.end())
                return sized(found->second.name + "*", pointerWidth);
            break;
        case PropertyKind::Struct:
            if (const auto found = symbols.types.find(reference.referencedObject); found != symbols.types.end() && found->second.size > 0)
                return sized(found->second.name, found->second.size);
            break;
        case PropertyKind::Enum:
            if (const auto found = symbols.enums.find(reference.secondaryObject); found != symbols.enums.end())
                return sized(found->second.name, found->second.size);
            break;
        case PropertyKind::Interface:
            if (symbols.types.contains(reference.referencedObject))
                return sized("FScriptInterface", pointerWidth * 2);
            break;
        case PropertyKind::Array:
        case PropertyKind::Set:
            if (reference.inner)
            {
                const auto inner = PropertyType(*reference.inner, symbols, pointerWidth, nameSize, depth + 1);
                if (!inner.empty())
                    return reference.kind == PropertyKind::Array ? sized("TArray<" + inner + ">", pointerWidth + 8)
                                                                 : "TSet<" + inner + ", " + std::to_string(reference.elementSize) + ">";
            }
            break;
        case PropertyKind::Map:
            if (reference.key && reference.value)
            {
                const auto key = PropertyType(*reference.key, symbols, pointerWidth, nameSize, depth + 1);
                const auto value = PropertyType(*reference.value, symbols, pointerWidth, nameSize, depth + 1);
                if (!key.empty() && !value.empty())
                    return "TMap<" + key + ", " + value + ", " + std::to_string(reference.elementSize) + ">";
            }
            break;
        default:
            break;
        }
        return {};
    }
} // namespace anduefker::generation
