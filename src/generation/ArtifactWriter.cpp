#include "anduefker/generation/ArtifactWriter.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anduefker::generation
{
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::binding::NameContainerKind;
    using ::anduefker::binding::ObjectContainerKind;
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::ue::EngineSchema;

    namespace
    {
        std::string StatusName(ParseStatus status)
        {
            switch (status)
            {
            case ParseStatus::Complete:
                return "Complete";
            case ParseStatus::Partial:
                return "Partial";
            case ParseStatus::Failed:
                return "Failed";
            }
            return "Failed";
        }

        std::string Hex(uintptr_t value)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << value;
            return stream.str();
        }

        std::string PropertyType(const PropertyIR &property,
                                 const std::unordered_map<uintptr_t, std::string> &types,
                                 const std::unordered_map<uintptr_t, std::string> &enums,
                                 bool &resolved)
        {
            resolved = false;
            const std::function<std::string(const TypeReferenceIR &, bool &)> nestedType =
                [&](const TypeReferenceIR &reference, bool &nestedResolved) -> std::string
            {
                nestedResolved = false;
                switch (reference.kind)
                {
                case PropertyKind::Int8:
                    if (reference.elementSize == 1)
                    {
                        nestedResolved = true;
                        return "std::int8_t";
                    }
                    break;
                case PropertyKind::Int16:
                    if (reference.elementSize == 2)
                    {
                        nestedResolved = true;
                        return "std::int16_t";
                    }
                    break;
                case PropertyKind::Int32:
                    if (reference.elementSize == 4)
                    {
                        nestedResolved = true;
                        return "std::int32_t";
                    }
                    break;
                case PropertyKind::Int64:
                    if (reference.elementSize == 8)
                    {
                        nestedResolved = true;
                        return "std::int64_t";
                    }
                    break;
                case PropertyKind::UInt16:
                    if (reference.elementSize == 2)
                    {
                        nestedResolved = true;
                        return "std::uint16_t";
                    }
                    break;
                case PropertyKind::UInt32:
                    if (reference.elementSize == 4)
                    {
                        nestedResolved = true;
                        return "std::uint32_t";
                    }
                    break;
                case PropertyKind::UInt64:
                    if (reference.elementSize == 8)
                    {
                        nestedResolved = true;
                        return "std::uint64_t";
                    }
                    break;
                case PropertyKind::Byte:
                    if (reference.elementSize == 1)
                    {
                        nestedResolved = true;
                        return "std::uint8_t";
                    }
                    break;
                case PropertyKind::Float:
                    if (reference.elementSize == 4)
                    {
                        nestedResolved = true;
                        return "float";
                    }
                    break;
                case PropertyKind::Double:
                    if (reference.elementSize == 8)
                    {
                        nestedResolved = true;
                        return "double";
                    }
                    break;
                case PropertyKind::Name:
                    if (reference.elementSize == 4 || reference.elementSize == 8 || reference.elementSize == 0xC)
                    {
                        nestedResolved = true;
                        return "FName";
                    }
                    break;
                case PropertyKind::String:
                    if (reference.elementSize == 0x10)
                    {
                        nestedResolved = true;
                        return "FString";
                    }
                    break;
                case PropertyKind::Struct:
                    if (const auto found = types.find(reference.referencedObject); found != types.end())
                    {
                        nestedResolved = true;
                        return found->second;
                    }
                    break;
                case PropertyKind::Object:
                case PropertyKind::Class:
                case PropertyKind::SoftObject:
                case PropertyKind::SoftClass:
                case PropertyKind::WeakObject:
                case PropertyKind::LazyObject:
                case PropertyKind::Interface:
                    if (const auto found = types.find(reference.referencedObject); found != types.end())
                    {
                        nestedResolved = true;
                        return found->second + "*";
                    }
                    break;
                case PropertyKind::Enum:
                    if (const auto found = enums.find(reference.secondaryObject); found != enums.end())
                    {
                        nestedResolved = true;
                        return found->second;
                    }
                    break;
                case PropertyKind::Array:
                    if (reference.inner)
                    {
                        bool innerResolved = false;
                        const std::string inner = nestedType(*reference.inner, innerResolved);
                        if (innerResolved)
                        {
                            nestedResolved = true;
                            return "TArray<" + inner + ">";
                        }
                    }
                    break;
                case PropertyKind::Set:
                    if (reference.inner)
                    {
                        bool innerResolved = false;
                        const std::string inner = nestedType(*reference.inner, innerResolved);
                        if (innerResolved)
                        {
                            nestedResolved = true;
                            return "TSet<" + inner + ">";
                        }
                    }
                    break;
                case PropertyKind::Map:
                    if (reference.key && reference.value)
                    {
                        bool keyResolved = false;
                        bool valueResolved = false;
                        const std::string key = nestedType(*reference.key, keyResolved);
                        const std::string value = nestedType(*reference.value, valueResolved);
                        if (keyResolved && valueResolved)
                        {
                            nestedResolved = true;
                            return "TMap<" + key + ", " + value + ">";
                        }
                    }
                    break;
                default:
                    break;
                }
                return {};
            };
            switch (property.type.kind)
            {
            case PropertyKind::Int8:
                if (property.elementSize == 1)
                {
                    resolved = true;
                    return "std::int8_t";
                }
                break;
            case PropertyKind::Int16:
                if (property.elementSize == 2)
                {
                    resolved = true;
                    return "std::int16_t";
                }
                break;
            case PropertyKind::Int32:
                if (property.elementSize == 4)
                {
                    resolved = true;
                    return "std::int32_t";
                }
                break;
            case PropertyKind::Int64:
                if (property.elementSize == 8)
                {
                    resolved = true;
                    return "std::int64_t";
                }
                break;
            case PropertyKind::UInt16:
                if (property.elementSize == 2)
                {
                    resolved = true;
                    return "std::uint16_t";
                }
                break;
            case PropertyKind::UInt32:
                if (property.elementSize == 4)
                {
                    resolved = true;
                    return "std::uint32_t";
                }
                break;
            case PropertyKind::UInt64:
                if (property.elementSize == 8)
                {
                    resolved = true;
                    return "std::uint64_t";
                }
                break;
            case PropertyKind::Byte:
                if (property.elementSize == 1)
                {
                    resolved = true;
                    return "std::uint8_t";
                }
                break;
            case PropertyKind::Float:
                if (property.elementSize == 4)
                {
                    resolved = true;
                    return "float";
                }
                break;
            case PropertyKind::Double:
                if (property.elementSize == 8)
                {
                    resolved = true;
                    return "double";
                }
                break;
            case PropertyKind::Bool:
                if (property.elementSize == 1)
                {
                    resolved = true;
                    return "std::uint8_t";
                }
                break;
            case PropertyKind::Name:
                if (property.elementSize == 4 || property.elementSize == 8 || property.elementSize == 0xC)
                {
                    resolved = true;
                    return "FName";
                }
                break;
            case PropertyKind::String:
                if (property.elementSize == 0x10)
                {
                    resolved = true;
                    return "FString";
                }
                break;
            case PropertyKind::Object:
            case PropertyKind::Class:
            case PropertyKind::SoftObject:
            case PropertyKind::SoftClass:
            case PropertyKind::WeakObject:
            case PropertyKind::LazyObject:
                if (property.elementSize == static_cast<int32_t>(sizeof(uintptr_t)) && property.type.referencedObject != 0)
                {
                    const auto found = types.find(property.type.referencedObject);
                    if (found != types.end())
                    {
                        resolved = true;
                        return found->second + "*";
                    }
                }
                break;
            case PropertyKind::Struct:
                if (property.type.referencedObject != 0)
                {
                    const auto found = types.find(property.type.referencedObject);
                    if (found != types.end())
                    {
                        resolved = true;
                        return found->second;
                    }
                }
                break;
            case PropertyKind::Enum:
                if (const auto found = enums.find(property.type.secondaryObject); found != enums.end())
                {
                    resolved = true;
                    return found->second;
                }
                break;
            case PropertyKind::Array:
            case PropertyKind::Set:
                if (property.type.inner)
                {
                    bool innerResolved = false;
                    const std::string inner = nestedType(*property.type.inner, innerResolved);
                    if (innerResolved)
                    {
                        resolved = true;
                        return property.type.kind == PropertyKind::Array ? "TArray<" + inner + ">" : "TSet<" + inner + ">";
                    }
                }
                break;
            case PropertyKind::Map:
                if (property.type.key && property.type.value)
                {
                    bool keyResolved = false;
                    bool valueResolved = false;
                    const std::string key = nestedType(*property.type.key, keyResolved);
                    const std::string value = nestedType(*property.type.value, valueResolved);
                    if (keyResolved && valueResolved)
                    {
                        resolved = true;
                        return "TMap<" + key + ", " + value + ">";
                    }
                }
                break;
            case PropertyKind::Interface:
                if (const auto found = types.find(property.type.referencedObject); found != types.end())
                {
                    resolved = true;
                    return found->second + "*";
                }
                break;
            default:
                break;
            }
            return {};
        }

        const char *ParseStatusName(ParseStatus status)
        {
            switch (status)
            {
            case ParseStatus::Complete:
                return "Complete";
            case ParseStatus::Partial:
                return "Partial";
            case ParseStatus::Failed:
                return "Failed";
            }
            return "Failed";
        }

        const char *TypeKindName(TypeKind kind)
        {
            return kind == TypeKind::Class ? "Class" : "Struct";
        }

        const char *PropertyKindName(PropertyKind kind)
        {
            switch (kind)
            {
            case PropertyKind::Unknown:
                return "Unknown";
            case PropertyKind::Bool:
                return "Bool";
            case PropertyKind::Byte:
                return "Byte";
            case PropertyKind::Int8:
                return "Int8";
            case PropertyKind::Int16:
                return "Int16";
            case PropertyKind::Int32:
                return "Int32";
            case PropertyKind::Int64:
                return "Int64";
            case PropertyKind::UInt16:
                return "UInt16";
            case PropertyKind::UInt32:
                return "UInt32";
            case PropertyKind::UInt64:
                return "UInt64";
            case PropertyKind::Float:
                return "Float";
            case PropertyKind::Double:
                return "Double";
            case PropertyKind::Name:
                return "Name";
            case PropertyKind::String:
                return "String";
            case PropertyKind::Text:
                return "Text";
            case PropertyKind::Object:
                return "Object";
            case PropertyKind::SoftObject:
                return "SoftObject";
            case PropertyKind::WeakObject:
                return "WeakObject";
            case PropertyKind::LazyObject:
                return "LazyObject";
            case PropertyKind::Class:
                return "Class";
            case PropertyKind::SoftClass:
                return "SoftClass";
            case PropertyKind::Struct:
                return "Struct";
            case PropertyKind::Enum:
                return "Enum";
            case PropertyKind::Array:
                return "Array";
            case PropertyKind::Set:
                return "Set";
            case PropertyKind::Map:
                return "Map";
            case PropertyKind::Interface:
                return "Interface";
            case PropertyKind::Delegate:
                return "Delegate";
            case PropertyKind::MulticastDelegate:
                return "MulticastDelegate";
            case PropertyKind::FieldPath:
                return "FieldPath";
            case PropertyKind::Optional:
                return "Optional";
            }
            return "Unknown";
        }

        EnumUnderlyingType WidenEnumUnderlying(EnumUnderlyingType type, const EnumIR &enumeration)
        {
            if (enumeration.values.empty())
                return type;

            int64_t minimum = enumeration.values.front().value;
            int64_t maximum = minimum;
            for (const EnumValueIR &value : enumeration.values)
            {
                minimum = std::min(minimum, value.value);
                maximum = std::max(maximum, value.value);
            }

            const auto signedWidth = [&](int bits) -> bool
            {
                const int64_t minValue = bits == 8 ? std::numeric_limits<int8_t>::min() : bits == 16 ? std::numeric_limits<int16_t>::min()
                                                                                      : bits == 32   ? std::numeric_limits<int32_t>::min()
                                                                                                     : std::numeric_limits<int64_t>::min();
                const int64_t maxValue = bits == 8 ? std::numeric_limits<int8_t>::max() : bits == 16 ? std::numeric_limits<int16_t>::max()
                                                                                      : bits == 32   ? std::numeric_limits<int32_t>::max()
                                                                                                     : std::numeric_limits<int64_t>::max();
                return minimum >= minValue && maximum <= maxValue;
            };
            const auto unsignedWidth = [&](int bits) -> bool
            {
                const uint64_t maxValue = bits == 8 ? std::numeric_limits<uint8_t>::max() : bits == 16 ? std::numeric_limits<uint16_t>::max()
                                                                                        : bits == 32   ? std::numeric_limits<uint32_t>::max()
                                                                                                       : std::numeric_limits<uint64_t>::max();
                return minimum >= 0 && static_cast<uint64_t>(maximum) <= maxValue;
            };

            switch (type)
            {
            case EnumUnderlyingType::Int8:
                if (signedWidth(8))
                    return type;
                [[fallthrough]];
            case EnumUnderlyingType::Int16:
                if (signedWidth(16))
                    return EnumUnderlyingType::Int16;
                [[fallthrough]];
            case EnumUnderlyingType::Int32:
                if (signedWidth(32))
                    return EnumUnderlyingType::Int32;
                return EnumUnderlyingType::Int64;
            case EnumUnderlyingType::Int64:
                return type;
            case EnumUnderlyingType::UInt8:
                if (unsignedWidth(8))
                    return type;
                [[fallthrough]];
            case EnumUnderlyingType::UInt16:
                if (unsignedWidth(16))
                    return EnumUnderlyingType::UInt16;
                [[fallthrough]];
            case EnumUnderlyingType::UInt32:
                if (unsignedWidth(32))
                    return EnumUnderlyingType::UInt32;
                return EnumUnderlyingType::UInt64;
            case EnumUnderlyingType::UInt64:
                return unsignedWidth(64) ? type : EnumUnderlyingType::Int64;
            case EnumUnderlyingType::Unknown:
                return EnumUnderlyingType::Int64;
            }
            return EnumUnderlyingType::Int64;
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
            case EnumUnderlyingType::Int64:
                return "std::int64_t";
            case EnumUnderlyingType::UInt64:
                return "std::uint64_t";
            case EnumUnderlyingType::Unknown:
                return "std::int64_t";
            }
            return "std::int64_t";
        }

        void WriteJsonBool(std::ostringstream &stream, bool value)
        {
            stream << (value ? "true" : "false");
        }
    } // namespace

    ArtifactWriter::ArtifactWriter(const RuntimeContext &context,
                                   const ReflectionIR &reflection,
                                   std::filesystem::path outputRoot,
                                   std::string packageName)
        : context_(context), reflection_(reflection), outputRoot_(std::move(outputRoot)), packageName_(std::move(packageName))
    {
    }

    std::string ArtifactWriter::Sanitize(std::string value, const char *fallback)
    {
        for (char &character : value)
        {
            const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') || character == '_';
            if (!valid)
                character = '_';
        }
        if (value.empty() || (value.front() >= '0' && value.front() <= '9'))
            value = std::string(fallback) + value;
        return value;
    }

    std::string ArtifactWriter::JsonEscape(const std::string &value)
    {
        std::string result;
        result.reserve(value.size() + 8);
        for (char character : value)
        {
            switch (character)
            {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result.push_back(character);
                break;
            }
        }
        return result;
    }

    std::string ArtifactWriter::BasicTypes() const
    {
        std::ostringstream stream;
        stream << "#pragma once\n#include <cstddef>\n#include <cstdint>\n\nnamespace AndUE\n{\n";
        stream << "struct FName { std::uint8_t Data[" << context_.Schema().fname.size << "]; };\n";
        stream << "struct FString { std::uintptr_t Data; std::int32_t Num; std::int32_t Max; };\n";
        stream << "template <typename ElementType> struct TArray { std::uintptr_t Data; std::int32_t Num; std::int32_t Max; };\n";
        stream << "template <typename ElementType> struct TSet { std::uint8_t Data[0x48]; };\n";
        stream << "template <typename KeyType, typename ValueType> struct TMap { std::uint8_t Data[0x50]; };\n";
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::ManifestJson(size_t opaqueFields) const
    {
        const ReflectionStats &stats = reflection_.stats;
        std::ostringstream stream;
        stream << "{\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"package\": \"" << JsonEscape(packageName_) << "\",\n";
        stream << "  \"engine\": \"" << JsonEscape(context_.Schema().validation.familyEvidence) << "\",\n";
        stream << "  \"status\": \"" << ParseStatusName(reflection_.status) << "\",\n";
        stream << "  \"artifact_kind\": \"" << (reflection_.status == ParseStatus::Partial ? "partial" : "complete") << "\",\n";
        stream << "  \"module\": \"" << JsonEscape(context_.Module().name) << "\",\n";
        stream << "  \"stats\": {\n";
        stream << "    \"object_slots\": " << stats.objectSlots << ",\n";
        stream << "    \"valid_objects\": " << stats.validObjects << ",\n";
        stream << "    \"parsed_types\": " << stats.parsedTypes << ",\n";
        stream << "    \"parsed_enums\": " << stats.parsedEnums << ",\n";
        stream << "    \"parsed_functions\": " << stats.parsedFunctions << ",\n";
        stream << "    \"parsed_properties\": " << stats.parsedProperties << ",\n";
        stream << "    \"unknown_properties\": " << stats.unknownProperties << ",\n";
        stream << "    \"unresolved_type_details\": " << stats.unresolvedTypeDetails << ",\n";
        stream << "    \"layout_conflicts\": " << stats.layoutConflicts << ",\n";
        stream << "    \"skipped_objects\": " << stats.skippedObjects << ",\n";
        stream << "    \"failures\": " << stats.failures << ",\n";
        stream << "    \"opaque_fields\": " << opaqueFields << "\n";
        stream << "  }\n";
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::DiagnosticsJson() const
    {
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 1,\n  \"status\": \""
               << ParseStatusName(reflection_.status) << "\",\n";
        stream << "  \"summary\": {\"unknown_properties\": " << reflection_.stats.unknownProperties
               << ", \"unresolved_type_details\": " << reflection_.stats.unresolvedTypeDetails
               << ", \"layout_conflicts\": " << reflection_.stats.layoutConflicts
               << ", \"failures\": " << reflection_.stats.failures << "},\n";
        stream << "  \"diagnostics\": [";
        for (size_t index = 0; index < reflection_.diagnostics.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "\"" << JsonEscape(reflection_.diagnostics[index]) << "\"";
        }
        stream << "],\n";
        stream << "  \"type_conflicts\": [\n";
        size_t conflictCount = 0;
        for (const TypeIR &type : reflection_.types)
        {
            for (const std::string &conflict : type.layoutConflicts)
            {
                if (conflictCount++ != 0)
                    stream << ",\n";
                stream << "    {\"type\":\"" << JsonEscape(type.fullName) << "\",\"message\":\""
                       << JsonEscape(conflict) << "\"}";
            }
        }
        stream << "\n  ],\n  \"function_conflicts\": [\n";
        conflictCount = 0;
        for (const TypeIR &type : reflection_.types)
        {
            for (const FunctionIR &function : type.functions)
            {
                for (const std::string &conflict : function.layoutConflicts)
                {
                    if (conflictCount++ != 0)
                        stream << ",\n";
                    stream << "    {\"function\":\"" << JsonEscape(function.fullName) << "\",\"message\":\""
                           << JsonEscape(conflict) << "\"}";
                }
            }
        }
        stream << "\n  ]\n}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Types(size_t &opaqueFields) const
    {
        std::unordered_map<uintptr_t, std::string> types;
        std::unordered_set<std::string> usedNames;
        std::unordered_map<uintptr_t, std::string> enums;
        for (const TypeIR &type : reflection_.types)
        {
            std::string name = Sanitize(type.name, "Type_");
            if (!usedNames.insert(name).second)
                name += "_" + Hex(type.address).substr(2);
            types.emplace(type.address, std::move(name));
        }
        for (const EnumIR &enumeration : reflection_.enums)
            enums.emplace(enumeration.address, Sanitize(enumeration.name, "Enum_"));

        std::ostringstream stream;
        stream << "#pragma once\n#include <cstdint>\n#include \"BasicTypes.hpp\"\n#include \"Enums.hpp\"\n\nnamespace AndUE\n{\n";
        for (const TypeIR &type : reflection_.types)
            stream << "struct " << types[type.address] << ";\n";
        if (!reflection_.types.empty())
            stream << "\n";
        std::vector<size_t> order;
        std::unordered_set<uintptr_t> emitted;
        while (order.size() < reflection_.types.size())
        {
            bool progress = false;
            for (size_t index = 0; index < reflection_.types.size(); ++index)
            {
                const TypeIR &candidate = reflection_.types[index];
                if (emitted.contains(candidate.address))
                    continue;
                if (candidate.superAddress != 0 && !emitted.contains(candidate.superAddress) &&
                    types.contains(candidate.superAddress))
                    continue;
                bool valueDependencyPending = false;
                for (const PropertyIR &property : candidate.properties)
                {
                    if (property.type.kind != PropertyKind::Struct || property.type.referencedObject == 0 ||
                        property.type.referencedObject == candidate.address)
                        continue;
                    if (types.contains(property.type.referencedObject) &&
                        !emitted.contains(property.type.referencedObject))
                    {
                        valueDependencyPending = true;
                        break;
                    }
                }
                if (valueDependencyPending)
                    continue;
                order.push_back(index);
                emitted.insert(candidate.address);
                progress = true;
            }
            if (!progress)
            {
                for (size_t index = 0; index < reflection_.types.size(); ++index)
                {
                    if (!emitted.contains(reflection_.types[index].address))
                    {
                        order.push_back(index);
                        emitted.insert(reflection_.types[index].address);
                    }
                }
            }
        }
        for (size_t typeIndex : order)
        {
            const TypeIR &type = reflection_.types[typeIndex];
            const std::string typeName = types[type.address];
            stream << "struct " << typeName;
            if (const auto base = types.find(type.superAddress); base != types.end())
                stream << " : public " << base->second;
            stream << "\n{\n";
            int32_t cursor = 0;
            size_t ordinal = 0;
            std::unordered_map<int32_t, std::string> boolStorageNames;
            std::unordered_map<int32_t, int32_t> boolStorageEnds;
            for (const PropertyIR &property : type.properties)
            {
                const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
                if (total <= 0 || property.offset < 0)
                    continue;

                const bool boolLayout = property.type.kind == PropertyKind::Bool &&
                                        property.boolean.fieldSize > 0 && property.boolean.fieldSize <= 8 &&
                                        property.boolean.byteOffset < property.boolean.fieldSize &&
                                        property.boolean.byteMask != 0 && property.boolean.fieldMask != 0;
                if (boolLayout)
                {
                    const int32_t storageOffset = property.offset + property.boolean.byteOffset;
                    const int32_t storageSize = property.boolean.fieldSize;
                    const int32_t storageEnd = storageOffset + storageSize;
                    if (storageEnd < storageOffset || storageEnd > type.size)
                        continue;

                    const auto existing = boolStorageNames.find(storageOffset);
                    if (storageOffset < cursor && existing == boolStorageNames.end())
                        continue;
                    if (storageOffset > cursor)
                        stream << "    std::uint8_t Pad_" << ordinal++ << "[0x" << std::hex
                               << (storageOffset - cursor) << std::dec << "];\n";
                    if (existing == boolStorageNames.end())
                    {
                        const std::string storageName = "BoolStorage_" + std::to_string(ordinal++);
                        boolStorageNames.emplace(storageOffset, storageName);
                        boolStorageEnds.emplace(storageOffset, storageEnd);
                        stream << "    std::uint8_t " << storageName;
                        if (storageSize > 1)
                            stream << "[" << storageSize << "]";
                        stream << ";\n";
                    }

                    const std::string member = Sanitize(property.name, "Member_") + "_" + std::to_string(ordinal++);
                    stream << "    static constexpr std::uint8_t " << member << "_Mask = 0x"
                           << std::hex << static_cast<unsigned int>(property.boolean.fieldMask) << std::dec << ";\n";
                    cursor = std::max(cursor, storageEnd);
                    continue;
                }

                if (property.offset < cursor || static_cast<int64_t>(property.offset) + total > type.size)
                    continue;
                if (property.offset > cursor)
                    stream << "    std::uint8_t Pad_" << ordinal++ << "[0x" << std::hex << (property.offset - cursor) << std::dec << "];\n";
                bool resolved = false;
                const std::string cppType = PropertyType(property, types, enums, resolved);
                const std::string member = Sanitize(property.name, "Member_") + "_" + std::to_string(ordinal++);
                if (!resolved)
                {
                    stream << "    std::uint8_t " << member << "[0x" << std::hex << total << std::dec << "]; // opaque\n";
                    ++opaqueFields;
                }
                else
                {
                    stream << "    " << cppType << " " << member;
                    if (property.arrayDim > 1)
                        stream << "[" << property.arrayDim << "]";
                    stream << ";\n";
                }
                cursor = property.offset + static_cast<int32_t>(total);
            }
            if (cursor < type.size)
                stream << "    std::uint8_t TailData[0x" << std::hex << (type.size - cursor) << std::dec << "];\n";
            stream << "};\n\n";
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Functions(size_t &opaqueFields) const
    {
        std::unordered_map<uintptr_t, std::string> types;
        std::unordered_set<std::string> usedTypeNames;
        std::unordered_map<uintptr_t, std::string> enums;
        for (const TypeIR &type : reflection_.types)
        {
            std::string name = Sanitize(type.name, "Type_");
            if (!usedTypeNames.insert(name).second)
                name += "_" + Hex(type.address).substr(2);
            types.emplace(type.address, std::move(name));
        }
        for (const EnumIR &enumeration : reflection_.enums)
            enums.emplace(enumeration.address, Sanitize(enumeration.name, "Enum_"));

        std::ostringstream stream;
        stream << "#pragma once\n#include <cstdint>\n#include \"Types.hpp\"\n\nnamespace AndUE\n{\n";
        if (reflection_.stats.parsedFunctions == 0)
            stream << "// No UFunction metadata was resolved in this dump.\n";
        for (const TypeIR &type : reflection_.types)
        {
            for (const FunctionIR &function : type.functions)
            {
                const std::string functionName = Sanitize(types[type.address] + "_" + function.name, "Function_");
                stream << "// " << function.fullName << "\n";
                stream << "inline constexpr std::uintptr_t " << functionName
                       << "_NativeRva = " << Hex(function.nativeRva) << ";\n";
                stream << "struct " << functionName << "_Params\n{\n";
                int32_t cursor = 0;
                size_t ordinal = 0;
                for (const PropertyIR &parameter : function.parameters)
                {
                    const int64_t total = static_cast<int64_t>(parameter.elementSize) * parameter.arrayDim;
                    if (total <= 0 || parameter.offset < cursor ||
                        static_cast<int64_t>(parameter.offset) + total > function.paramSize)
                        continue;
                    if (parameter.offset > cursor)
                        stream << "    std::uint8_t Pad_" << ordinal++ << "[0x" << std::hex
                               << (parameter.offset - cursor) << std::dec << "];\n";
                    bool resolved = false;
                    const std::string cppType = PropertyType(parameter, types, enums, resolved);
                    const std::string member = Sanitize(parameter.name, "Param_") + "_" + std::to_string(ordinal++);
                    if (!resolved)
                    {
                        stream << "    std::uint8_t " << member << "[0x" << std::hex << total << std::dec << "];\n";
                        ++opaqueFields;
                    }
                    else
                    {
                        stream << "    " << cppType << " " << member;
                        if (parameter.arrayDim > 1)
                            stream << "[" << parameter.arrayDim << "]";
                        stream << ";\n";
                    }
                    cursor = parameter.offset + static_cast<int32_t>(total);
                }
                if (cursor < function.paramSize)
                    stream << "    std::uint8_t TailData[0x" << std::hex << (function.paramSize - cursor)
                           << std::dec << "];\n";
                stream << "};\n\n";
            }
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Enums() const
    {
        std::ostringstream stream;
        stream << "#pragma once\n#include <cstdint>\n\nnamespace AndUE\n{\n";
        if (reflection_.enums.empty())
            stream << "// No UEnum metadata was resolved in this dump.\n";
        std::unordered_set<std::string> usedNames;
        for (const EnumIR &enumeration : reflection_.enums)
        {
            std::string enumName = Sanitize(enumeration.name, "Enum_");
            if (!usedNames.insert(enumName).second)
                enumName += "_" + Hex(enumeration.address).substr(2);
            stream << "enum class " << enumName << " : "
                   << EnumUnderlyingName(WidenEnumUnderlying(enumeration.underlyingType, enumeration)) << "\n{\n";
            for (const EnumValueIR &value : enumeration.values)
                stream << "    " << Sanitize(value.name, "Value_") << " = " << value.value << ",\n";
            stream << "};\n\n";
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::RuntimeJson() const
    {
        const RuntimeBinding &binding = context_.Binding();
        const EngineSchema &schema = context_.Schema();
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 1,\n";
        stream << "  \"engine\": \"" << JsonEscape(schema.validation.familyEvidence) << "\",\n";
        stream << "  \"module\": {\"name\":\"" << JsonEscape(context_.Module().name)
               << "\",\"base\":\"" << Hex(context_.Module().base) << "\",\"end\":\""
               << Hex(context_.Module().end) << "\"},\n";
        stream << "  \"binding\": {\n";
        stream << "    \"object_root\": \"" << Hex(binding.objectRoot.address) << "\",\n";
        stream << "    \"name_root\": \"" << Hex(binding.nameRoot.address) << "\",\n";
        stream << "    \"objects\": {\"kind\":\""
               << (binding.objects.kind == ObjectContainerKind::Chunked ? "chunked" : "fixed")
               << "\",\"objects_offset\":" << binding.objects.objectsOffset
               << ",\"num_elements_offset\":" << binding.objects.numElementsOffset
               << ",\"max_elements_offset\":" << binding.objects.maxElementsOffset
               << ",\"max_chunks_offset\":" << binding.objects.maxChunksOffset
               << ",\"elements_per_chunk\":" << binding.objects.elementsPerChunk
               << ",\"item_object_offset\":" << binding.objects.itemObjectOffset
               << ",\"item_stride\":" << binding.objects.itemStride
               << ",\"item_index_offset\":" << binding.objects.itemIndexOffset << "},\n";
        stream << "    \"names\": {\"kind\":\""
               << (binding.names.kind == NameContainerKind::Pool ? "pool" : "array") << "\"";
        if (binding.names.kind == NameContainerKind::Pool)
        {
            stream << ",\"blocks_offset\":" << binding.names.pool.blocksOffset
                   << ",\"blocks_bit\":" << binding.names.pool.blocksBit
                   << ",\"entry_stride\":" << binding.names.pool.entryStride
                   << ",\"entry_header_offset\":" << binding.names.pool.entryHeaderOffset
                   << ",\"entry_string_offset\":" << binding.names.pool.entryStringOffset
                   << ",\"entry_length_shift\":" << binding.names.pool.entryLengthShift
                   << ",\"entry_wide_mask\":" << binding.names.pool.entryWideMask;
        }
        else
        {
            stream << ",\"chunks_offset\":" << binding.names.array.chunksOffset
                   << ",\"elements_per_chunk\":" << binding.names.array.elementsPerChunk
                   << ",\"entry_index_offset\":" << binding.names.array.entryIndexOffset
                   << ",\"entry_string_offset\":" << binding.names.array.entryStringOffset;
        }
        stream << "},\n  \"schema\": {\n";
        stream << "    \"uobject\": {\"flags\":" << schema.uobject.flags
               << ",\"internal_index\":" << schema.uobject.internalIndex
               << ",\"class\":" << schema.uobject.classPointer
               << ",\"name\":" << schema.uobject.name << ",\"outer\":" << schema.uobject.outer << "},\n";
        stream << "    \"ustruct\": {\"super\":" << schema.ustruct.superStruct
               << ",\"children\":" << schema.ustruct.children
               << ",\"child_properties\":" << schema.ustruct.childProperties
               << ",\"size\":" << schema.ustruct.size << "},\n";
        stream << "    \"property\": {\"array_dim\":" << schema.property.arrayDim
               << ",\"element_size\":" << schema.property.elementSize
               << ",\"flags\":" << schema.property.propertyFlags
               << ",\"offset_internal\":" << schema.property.offsetInternal << "},\n";
        stream << "    \"ufunction\": {\"flags\":" << schema.ufunction.functionFlags
               << ",\"num_params\":" << schema.ufunction.numParams
               << ",\"param_size\":" << schema.ufunction.paramSize
               << ",\"native_function\":" << schema.ufunction.nativeFunction << "},\n";
        stream << "    \"uenum\": {\"names\":" << schema.uenum.names
               << ",\"underlying_type\":" << schema.uenum.underlyingType << "}\n";
        stream << "  }\n}\n";
        return stream.str();
    }

    std::string ArtifactWriter::ReflectionJson() const
    {
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 1,\n";
        stream << "  \"status\": \"" << StatusName(reflection_.status) << "\",\n";
        stream << "  \"engine\": \"" << JsonEscape(context_.Schema().validation.familyEvidence) << "\",\n";
        stream << "  \"stats\": {\"parsed_types\": " << reflection_.stats.parsedTypes
               << ", \"parsed_enums\": " << reflection_.stats.parsedEnums
               << ", \"parsed_functions\": " << reflection_.stats.parsedFunctions
               << ", \"parsed_properties\": " << reflection_.stats.parsedProperties
               << ", \"unknown_properties\": " << reflection_.stats.unknownProperties
               << ", \"unresolved_type_details\": " << reflection_.stats.unresolvedTypeDetails
               << ", \"failures\": " << reflection_.stats.failures << "},\n";
        stream << "  \"types\": [\n";
        for (size_t index = 0; index < reflection_.types.size(); ++index)
        {
            const TypeIR &type = reflection_.types[index];
            stream << "    {\"address\":\"" << Hex(type.address) << "\",\"kind\":\""
                   << TypeKindName(type.kind) << "\",\"name\":\"" << JsonEscape(type.name)
                   << "\",\"full_name\":\"" << JsonEscape(type.fullName)
                   << "\",\"size\":" << type.size << ",\"super\":\""
                   << Hex(type.superAddress) << "\",\"properties\":[";
            for (size_t propertyIndex = 0; propertyIndex < type.properties.size(); ++propertyIndex)
            {
                const PropertyIR &property = type.properties[propertyIndex];
                stream << "{\"address\":\"" << Hex(property.address) << "\",\"name\":\""
                       << JsonEscape(property.name) << "\",\"class\":\""
                       << JsonEscape(property.reflectedClass) << "\",\"offset\":" << property.offset
                       << ",\"element_size\":" << property.elementSize << ",\"array_dim\":"
                       << property.arrayDim << ",\"flags\":\""
                       << Hex(static_cast<uintptr_t>(property.flags)) << "\",\"kind\":\""
                       << PropertyKindName(property.type.kind) << "\",\"referenced_object\":\""
                       << Hex(property.type.referencedObject) << "\",\"type_details_resolved\":";
                WriteJsonBool(stream, property.typeDetailsResolved);
                if (property.type.kind == PropertyKind::Bool)
                {
                    stream << ",\"bool_layout\":{\"field_size\":"
                           << static_cast<unsigned int>(property.boolean.fieldSize)
                           << ",\"byte_offset\":" << static_cast<unsigned int>(property.boolean.byteOffset)
                           << ",\"byte_mask\":" << static_cast<unsigned int>(property.boolean.byteMask)
                           << ",\"field_mask\":" << static_cast<unsigned int>(property.boolean.fieldMask) << "}";
                }
                stream << "}";
                if (propertyIndex + 1 != type.properties.size())
                    stream << ',';
            }
            stream << "],\"layout_conflicts\":[";
            for (size_t conflictIndex = 0; conflictIndex < type.layoutConflicts.size(); ++conflictIndex)
            {
                stream << "\"" << JsonEscape(type.layoutConflicts[conflictIndex]) << "\"";
                if (conflictIndex + 1 != type.layoutConflicts.size())
                    stream << ',';
            }
            stream << "],\"functions\":[";
            for (size_t functionIndex = 0; functionIndex < type.functions.size(); ++functionIndex)
            {
                const FunctionIR &function = type.functions[functionIndex];
                stream << "{\"address\":\"" << Hex(function.address) << "\",\"name\":\""
                       << JsonEscape(function.name) << "\",\"full_name\":\""
                       << JsonEscape(function.fullName) << "\",\"native_rva\":\""
                       << Hex(function.nativeRva) << "\",\"flags\":\""
                       << Hex(static_cast<uintptr_t>(function.flags)) << "\",\"num_params\":"
                       << static_cast<unsigned int>(function.numParams) << ",\"param_size\":"
                       << function.paramSize << ",\"parameters\":[";
                for (size_t parameterIndex = 0; parameterIndex < function.parameters.size(); ++parameterIndex)
                {
                    const PropertyIR &parameter = function.parameters[parameterIndex];
                    stream << "{\"name\":\"" << JsonEscape(parameter.name) << "\",\"offset\":"
                           << parameter.offset << ",\"element_size\":" << parameter.elementSize
                           << ",\"array_dim\":" << parameter.arrayDim << ",\"kind\":\""
                           << PropertyKindName(parameter.type.kind) << "\"}";
                    if (parameterIndex + 1 != function.parameters.size())
                        stream << ',';
                }
                stream << "],\"layout_conflicts\":[";
                for (size_t conflictIndex = 0; conflictIndex < function.layoutConflicts.size(); ++conflictIndex)
                {
                    stream << "\"" << JsonEscape(function.layoutConflicts[conflictIndex]) << "\"";
                    if (conflictIndex + 1 != function.layoutConflicts.size())
                        stream << ',';
                }
                stream << "]}";
                if (functionIndex + 1 != type.functions.size())
                    stream << ',';
            }
            stream << "]}";
            if (index + 1 != reflection_.types.size())
                stream << ',';
            stream << '\n';
        }
        stream << "  ],\n  \"enums\": [\n";
        for (size_t index = 0; index < reflection_.enums.size(); ++index)
        {
            const EnumIR &enumeration = reflection_.enums[index];
            stream << "    {\"address\":\"" << Hex(enumeration.address) << "\",\"name\":\""
                   << JsonEscape(enumeration.name) << "\",\"full_name\":\""
                   << JsonEscape(enumeration.fullName) << "\",\"cpp_form\":"
                   << static_cast<unsigned int>(enumeration.cppForm) << ",\"flags\":"
                   << static_cast<unsigned int>(enumeration.flags) << ",\"underlying_type\":"
                   << static_cast<int>(enumeration.underlyingType) << ",\"values\":[";
            for (size_t valueIndex = 0; valueIndex < enumeration.values.size(); ++valueIndex)
            {
                const EnumValueIR &value = enumeration.values[valueIndex];
                stream << "{\"name\":\"" << JsonEscape(value.name) << "\",\"value\":" << value.value << "}";
                if (valueIndex + 1 != enumeration.values.size())
                    stream << ',';
            }
            stream << "]}";
            if (index + 1 != reflection_.enums.size())
                stream << ',';
            stream << '\n';
        }
        stream << "  ]\n}\n";
        return stream.str();
    }

    ArtifactResult ArtifactWriter::Write() const
    {
        ArtifactResult result;
        if (reflection_.status == ParseStatus::Failed)
        {
            result.error = "reflection input is failed";
            return result;
        }
        if (outputRoot_.empty() || packageName_.empty())
        {
            result.error = "output root and package name are required";
            return result;
        }

        std::error_code error;
        std::filesystem::create_directories(outputRoot_, error);
        if (error)
        {
            result.error = "output root creation failed";
            return result;
        }
        const std::string packageStem = Sanitize(packageName_, "Package");
        const std::string artifactStem = packageStem + (reflection_.status == ParseStatus::Partial ? ".partial" : "");
        const std::filesystem::path finalPath = outputRoot_ / artifactStem;
        if (std::filesystem::exists(finalPath, error))
        {
            result.error = "output directory already exists";
            return result;
        }
        const std::filesystem::path temporary = outputRoot_ / ("." + artifactStem + ".tmp");
        std::filesystem::remove_all(temporary, error);
        std::filesystem::create_directories(temporary, error);
        if (error)
        {
            result.error = "temporary output directory creation failed";
            return result;
        }

        size_t opaque = 0;
        const std::string basicTypes = BasicTypes();
        const std::string types = Types(opaque);
        const std::string enums = Enums();
        const std::string functions = Functions(opaque);
        const std::string reflectionJson = ReflectionJson();
        const std::string manifestJson = ManifestJson(opaque);
        const std::string diagnosticsJson = DiagnosticsJson();
        const std::string runtimeJson = RuntimeJson();
        const std::vector<std::pair<std::string, std::string>> files = {
            {"BasicTypes.hpp", basicTypes},
            {"Types.hpp", types},
            {"Enums.hpp", enums},
            {"Functions.hpp", functions},
            {"reflection.json", reflectionJson},
            {"manifest.json", manifestJson},
            {"diagnostics.json", diagnosticsJson},
            {"runtime.json", runtimeJson},
        };
        for (const auto &[name, content] : files)
        {
            std::ofstream stream(temporary / name, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                result.error = "output file open failed: " + name;
                std::filesystem::remove_all(temporary, error);
                return result;
            }
            stream.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!stream.good())
            {
                result.error = "output file write failed: " + name;
                std::filesystem::remove_all(temporary, error);
                return result;
            }
            ++result.filesWritten;
        }
        std::filesystem::rename(temporary, finalPath, error);
        if (error)
        {
            result.error = "output directory commit failed";
            std::filesystem::remove_all(temporary, error);
            return result;
        }
        result.outputPath = finalPath;
        result.opaqueFields = opaque;
        result.status = reflection_.status;
        return result;
    }
} // namespace anduefker::generation
