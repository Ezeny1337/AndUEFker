#include "anduefker/generation/ArtifactWriter.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anduefker::generation
{
    using ::anduefker::app::ModuleArchitecture;
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::binding::NameContainerKind;
    using ::anduefker::binding::ObjectContainerKind;
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::ue::EngineSchema;
    using ::anduefker::ue::FNameDisplayLayout;
    using ::anduefker::ue::FNameNumberLayout;
    using ::anduefker::ue::SchemaLayoutVariantName;

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

        std::string Hex(uint64_t value)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << value;
            return stream.str();
        }

        const char *ArchitectureName(ModuleArchitecture architecture)
        {
            switch (architecture)
            {
            case ModuleArchitecture::Arm32:
                return "ARM32";
            case ModuleArchitecture::Arm64:
                return "ARM64";
            case ModuleArchitecture::Unknown:
                break;
            }
            return "unknown";
        }

        struct CppTypeInfo
        {
            std::string name;
            int32_t size = 0;
        };

        struct CppEnumInfo
        {
            std::string name;
            EnumUnderlyingType underlyingType = EnumUnderlyingType::Unknown;
            int32_t size = 0;
        };

        std::string PropertyType(const TypeReferenceIR &reference,
                                 const std::unordered_map<uintptr_t, CppTypeInfo> &types,
                                 const std::unordered_map<uintptr_t, CppEnumInfo> &enums,
                                 int32_t pointerWidth,
                                 int32_t nameSize,
                                 size_t depth = 0)
        {
            if (reference.elementSize <= 0 || depth > 32)
                return {};
            const auto sized = [&](const std::string &name, int32_t size) -> std::string
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
            case PropertyKind::Byte:
            case PropertyKind::Bool:
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
                if (const auto found = types.find(reference.referencedObject); found != types.end())
                    return sized(found->second.name + "*", pointerWidth);
                break;
            case PropertyKind::Struct:
                if (const auto found = types.find(reference.referencedObject);
                    found != types.end() && found->second.size > 0)
                    return sized(found->second.name, found->second.size);
                break;
            case PropertyKind::Enum:
                if (const auto found = enums.find(reference.secondaryObject); found != enums.end())
                    return sized(found->second.name, found->second.size);
                break;
            case PropertyKind::Array:
            case PropertyKind::Set:
                if (reference.inner)
                {
                    const std::string inner = PropertyType(*reference.inner, types, enums,
                                                           pointerWidth, nameSize, depth + 1);
                    if (!inner.empty())
                    {
                        if (reference.kind == PropertyKind::Array)
                            return sized("TArray<" + inner + ">", pointerWidth + 8);
                        return "TSet<" + inner + ", " + Hex(static_cast<uint32_t>(reference.elementSize)) + ">";
                    }
                }
                break;
            case PropertyKind::Map:
                if (reference.key && reference.value)
                {
                    const std::string key = PropertyType(*reference.key, types, enums,
                                                         pointerWidth, nameSize, depth + 1);
                    const std::string value = PropertyType(*reference.value, types, enums,
                                                           pointerWidth, nameSize, depth + 1);
                    if (!key.empty() && !value.empty())
                        return "TMap<" + key + ", " + value + ", " + Hex(static_cast<uint32_t>(reference.elementSize)) + ">";
                }
                break;
            case PropertyKind::Interface:
                if (types.contains(reference.referencedObject))
                    return sized("FScriptInterface", pointerWidth * 2);
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

        int32_t EnumUnderlyingSize(EnumUnderlyingType type)
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
            case EnumUnderlyingType::Int64:
            case EnumUnderlyingType::UInt64:
            case EnumUnderlyingType::Unknown:
                return 8;
            }
            return 8;
        }

        void WriteJsonBool(std::ostringstream &stream, bool value)
        {
            stream << (value ? "true" : "false");
        }
    } // namespace

    struct ArtifactWriter::CppSymbols
    {
        std::unordered_map<uintptr_t, CppTypeInfo> types;
        std::unordered_map<uintptr_t, CppEnumInfo> enums;
    };

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

    ArtifactWriter::CppSymbols ArtifactWriter::BuildCppSymbols() const
    {
        CppSymbols result;
        std::unordered_set<std::string> usedNames = {"FName", "FString", "FScriptInterface", "TArray", "TSet", "TMap"};
        const auto uniqueName = [&](const std::string &name, const char *fallback, uintptr_t address)
        {
            std::string candidate = Sanitize(name, fallback);
            if (!usedNames.insert(candidate).second)
            {
                candidate += "_" + Hex(address).substr(2);
                const std::string stem = candidate;
                size_t suffix = 0;
                while (!usedNames.insert(candidate).second)
                    candidate = stem + "_" + std::to_string(++suffix);
            }
            return candidate;
        };
        // 类型、枚举及其引用共用命名表，避免不同头文件各自处理重名。
        for (const TypeIR &type : reflection_.types)
            result.types.emplace(type.address, CppTypeInfo{uniqueName(type.name, "Type_", type.address), type.size});
        for (const EnumIR &enumeration : reflection_.enums)
        {
            const EnumUnderlyingType underlying = WidenEnumUnderlying(enumeration.underlyingType, enumeration);
            result.enums.emplace(enumeration.address,
                                 CppEnumInfo{uniqueName(enumeration.name, "Enum_", enumeration.address),
                                             underlying, EnumUnderlyingSize(underlying)});
        }
        return result;
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
        stream << "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n";
        stream << "// For inspection and analysis; reflected offsets and sizes are authoritative.\n";
        stream << "// Target pointer width: " << static_cast<unsigned int>(context_.Module().pointerWidth) << " bytes.\n";
        stream << "namespace AndUE\n{\n";
        stream << "struct FName { std::uint8_t Data[" << context_.Schema().fname.size << "]; };\n";
        stream << "struct FString { std::uintptr_t Data; std::int32_t Num; std::int32_t Max; };\n";
        stream << "template <typename ElementType> struct TArray { std::uintptr_t Data; std::int32_t Num; std::int32_t Max; };\n";
        stream << "struct FScriptInterface { std::uintptr_t ObjectPointer; std::uintptr_t InterfacePointer; };\n";
        stream << "// Opaque container storage; StorageSize comes from the reflected field.\n";
        stream << "template <typename ElementType, std::size_t StorageSize> struct TSet { std::uint8_t Data[StorageSize]; };\n";
        stream << "template <typename KeyType, typename ValueType, std::size_t StorageSize> struct TMap { std::uint8_t Data[StorageSize]; };\n";
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
        stream << "  \"profile\": {\"id\":\""
               << JsonEscape(context_.Schema().validation.profileId) << "\",\"label\":\""
               << JsonEscape(context_.Schema().validation.profileLabel) << "\",\"version_range\":\""
               << JsonEscape(context_.Schema().validation.profileVersionRange) << "\"},\n";
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
        stream << "    \"empty_object_slots\": " << stats.emptyObjectSlots << ",\n";
        stream << "    \"object_read_failures\": " << stats.objectReadFailures << ",\n";
        stream << "    \"class_name_read_failures\": " << stats.classNameReadFailures << ",\n";
        stream << "    \"skipped_class_default_objects\": " << stats.skippedClassDefaultObjects << ",\n";
        stream << "    \"skipped_incomplete_objects\": " << stats.skippedIncompleteObjects << ",\n";
        stream << "    \"object_diagnostic_samples_omitted\": " << stats.objectDiagnosticSamplesOmitted << ",\n";
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
               << ", \"skipped_objects\": " << reflection_.stats.skippedObjects
               << ", \"empty_object_slots\": " << reflection_.stats.emptyObjectSlots
               << ", \"object_read_failures\": " << reflection_.stats.objectReadFailures
               << ", \"class_name_read_failures\": " << reflection_.stats.classNameReadFailures
               << ", \"skipped_class_default_objects\": " << reflection_.stats.skippedClassDefaultObjects
               << ", \"skipped_incomplete_objects\": " << reflection_.stats.skippedIncompleteObjects
               << ", \"object_diagnostic_samples_omitted\": " << reflection_.stats.objectDiagnosticSamplesOmitted
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

    void ArtifactWriter::WriteFields(std::ostringstream &stream,
                                     const std::vector<PropertyIR> &properties,
                                     int32_t initialOffset,
                                     int32_t size,
                                     const CppSymbols &symbols,
                                     size_t &opaqueFields) const
    {
        struct BoolStorage
        {
            int32_t end = 0;
            uint64_t masks = 0;
        };
        std::unordered_map<int32_t, BoolStorage> boolStorage;
        std::vector<const PropertyIR *> ordered;
        ordered.reserve(properties.size());
        for (const PropertyIR &property : properties)
            ordered.push_back(&property);
        std::stable_sort(ordered.begin(), ordered.end(), [](const PropertyIR *left, const PropertyIR *right)
                         { return left->offset < right->offset; });

        int32_t cursor = initialOffset;
        size_t ordinal = 0;
        for (const PropertyIR *entry : ordered)
        {
            const PropertyIR &property = *entry;
            const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
            const int64_t end = static_cast<int64_t>(property.offset) + total;
            const std::string member = Sanitize(property.name, "Member_") + "_" + std::to_string(ordinal++);
            if (property.elementSize <= 0 || property.arrayDim <= 0 || property.offset < 0 || end > size)
            {
                stream << "    // Field has invalid dimensions or exceeds the reflected size: " << member << "\n";
                continue;
            }

            const bool boolLayout = property.type.kind == PropertyKind::Bool &&
                                    property.boolean.fieldSize == property.elementSize &&
                                    property.boolean.fieldSize > 0 && property.boolean.fieldSize <= 8 &&
                                    property.boolean.byteOffset < property.boolean.fieldSize &&
                                    property.boolean.byteMask != 0 && property.boolean.fieldMask != 0;
            if (boolLayout)
            {
                // ByteOffset 是存储内部的字节位置，不能加到存储起点后再占用整个 FieldSize。
                const uint64_t mask = static_cast<uint64_t>(property.boolean.fieldMask) << (property.boolean.byteOffset * 8);
                auto existing = boolStorage.find(property.offset);
                if (existing != boolStorage.end())
                {
                    if (existing->second.end != end || (existing->second.masks & mask) != 0)
                    {
                        stream << "    // Conflicting bool storage size or mask: " << member
                               << ", offset=" << Hex(property.offset) << ", size=" << Hex(static_cast<uint64_t>(total)) << "\n";
                    }
                    existing->second.masks |= mask;
                }
                else
                {
                    if (property.offset < cursor)
                        stream << "    // Bool storage overlaps the base or an existing field: " << member << "\n";
                    if (property.offset > cursor)
                        stream << "    std::uint8_t Pad_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
                    const std::string storageName = "BoolStorage_" + std::to_string(ordinal++);
                    stream << "    std::uint8_t " << storageName << "[" << Hex(static_cast<uint64_t>(total)) << "]; // "
                           << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ")\n";
                    boolStorage.emplace(property.offset, BoolStorage{static_cast<int32_t>(end), mask});
                    cursor = std::max(cursor, static_cast<int32_t>(end));
                }
                stream << "    static constexpr std::size_t " << member << "_Offset = " << property.offset << ";\n";
                stream << "    static constexpr std::size_t " << member << "_ElementSize = " << property.elementSize << ";\n";
                stream << "    static constexpr std::uint8_t " << member << "_ByteOffset = "
                       << static_cast<unsigned int>(property.boolean.byteOffset) << ";\n";
                stream << "    static constexpr std::uint8_t " << member << "_ByteMask = "
                       << Hex(property.boolean.byteMask) << ";\n";
                stream << "    static constexpr std::uint8_t " << member << "_Mask = "
                       << Hex(property.boolean.fieldMask) << ";\n";
                continue;
            }

            if (property.offset < cursor)
                stream << "    // Overlapping reflected field; use its explicit offset: " << member << "\n";
            if (property.offset > cursor)
                stream << "    std::uint8_t Pad_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
            const bool hasTypeDetails = property.type.kind != PropertyKind::Bool && property.typeDetailsResolved &&
                                        property.type.elementSize == property.elementSize;
            const std::string cppType = hasTypeDetails
                                            ? PropertyType(property.type, symbols.types, symbols.enums,
                                                           context_.Module().pointerWidth, context_.Schema().fname.size)
                                            : std::string{};
            if (cppType.empty())
            {
                stream << "    std::uint8_t " << member << "[" << Hex(static_cast<uint64_t>(total))
                       << "]; // " << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ") opaque\n";
                ++opaqueFields;
            }
            else
            {
                stream << "    " << cppType << " " << member;
                if (property.arrayDim > 1)
                    stream << "[" << property.arrayDim << "]";
                stream << "; // " << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ")\n";
            }
            stream << "    static constexpr std::size_t " << member << "_Offset = " << property.offset << ";\n";
            cursor = std::max(cursor, static_cast<int32_t>(end));
        }
        if (cursor < size)
            stream << "    std::uint8_t TailData[" << Hex(size - cursor) << "];\n";
        stream << "};\n";
        stream << '\n';
    }

    std::string ArtifactWriter::Types(const CppSymbols &symbols, size_t &opaqueFields) const
    {
        const auto &types = symbols.types;

        std::ostringstream stream;
        stream << "#pragma once\n#include <cstddef>\n#include <cstdint>\n#include \"BasicTypes.hpp\"\n#include \"Enums.hpp\"\n\n";
        stream << "// Sizes and offsets describe target memory, not native C++ layout.\n";
        stream << "namespace AndUE\n{\n";
        for (const TypeIR &type : reflection_.types)
            stream << "struct " << types.at(type.address).name << ";\n";
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
            const std::string typeName = types.at(type.address).name;
            int32_t initialOffset = 0;
            const auto base = types.find(type.superAddress);
            const bool baseSizeValid = base != types.end() && base->second.size >= 0 && base->second.size <= type.size;
            if (type.superAddress != 0 && !baseSizeValid)
                stream << "// Base size is unavailable or inconsistent; padding uses the full reflected range.\n";
            stream << "// Reflected size: " << Hex(static_cast<uint32_t>(type.size)) << "\n";
            stream << "struct " << typeName;
            if (base != types.end())
                stream << " : public " << base->second.name;
            if (baseSizeValid)
                initialOffset = base->second.size;
            stream << "\n{\n";
            WriteFields(stream, type.properties, initialOffset, type.size, symbols, opaqueFields);
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Functions(const CppSymbols &symbols, size_t &opaqueFields) const
    {
        std::ostringstream stream;
        stream << "#pragma once\n#include <cstddef>\n#include <cstdint>\n#include \"Types.hpp\"\n\n";
        stream << "namespace AndUE\n{\n";
        if (reflection_.stats.parsedFunctions == 0)
            stream << "// No UFunction metadata was resolved in this dump.\n";
        for (const TypeIR &type : reflection_.types)
        {
            for (const FunctionIR &function : type.functions)
            {
                const std::string functionName = Sanitize(symbols.types.at(type.address).name + "_" + function.name, "Function_");
                stream << "// " << function.fullName << "\n";
                stream << "inline constexpr std::uintptr_t " << functionName
                       << "_NativeRva = " << Hex(function.nativeRva) << ";\n";
                stream << "inline constexpr std::size_t " << functionName << "_ParamsSize = " << function.paramSize << ";\n";
                stream << "struct " << functionName << "_Params\n{\n";
                WriteFields(stream, function.parameters, 0, function.paramSize, symbols, opaqueFields);
            }
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Enums(const CppSymbols &symbols) const
    {
        std::ostringstream stream;
        stream << "#pragma once\n#include <cstdint>\n\nnamespace AndUE\n{\n";
        if (reflection_.enums.empty())
            stream << "// No UEnum metadata was resolved in this dump.\n";
        for (const EnumIR &enumeration : reflection_.enums)
        {
            const CppEnumInfo &info = symbols.enums.at(enumeration.address);
            stream << "enum class " << info.name << " : "
                   << EnumUnderlyingName(info.underlyingType) << "\n{\n";
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
        stream << "  \"profile\": {\"id\":\"" << JsonEscape(schema.validation.profileId)
               << "\",\"label\":\"" << JsonEscape(schema.validation.profileLabel)
               << "\",\"version_range\":\"" << JsonEscape(schema.validation.profileVersionRange)
               << "\",\"layout\":\"" << SchemaLayoutVariantName(schema.layout) << "\"},\n";
        stream << "  \"module\": {\"name\":\"" << JsonEscape(context_.Module().name)
               << "\",\"base\":\"" << Hex(context_.Module().base) << "\",\"end\":\""
               << Hex(context_.Module().end) << "\",\"architecture\":\""
               << ArchitectureName(context_.Module().architecture) << "\",\"pointer_width\":"
               << static_cast<int>(context_.Module().pointerWidth) << "},\n";
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
        stream << "    \"fname\": {\"comparison_index\":" << schema.fname.comparisonIndex
               << ",\"number\":" << schema.fname.number
               << ",\"display_index\":" << schema.fname.displayIndex
               << ",\"size\":" << schema.fname.size
               << ",\"number_layout\":\""
               << (schema.fname.numberLayout == FNameNumberLayout::Outlined ? "outlined" : "inline")
               << "\",\"display_layout\":\""
               << (schema.fname.displayLayout == FNameDisplayLayout::BeforeNumber
                       ? "before-number"
                   : schema.fname.displayLayout == FNameDisplayLayout::AfterNumber ? "after-number"
                                                                                   : "none")
               << "\"},\n";
        stream << "    \"ffield\": {\"class\":" << schema.ffield.classPointer
               << ",\"owner\":" << schema.ffield.owner << ",\"next\":" << schema.ffield.next
               << ",\"name\":" << schema.ffield.name << "},\n";
        stream << "    \"ffield_class\": {\"name\":" << schema.ffieldClass.name
               << ",\"id\":" << schema.ffieldClass.id
               << ",\"cast_flags\":" << schema.ffieldClass.castFlags
               << ",\"class_flags\":" << schema.ffieldClass.classFlags
               << ",\"super_class\":" << schema.ffieldClass.superClass << "},\n";
        stream << "    \"ustruct\": {\"super\":" << schema.ustruct.superStruct
               << ",\"children\":" << schema.ustruct.children
               << ",\"child_properties\":" << schema.ustruct.childProperties
               << ",\"size\":" << schema.ustruct.propertiesSizeOffset << "},\n";
        stream << "    \"property\": {\"array_dim\":" << schema.property.arrayDim
               << ",\"element_size\":" << schema.property.elementSize
               << ",\"flags\":" << schema.property.propertyFlags
               << ",\"offset_internal\":" << schema.property.offsetInternal << "},\n";
        stream << "    \"ufunction\": {\"flags\":" << schema.ufunction.functionFlags
               << ",\"num_params\":" << schema.ufunction.numParams
               << ",\"param_size\":" << schema.ufunction.paramSize
               << ",\"return_value_offset\":" << schema.ufunction.returnValueOffset
               << ",\"native_function\":" << schema.ufunction.nativeFunction << "},\n";
        stream << "    \"uenum\": {\"names\":" << schema.uenum.names
               << ",\"underlying_type\":" << schema.uenum.underlyingType << "}\n";
        stream << "  }";

        if (!binding.commonObjects.empty())
        {
            stream << ",\n  \"common_objects\": [\n";
            for (size_t i = 0; i < binding.commonObjects.size(); ++i)
            {
                const auto &obj = binding.commonObjects[i];
                stream << "    {\"name\":\"" << JsonEscape(obj.name)
                       << "\",\"address\":\"" << Hex(obj.address)
                       << "\",\"index\":" << obj.index << "}";
                if (i + 1 != binding.commonObjects.size())
                    stream << ",";
                stream << "\n";
            }
            stream << "  ]";
        }

        stream << "\n}\n";
        return stream.str();
    }

    std::string ArtifactWriter::ReflectionJson() const
    {
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 1,\n";
        stream << "  \"status\": \"" << StatusName(reflection_.status) << "\",\n";
        stream << "  \"engine\": \"" << JsonEscape(context_.Schema().validation.familyEvidence) << "\",\n";
        stream << "  \"profile\": {\"id\":\""
               << JsonEscape(context_.Schema().validation.profileId) << "\",\"label\":\""
               << JsonEscape(context_.Schema().validation.profileLabel) << "\",\"version_range\":\""
               << JsonEscape(context_.Schema().validation.profileVersionRange) << "\"},\n";
        stream << "  \"stats\": {\"parsed_types\": " << reflection_.stats.parsedTypes
               << ", \"parsed_enums\": " << reflection_.stats.parsedEnums
               << ", \"parsed_functions\": " << reflection_.stats.parsedFunctions
               << ", \"parsed_properties\": " << reflection_.stats.parsedProperties
               << ", \"unknown_properties\": " << reflection_.stats.unknownProperties
               << ", \"unresolved_type_details\": " << reflection_.stats.unresolvedTypeDetails
               << ", \"object_slots\": " << reflection_.stats.objectSlots
               << ", \"valid_objects\": " << reflection_.stats.validObjects
               << ", \"skipped_objects\": " << reflection_.stats.skippedObjects
               << ", \"empty_object_slots\": " << reflection_.stats.emptyObjectSlots
               << ", \"object_read_failures\": " << reflection_.stats.objectReadFailures
               << ", \"class_name_read_failures\": " << reflection_.stats.classNameReadFailures
               << ", \"skipped_class_default_objects\": " << reflection_.stats.skippedClassDefaultObjects
               << ", \"skipped_incomplete_objects\": " << reflection_.stats.skippedIncompleteObjects
               << ", \"object_diagnostic_samples_omitted\": " << reflection_.stats.objectDiagnosticSamplesOmitted
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
                       << Hex(property.flags) << "\",\"kind\":\""
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
                       << function.paramSize << ",\"return_value_offset\":"
                       << function.returnValueOffset << ",\"header_num_params\":"
                       << static_cast<unsigned int>(function.headerNumParams) << ",\"header_param_size\":"
                       << function.headerParamSize << ",\"derived_num_params\":"
                       << function.derivedNumParams << ",\"derived_param_size\":"
                       << function.derivedParamSize << ",\"default_initializer_count\":"
                       << function.defaultInitializerCount << ",\"parameter_semantics_valid\":";
                WriteJsonBool(stream, function.parameterSemanticsValid);
                stream << ",\"parameter_semantics_consistent\":";
                WriteJsonBool(stream, function.parameterSemanticsConsistent);
                stream << ",\"parameters\":[";
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
        if ((context_.Module().pointerWidth != 4 && context_.Module().pointerWidth != 8) ||
            (context_.Schema().fname.size != 4 && context_.Schema().fname.size != 8 && context_.Schema().fname.size != 12))
        {
            result.error = "target pointer width or FName size is invalid for SDK description generation";
            return result;
        }
        if (std::any_of(reflection_.types.begin(), reflection_.types.end(), [](const TypeIR &type)
                        { return type.size < 0; }))
        {
            result.error = "negative reflected type size is invalid for SDK description generation";
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
        CppSymbols symbols = BuildCppSymbols();
        const std::string basicTypes = BasicTypes();
        const std::string types = Types(symbols, opaque);
        const std::string enums = Enums(symbols);
        const std::string functions = Functions(symbols, opaque);
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
