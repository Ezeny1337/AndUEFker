#include "anduefker/generation/CppTypeResolver.hpp"
#include "anduefker/ir/ReflectionLayout.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
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
        std::unordered_set<std::string> used = {"FName", "FString", "FScriptInterface", "TargetAddress", "TArray", "TSet", "TMap", "TEnumStorage", "TOpaqueStorage", "TStaticArrayStorage", "FTextStorage", "TWeakObjectStorage", "TLazyObjectStorage", "TSoftObjectStorage", "TSoftClassStorage", "TFieldPathStorage", "TDelegateStorage", "TMulticastDelegateStorage", "TOptionalStorage"};
        // 所有声明和引用共用同一个符号表；重名后缀不依赖进程地址
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
        std::vector<const ir::FunctionIR *> functions;
        functions.reserve(reflection.functions.size());
        for (const auto &[address, function] : reflection.functions)
        {
            (void)address;
            if (function.headerReadable)
                functions.push_back(&function);
        }
        std::sort(functions.begin(), functions.end(), [](const auto *left, const auto *right)
                  { return std::tie(left->fullName, left->address) < std::tie(right->fullName, right->address); });
        for (const auto *definition : functions)
        {
            const auto &function = *definition;
            const auto owner = result.types.find(function.outerAddress);
            const std::string ownerName = owner == result.types.end() ? function.outerFullName : owner->second.name;
            const std::string stem = SanitizeIdentifier(ownerName + "_" + function.name, "Function_");
            std::string name = stem;
            size_t suffix = 0;
            static constexpr std::array<const char *, 7> endings = {
                "_Params", "_ParamsSize", "_ExecEntryRva", "_HasExecEntryRva", "_NativeExecRva", "_HasNativeExecRva", "_Signature"};
            const auto conflicts = [&]
            {
                return std::any_of(endings.begin(), endings.end(), [&](const char *ending)
                                   { return used.contains(name + ending); });
            };
            while (conflicts())
                name = stem + "_" + std::to_string(++suffix);
            for (const auto &ending : endings)
                used.insert(name + ending);
            result.functions.emplace(function.address, std::move(name));
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

    const char *PropertyStorageKindName(PropertyStorageKind kind)
    {
        switch (kind)
        {
        case PropertyStorageKind::Unavailable:
            return "unavailable";
        case PropertyStorageKind::SizedDescription:
            return "sized-description";
        case PropertyStorageKind::BoolMask:
            return "bool-mask";
        case PropertyStorageKind::TypedOpaque:
            return "typed-opaque";
        case PropertyStorageKind::PartialContainer:
            return "partial-container";
        }
        return "unavailable";
    }

    static CppPropertyType ResolveSizedType(const ir::TypeReferenceIR &reference, const CppSymbols &symbols,
                                            int32_t pointerWidth, int32_t nameSize, size_t depth)
    {
        if (!reference.detailsResolved)
            return {{}, "unresolved-type-details"};
        if (reference.elementSize <= 0)
            return {{}, "invalid-element-size"};
        if (depth >= 32)
            return {{}, "type-depth-limit"};
        const auto sized = [&](const std::string &name, int32_t size)
        {
            return reference.elementSize == size ? CppPropertyType{name, {}} : CppPropertyType{{}, "size-mismatch:expected=" + std::to_string(size) + ":observed=" + std::to_string(reference.elementSize)};
        };
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
            return sized("std::uint8_t", 1);
        case PropertyKind::Byte:
            if (reference.referencedObject != 0)
            {
                const auto found = symbols.enums.find(reference.referencedObject);
                if (found == symbols.enums.end())
                    return {{}, "missing-enum-symbol"};
                const auto &enumeration = found->second;
                return sized(enumeration.underlyingType == EnumUnderlyingType::UInt8 && enumeration.size == 1
                                 ? enumeration.name
                                 : "TEnumStorage<" + enumeration.name + ", std::uint8_t>",
                             1);
            }
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
            return {{}, "missing-type-symbol"};
        case PropertyKind::Struct:
            if (const auto found = symbols.types.find(reference.referencedObject); found != symbols.types.end())
            {
                if (found->second.size <= 0)
                    return {{}, "invalid-referenced-type-size"};
                return sized(found->second.name, found->second.size);
            }
            return {{}, "missing-type-symbol"};
        case PropertyKind::Enum:
            if (const auto found = symbols.enums.find(reference.secondaryObject); found != symbols.enums.end())
            {
                if (!reference.inner || reference.inner->elementSize != reference.elementSize)
                    return {{}, "invalid-enum-storage"};
                EnumUnderlyingType storage = EnumUnderlyingType::Unknown;
                switch (reference.inner->kind)
                {
                case PropertyKind::Int8:
                    storage = EnumUnderlyingType::Int8;
                    break;
                case PropertyKind::Byte:
                    storage = EnumUnderlyingType::UInt8;
                    break;
                case PropertyKind::Int16:
                    storage = EnumUnderlyingType::Int16;
                    break;
                case PropertyKind::UInt16:
                    storage = EnumUnderlyingType::UInt16;
                    break;
                case PropertyKind::Int32:
                    storage = EnumUnderlyingType::Int32;
                    break;
                case PropertyKind::UInt32:
                    storage = EnumUnderlyingType::UInt32;
                    break;
                case PropertyKind::Int64:
                    storage = EnumUnderlyingType::Int64;
                    break;
                case PropertyKind::UInt64:
                    storage = EnumUnderlyingType::UInt64;
                    break;
                default:
                    return {{}, "invalid-enum-storage"};
                }
                const auto inner = ResolvePropertyType(*reference.inner, symbols, pointerWidth, nameSize, depth + 1);
                if (inner.name.empty())
                    return {{}, "inner:" + inner.failureReason};
                if (storage == found->second.underlyingType && found->second.size == reference.elementSize)
                    return {found->second.name, {}};
                return {"TEnumStorage<" + found->second.name + ", " + inner.name + ">", {}};
            }
            return {{}, "missing-enum-symbol"};
        case PropertyKind::Interface:
            if (symbols.types.contains(reference.referencedObject))
                return sized("FScriptInterface", pointerWidth * 2);
            return {{}, "missing-type-symbol"};
        default:
            return {{}, "unsupported-property-kind"};
        }
    }

    CppPropertyType ResolvePropertyType(const ir::TypeReferenceIR &reference, const CppSymbols &symbols,
                                        int32_t pointerWidth, int32_t nameSize, size_t depth)
    {
        if (depth >= 32)
            return {{}, "type-depth-limit"};
        CppPropertyType result = ResolveSizedType(reference, symbols, pointerWidth, nameSize, depth);
        result.semanticsResolved = reference.detailsResolved;
        result.semanticName = result.name.empty() ? reference.reflectedClass : result.name;
        if (!result.name.empty())
        {
            const auto enumeration = symbols.enums.find(reference.kind == PropertyKind::Byte ? reference.referencedObject : reference.secondaryObject);
            if ((reference.kind == PropertyKind::Byte || reference.kind == PropertyKind::Enum) && enumeration != symbols.enums.end())
                result.semanticName = enumeration->second.name;
            result.storage = PropertyStorageKind::SizedDescription;
            return result;
        }
        if ((!reference.detailsResolved && !reference.nodeDetailsResolved) || reference.elementSize <= 0)
            return result;
        const std::string size = std::to_string(reference.elementSize);
        const auto object = symbols.types.find(reference.referencedObject);
        const std::string target = object == symbols.types.end() ? "void" : object->second.name;
        const auto opaque = [&](const std::string &wrapper, const std::string &semantic, const std::string &argument = "")
        {
            result.name = wrapper + "<" + argument + size + ">";
            result.semanticName = semantic;
            result.storage = PropertyStorageKind::TypedOpaque;
            result.failureReason = "internals-not-expanded";
        };
        const auto child = [&](const std::shared_ptr<ir::TypeReferenceIR> &node)
        {
            CppPropertyType type = node ? ResolvePropertyType(*node, symbols, pointerWidth, nameSize, depth + 1)
                                        : CppPropertyType{{}, "missing-child-type"};
            if (node && type.name.empty() && node->elementSize > 0)
            {
                // 保留已知的子项分类与反射范围，同时不对内部结构的有效性做任何断言
                type.name = "TOpaqueStorage<" + std::to_string(node->elementSize) + ">";
                type.semanticName = node->reflectedClass.empty() ? "Unknown" : node->reflectedClass;
                type.storage = PropertyStorageKind::TypedOpaque;
            }
            if (node && !type.name.empty() && node->arrayDim > 1)
            {
                type.name = "TStaticArrayStorage<" + type.name + ", " + std::to_string(node->elementSize) +
                            ", " + std::to_string(node->arrayDim) + ">";
                type.semanticName += "[" + std::to_string(node->arrayDim) + "]";
                type.storage = PropertyStorageKind::TypedOpaque;
                type.failureReason = type.failureReason.empty() ? "static-array-storage-not-expanded"
                                                                : "static-array-storage-not-expanded:" + type.failureReason;
            }
            if (node && node->arrayDim <= 0)
                type = {{}, "invalid-child-array-dim"};
            return type;
        };
        switch (reference.kind)
        {
        case PropertyKind::Text:
            opaque("FTextStorage", "FText");
            break;
        case PropertyKind::WeakObject:
            if (object == symbols.types.end())
            {
                result.failureReason = "missing-type-symbol";
                break;
            }
            opaque("TWeakObjectStorage", "TWeakObjectPtr<" + target + ">", target + ", ");
            break;
        case PropertyKind::LazyObject:
            if (object == symbols.types.end())
            {
                result.failureReason = "missing-type-symbol";
                break;
            }
            opaque("TLazyObjectStorage", "TLazyObjectPtr<" + target + ">", target + ", ");
            break;
        case PropertyKind::SoftObject:
            if (object == symbols.types.end())
            {
                result.failureReason = "missing-type-symbol";
                break;
            }
            opaque("TSoftObjectStorage", "TSoftObjectPtr<" + target + ">", target + ", ");
            break;
        case PropertyKind::SoftClass:
            if (object == symbols.types.end())
            {
                result.failureReason = "missing-type-symbol";
                break;
            }
            opaque("TSoftClassStorage", "TSoftClassPtr<" + target + ">", target + ", ");
            break;
        case PropertyKind::FieldPath:
            opaque("TFieldPathStorage", reference.reflectedClass);
            break;
        case PropertyKind::Delegate:
        case PropertyKind::MulticastDelegate:
        {
            const auto signature = symbols.functions.find(reference.referencedObject);
            if (signature == symbols.functions.end())
            {
                result.failureReason = "missing-signature-symbol";
                break;
            }
            const std::string tag = signature->second + "_Signature";
            opaque(reference.kind == PropertyKind::Delegate ? "TDelegateStorage" : "TMulticastDelegateStorage",
                   reference.reflectedClass + "<" + tag + ">", tag + ", ");
            break;
        }
        case PropertyKind::Array:
        case PropertyKind::Set:
        case PropertyKind::Optional:
        {
            const auto inner = child(reference.inner);
            if (inner.name.empty())
            {
                result.failureReason = "inner:" + inner.failureReason;
                break;
            }
            const std::string wrapper = reference.kind == PropertyKind::Array ? "TArray" : reference.kind == PropertyKind::Set ? "TSet"
                                                                                                                               : "TOptionalStorage";
            opaque(wrapper, wrapper + "<" + inner.semanticName + ">", inner.name + ", ");
            result.storage = inner.IsOpaque() ? PropertyStorageKind::PartialContainer : PropertyStorageKind::TypedOpaque;
            result.failureReason = inner.IsOpaque() ? "inner:" + inner.failureReason : "container-internals-not-expanded";
            break;
        }
        case PropertyKind::Map:
        {
            const auto key = child(reference.key);
            const auto value = child(reference.value);
            if (key.name.empty() || value.name.empty())
            {
                result.failureReason = key.name.empty() ? "key:" + key.failureReason : "value:" + value.failureReason;
                break;
            }
            opaque("TMap", "TMap<" + key.semanticName + ", " + value.semanticName + ">", key.name + ", " + value.name + ", ");
            result.storage = key.IsOpaque() || value.IsOpaque() ? PropertyStorageKind::PartialContainer : PropertyStorageKind::TypedOpaque;
            result.failureReason = key.IsOpaque() ? "key:" + key.failureReason : value.IsOpaque() ? "value:" + value.failureReason
                                                                                                  : "container-internals-not-expanded";
            break;
        }
        default:
            break;
        }
        return result;
    }

    FieldDescription DescribeField(const ir::PropertyIR &property, int32_t bound,
                                   const CppSymbols &symbols, int32_t pointerWidth, int32_t nameSize)
    {
        FieldDescription result;
        result.property = &property;
        result.type.semanticName = property.reflectedClass;
        result.type.semanticsResolved = property.typeDetailsResolved;
        int64_t end = 0;
        result.validBounds = ir::IsValidPropertyBounds(property, bound, end);
        result.boolLayout = ir::IsValidPropertyBoolLayout(property);
        if (property.type.elementSize != property.elementSize)
            result.type.failureReason = "property-type-size-disagreement";
        else if (property.type.kind == PropertyKind::Bool)
        {
            result.type.semanticName = "bool";
            result.type.semanticsResolved = property.typeDetailsResolved;
            result.type.storage = result.boolLayout ? PropertyStorageKind::BoolMask : PropertyStorageKind::Unavailable;
            if (!result.boolLayout)
                result.type.failureReason = "invalid-bool-layout";
        }
        else
            result.type = ResolvePropertyType(property.type, symbols, pointerWidth, nameSize);
        return result;
    }
} // namespace anduefker::generation
