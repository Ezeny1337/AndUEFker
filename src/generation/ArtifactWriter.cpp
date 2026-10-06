#include "anduefker/generation/ArtifactWriter.hpp"
#include "anduefker/generation/JsonExport.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <array>
#include <chrono>
#include <cstdio>
#include <string_view>

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

        struct TemporaryDirectory
        {
            std::filesystem::path path;
            bool committed = false;
            ~TemporaryDirectory()
            {
                try
                {
                    if (!committed && !path.empty())
                    {
                        std::error_code error;
                        std::filesystem::remove_all(path, error);
                        if (error)
                            std::fprintf(stderr, "Temporary artifact cleanup failed: %s\n", error.message().c_str());
                    }
                }
                catch (...)
                {
                    std::fprintf(stderr, "Temporary artifact cleanup could not complete\n");
                }
            }
        };
    } // namespace

    ArtifactWriter::ArtifactWriter(const RuntimeContext &context,
                                   const ReflectionIR &reflection,
                                   std::filesystem::path outputRoot,
                                   std::string packageName)
        : context_(context), reflection_(reflection), outputRoot_(std::move(outputRoot)), packageName_(std::move(packageName))
    {
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

    std::string ArtifactWriter::ManifestJson(const GenerationReport &report, ParseStatus status) const
    {
        const ReflectionStats &stats = reflection_.stats;
        std::ostringstream stream;
        stream << "{\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"package\": \"" << EscapeJson(packageName_) << "\",\n";
        stream << "  \"engine\": \"" << EscapeJson(context_.Schema().validation.familyEvidence) << "\",\n";
        stream << "  \"profile\": {\"id\":\""
               << EscapeJson(context_.Schema().validation.profileId) << "\",\"label\":\""
               << EscapeJson(context_.Schema().validation.profileLabel) << "\",\"version_range\":\""
               << EscapeJson(context_.Schema().validation.profileVersionRange) << "\"},\n";
        stream << "  \"status\": \"" << ParseStatusName(status) << "\",\n";
        stream << "  \"reflection_status\": \"" << ParseStatusName(reflection_.status) << "\",\n";
        stream << "  \"sdk_status\": \"" << ParseStatusName(report.Status()) << "\",\n";
        stream << "  \"artifact_kind\": \"" << (status == ParseStatus::Partial ? "partial" : "complete") << "\",\n";
        stream << "  \"module\": \"" << EscapeJson(context_.Module().name) << "\",\n";
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
        stream << "    \"enum_read_failures\": " << stats.enumReadFailures << ",\n";
        stream << "    \"identity_failures\": " << stats.identityFailures << ",\n";
        stream << "    \"unvisited_objects\": " << stats.unvisitedObjects << ",\n";
        stream << "    \"opaque_fields\": " << report.opaqueFields << ",\n";
        stream << "    \"omitted_fields\": " << report.omittedFields << ",\n";
        stream << "    \"sdk_layout_warnings\": " << report.layoutWarnings << "\n";
        stream << "  },\n  \"capture\":";
        WriteCaptureJson(stream, reflection_.capture);
        stream << '\n';
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::DiagnosticsJson(const GenerationReport &report, ParseStatus status) const
    {
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 1,\n  \"status\": \""
               << ParseStatusName(status) << "\",\n";
        stream << "  \"reflection_status\":\"" << ParseStatusName(reflection_.status) << "\",\n";
        stream << "  \"sdk_status\":\"" << ParseStatusName(report.Status()) << "\",\n";
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
               << ", \"enum_read_failures\": " << reflection_.stats.enumReadFailures
               << ", \"identity_failures\": " << reflection_.stats.identityFailures
               << ", \"unvisited_objects\": " << reflection_.stats.unvisitedObjects
               << ", \"opaque_fields\": " << report.opaqueFields << ", \"omitted_fields\": " << report.omittedFields
               << ", \"sdk_layout_warnings\": " << report.layoutWarnings
               << ", \"failures\": " << reflection_.stats.failures << "},\n";
        stream << "  \"capture\":";
        WriteCaptureJson(stream, reflection_.capture);
        stream << ",\n  \"property_diagnostics\":";
        WritePropertyDiagnosticsJson(stream, reflection_);
        stream << ",\n  \"generation_report\":{\"total\":" << report.layoutWarnings
               << ",\"sample_limit_per_category\":8,\"samples_omitted\":" << (report.layoutWarnings - report.events.size())
               << ",\"legacy_messages_omitted\":" << (report.layoutWarnings - report.diagnostics.size())
               << ",\"counts_by_category\":{";
        bool firstCategory = true;
        for (const auto &[category, count] : report.counts)
        {
            if (!firstCategory)
                stream << ',';
            firstCategory = false;
            stream << '"' << EscapeJson(category) << "\":" << count;
        }
        stream << "},\"samples\":[";
        for (size_t index = 0; index < report.events.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &event = report.events[index];
            stream << "{\"category\":\"" << EscapeJson(event.category)
                   << "\",\"message\":\"" << EscapeJson(event.message)
                   << "\",\"owner_full_name\":\"" << EscapeJson(event.owner)
                   << "\",\"owner_address\":\"" << Hex(event.ownerAddress)
                   << "\",\"scope\":\"" << EscapeJson(event.scope)
                   << "\",\"super_type\":\"" << EscapeJson(event.super)
                   << "\",\"super_address\":\"" << Hex(event.superAddress)
                   << "\",\"type_size\":" << event.typeSize << ",\"base_size\":" << event.baseSize
                   << ",\"initial_cursor\":" << event.initialCursor
                   << ",\"property_name\":\"" << EscapeJson(event.property)
                   << "\",\"property_address\":\"" << Hex(event.propertyAddress)
                   << "\",\"property_offset\":" << event.offset << ",\"element_size\":" << event.elementSize
                   << ",\"array_dim\":" << event.arrayDim << ",\"property_end\":" << event.end
                   << ",\"cursor_before\":" << event.cursor << ",\"cursor_source\":\"" << EscapeJson(event.cursorSource)
                   << "\",\"conflicting_property\":\"" << EscapeJson(event.conflictingProperty)
                   << "\",\"conflicting_address\":\"" << Hex(event.conflictingAddress)
                   << "\",\"bool_layout\":{\"field_size\":" << static_cast<unsigned int>(event.boolean.fieldSize)
                   << ",\"byte_offset\":" << static_cast<unsigned int>(event.boolean.byteOffset)
                   << ",\"byte_mask\":" << static_cast<unsigned int>(event.boolean.byteMask)
                   << ",\"field_mask\":" << static_cast<unsigned int>(event.boolean.fieldMask)
                   << "},\"emission_strategy\":\"" << EscapeJson(event.strategy) << "\"}";
        }
        stream << "]}";
        stream << ",\n  \"generation_diagnostics\":[";
        for (size_t index = 0; index < report.diagnostics.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << '"' << EscapeJson(report.diagnostics[index]) << '"';
        }
        stream << "],\n";
        stream << "  \"diagnostics\": [";
        for (size_t index = 0; index < reflection_.diagnostics.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "\"" << EscapeJson(reflection_.diagnostics[index]) << "\"";
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
                stream << "    {\"type\":\"" << EscapeJson(type.fullName) << "\",\"address\":\"" << Hex(type.address)
                       << "\",\"message\":\""
                       << EscapeJson(conflict) << "\"}";
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
                    stream << "    {\"function\":\"" << EscapeJson(function.fullName) << "\",\"address\":\"" << Hex(function.address)
                           << "\",\"message\":\""
                           << EscapeJson(conflict) << "\"}";
                }
            }
        }
        stream << "\n  ],\n  \"enum_conflicts\": [\n";
        conflictCount = 0;
        for (const auto &enumeration : reflection_.enums)
        {
            for (const auto &message : enumeration.diagnostics)
            {
                if (conflictCount++ != 0)
                    stream << ",\n";
                stream << "    {\"enum\":\"" << EscapeJson(enumeration.fullName) << "\",\"address\":\""
                       << Hex(enumeration.address) << "\",\"message\":\"" << EscapeJson(message) << "\"}";
            }
        }
        stream << "\n  ]\n}\n";
        return stream.str();
    }

    void ArtifactWriter::WriteFields(std::ostringstream &stream,
                                     const TypeIR &owner,
                                     const FunctionIR *function,
                                     const std::vector<PropertyIR> &properties,
                                     int32_t initialOffset,
                                     int32_t size,
                                     const CppSymbols &symbols,
                                     GenerationReport &report) const
    {
        size_t &opaqueFields = report.opaqueFields;
        struct BoolStorage
        {
            int32_t end = 0;
            uint64_t masks = 0;
            const PropertyIR *firstProperty = nullptr;
        };
        std::unordered_map<int32_t, BoolStorage> boolStorage;
        std::vector<const PropertyIR *> ordered;
        ordered.reserve(properties.size());
        for (const PropertyIR &property : properties)
            ordered.push_back(&property);
        std::stable_sort(ordered.begin(), ordered.end(), [](const PropertyIR *left, const PropertyIR *right)
                         { return left->offset < right->offset; });

        int32_t cursor = initialOffset;
        const PropertyIR *cursorProperty = nullptr;
        size_t ordinal = 0;
        const auto warn = [&](const PropertyIR &property, const std::string &category,
                              const std::string &message, const char *strategy, const PropertyIR *conflicting)
        {
            LayoutEvent event;
            event.category = category;
            event.message = message;
            event.owner = function ? function->fullName : owner.fullName;
            event.ownerAddress = function ? function->address : owner.address;
            event.scope = function ? "function-parameters" : "type-fields";
            event.superAddress = function ? 0 : owner.superAddress;
            const auto base = symbols.types.find(event.superAddress);
            if (!function && base != symbols.types.end())
            {
                event.super = base->second.name;
                event.baseSize = base->second.size;
            }
            event.typeSize = size;
            event.initialCursor = initialOffset;
            event.property = property.name;
            event.propertyAddress = property.address;
            event.offset = property.offset;
            event.elementSize = property.elementSize;
            event.arrayDim = property.arrayDim;
            event.end = static_cast<int64_t>(property.offset) + static_cast<int64_t>(property.elementSize) * property.arrayDim;
            event.cursor = cursor;
            event.cursorSource = cursorProperty ? "previous-field" : (initialOffset != 0 ? "base-extent" : "origin");
            if (conflicting)
            {
                event.conflictingAddress = conflicting->address;
                event.conflictingProperty = conflicting->name;
            }
            else if (property.offset < initialOffset)
                event.conflictingAddress = owner.superAddress;
            event.boolean = property.boolean;
            event.strategy = strategy;
            if (property.type.kind != PropertyKind::Bool && strategy != std::string_view("omitted"))
            {
                const bool details = property.typeDetailsResolved && property.type.elementSize == property.elementSize;
                const auto cppType = details ? PropertyType(property.type, symbols, context_.Module().pointerWidth,
                                                            context_.Schema().fname.size)
                                             : std::string{};
                event.strategy = cppType.empty() ? "opaque-storage-and-offset" : "typed-member-and-offset";
            }
            report.Warn(std::move(event));
        };
        for (const PropertyIR *entry : ordered)
        {
            const PropertyIR &property = *entry;
            const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
            const int64_t end = static_cast<int64_t>(property.offset) + total;
            const std::string member = SanitizeIdentifier(property.name, "Member_") + "_" + std::to_string(ordinal++);
            if (property.elementSize <= 0 || property.arrayDim <= 0 || property.offset < 0 || end > size)
            {
                ++report.omittedFields;
                warn(property, "invalid-property-bounds", "field omitted: " + property.name + " address=" + Hex(property.address),
                     "omitted", nullptr);
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
                        warn(property, existing->second.end != end ? "bool-storage-size-conflict" : "bool-mask-conflict",
                             "bool storage conflict: " + property.name + " address=" + Hex(property.address),
                             "shared-storage-and-offsets", existing->second.firstProperty);
                        stream << "    // Conflicting bool storage size or mask: " << member
                               << ", offset=" << Hex(property.offset) << ", size=" << Hex(static_cast<uint64_t>(total)) << "\n";
                    }
                    existing->second.masks |= mask;
                }
                else
                {
                    if (property.offset < cursor)
                    {
                        warn(property, property.offset < initialOffset ? "inherited-extent-intersection" : "bool-storage-overlap",
                             "bool storage overlap: " + property.name + " address=" + Hex(property.address),
                             "sequential-bool-storage-and-offsets", cursorProperty);
                        stream << "    // Bool storage overlaps the base or an existing field: " << member << "\n";
                    }
                    if (property.offset > cursor)
                        stream << "    std::uint8_t Pad_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
                    const std::string storageName = "BoolStorage_" + std::to_string(ordinal++);
                    stream << "    std::uint8_t " << storageName << "[" << Hex(static_cast<uint64_t>(total)) << "]; // "
                           << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ")\n";
                    boolStorage.emplace(property.offset, BoolStorage{static_cast<int32_t>(end), mask, &property});
                    if (end > cursor)
                        cursorProperty = &property;
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
            {
                warn(property, property.offset < initialOffset ? "inherited-extent-intersection" : "same-owner-overlap",
                     "field overlap: " + property.name + " address=" + Hex(property.address),
                     "sequential-member-and-offset", cursorProperty);
                stream << "    // Overlapping reflected field; use its explicit offset: " << member << "\n";
            }
            if (property.offset > cursor)
                stream << "    std::uint8_t Pad_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
            const bool hasTypeDetails = property.type.kind != PropertyKind::Bool && property.typeDetailsResolved &&
                                        property.type.elementSize == property.elementSize;
            const std::string cppType = hasTypeDetails
                                            ? PropertyType(property.type, symbols,
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
                if (property.type.kind == PropertyKind::Set || property.type.kind == PropertyKind::Map)
                    ++opaqueFields;
            }
            stream << "    static constexpr std::size_t " << member << "_Offset = " << property.offset << ";\n";
            if (end > cursor)
                cursorProperty = &property;
            cursor = std::max(cursor, static_cast<int32_t>(end));
        }
        if (cursor < size)
            stream << "    std::uint8_t TailData[" << Hex(size - cursor) << "];\n";
        stream << "};\n";
        stream << '\n';
    }

    std::string ArtifactWriter::Types(const CppSymbols &symbols, GenerationReport &report) const
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
                report.Warn("declaration-dependency", "cyclic or unresolved declaration dependencies");
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
            {
                LayoutEvent event;
                event.category = "missing-base-extent";
                event.message = "base size unavailable: " + type.fullName;
                event.owner = type.fullName;
                event.ownerAddress = type.address;
                event.superAddress = type.superAddress;
                event.typeSize = type.size;
                event.scope = "type-fields";
                event.strategy = "full-reflected-range";
                if (base != types.end())
                {
                    event.super = base->second.name;
                    event.baseSize = base->second.size;
                }
                report.Warn(std::move(event));
                stream << "// Base size is unavailable or inconsistent; padding uses the full reflected range.\n";
            }
            stream << "// Reflected size: " << Hex(static_cast<uint32_t>(type.size)) << "\n";
            stream << "struct " << typeName;
            if (base != types.end())
                stream << " : public " << base->second.name;
            if (baseSizeValid)
                initialOffset = base->second.size;
            stream << "\n{\n";
            WriteFields(stream, type, nullptr, type.properties, initialOffset, type.size, symbols, report);
        }
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::Functions(const CppSymbols &symbols, GenerationReport &report) const
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
                const std::string &functionName = symbols.functions.at({type.address, function.address});
                stream << "// " << function.fullName << "\n";
                stream << "inline constexpr std::uintptr_t " << functionName
                       << "_NativeRva = " << Hex(function.nativeRva) << ";\n";
                stream << "inline constexpr std::size_t " << functionName << "_ParamsSize = " << function.paramSize << ";\n";
                stream << "struct " << functionName << "_Params\n{\n";
                WriteFields(stream, type, &function, function.parameters, 0, function.paramSize, symbols, report);
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
            for (size_t index = 0; index < enumeration.values.size(); ++index)
            {
                stream << "    " << info.values[index] << " = ";
                if (info.underlyingType == EnumUnderlyingType::UInt64)
                    stream << Hex(static_cast<uint64_t>(enumeration.values[index].value));
                else
                    stream << enumeration.values[index].value;
                stream << ",\n";
            }
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
        stream << "  \"engine\": \"" << EscapeJson(schema.validation.familyEvidence) << "\",\n";
        stream << "  \"profile\": {\"id\":\"" << EscapeJson(schema.validation.profileId)
               << "\",\"label\":\"" << EscapeJson(schema.validation.profileLabel)
               << "\",\"version_range\":\"" << EscapeJson(schema.validation.profileVersionRange)
               << "\",\"layout\":\"" << SchemaLayoutVariantName(schema.layout) << "\"},\n";
        stream << "  \"module\": {\"name\":\"" << EscapeJson(context_.Module().name)
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
        stream << "}\n  },\n  \"schema\": {\n";
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
               << ",\"name\":" << schema.ffield.name
               << ",\"owner_encoding\":\"" << (schema.features.fFieldOwnerEncoding == ::anduefker::ue::FFieldOwnerEncoding::TaggedPointer ? "tagged-pointer" : "explicit-discriminator") << "\"},\n";
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
        stream << "    \"property_subtypes\": {\"bool_base\":" << schema.propertySubtypes.boolBase
               << ",\"byte_enum\":" << schema.propertySubtypes.byteEnum
               << ",\"object_class\":" << schema.propertySubtypes.objectClass
               << ",\"class_meta_class\":" << schema.propertySubtypes.classMetaClass
               << ",\"struct_type\":" << schema.propertySubtypes.structType
               << ",\"array_inner\":" << schema.propertySubtypes.arrayInner
               << ",\"set_element\":" << schema.propertySubtypes.setElement
               << ",\"map_base\":" << schema.propertySubtypes.mapBase
               << ",\"enum_base\":" << schema.propertySubtypes.enumBase
               << ",\"delegate_signature\":" << schema.propertySubtypes.delegateSignature
               << ",\"field_path_class\":" << schema.propertySubtypes.fieldPathClass
               << ",\"optional_value\":" << schema.propertySubtypes.optionalValue << "},\n";
        stream << "    \"ufunction\": {\"flags\":" << schema.ufunction.functionFlags
               << ",\"num_params\":" << schema.ufunction.numParams
               << ",\"param_size\":" << schema.ufunction.paramSize
               << ",\"return_value_offset\":" << schema.ufunction.returnValueOffset
               << ",\"native_function\":" << schema.ufunction.nativeFunction << "},\n";
        stream << "    \"uenum\": {\"names\":" << schema.uenum.names
               << ",\"underlying_type\":" << schema.uenum.underlyingType
               << ",\"cpp_form\":" << schema.uenum.cppForm << ",\"flags\":" << schema.uenum.flags
               << ",\"package\":" << schema.uenum.enumPackage
               << ",\"cpp_form_is_byte\":" << (schema.features.enumCppFormIsByte ? "true" : "false")
               << ",\"flags_is_byte\":" << (schema.features.enumFlagsIsByte ? "true" : "false") << "}\n";
        stream << "  }";

        if (!binding.commonObjects.empty())
        {
            stream << ",\n  \"common_objects\": [\n";
            for (size_t i = 0; i < binding.commonObjects.size(); ++i)
            {
                const auto &obj = binding.commonObjects[i];
                stream << "    {\"name\":\"" << EscapeJson(obj.name)
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
        const auto &validation = context_.Schema().validation;
        WriteReflectionJson(stream, reflection_, {validation.familyEvidence, validation.profileId, validation.profileLabel, validation.profileVersionRange});
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
        const std::string packageStem = SanitizeIdentifier(packageName_, "Package");
        GenerationReport report;
        const CppSymbols symbols = BuildCppSymbols(reflection_);
        const std::string basicTypes = BasicTypes();
        const std::string types = Types(symbols, report);
        const std::string enums = Enums(symbols);
        const std::string functions = Functions(symbols, report);
        const ParseStatus status = reflection_.status == ParseStatus::Partial || report.Status() == ParseStatus::Partial
                                       ? ParseStatus::Partial
                                       : ParseStatus::Complete;
        result.reflectionStatus = reflection_.status;
        result.sdkStatus = report.Status();
        result.opaqueFields = report.opaqueFields;
        result.omittedFields = report.omittedFields;
        result.layoutWarnings = report.layoutWarnings;
        for (const auto &[category, count] : report.counts)
            result.generationDiagnostics.push_back("SDK layout summary: category=" + category +
                                                   " total=" + std::to_string(count) +
                                                   " samples_omitted=" + std::to_string(count > 8 ? count - 8 : 0));
        for (const auto &event : report.events)
            result.generationDiagnostics.push_back("SDK layout event: category=" + event.category +
                                                   " owner=" + event.owner + " property=" + event.property +
                                                   " address=" + Hex(event.propertyAddress) +
                                                   " offset=" + std::to_string(event.offset) +
                                                   " end=" + std::to_string(event.end) +
                                                   " base_size=" + std::to_string(event.baseSize) +
                                                   " cursor=" + std::to_string(event.cursor) +
                                                   " cursor_source=" + event.cursorSource);
        const std::string artifactStem = packageStem + (status == ParseStatus::Partial ? ".partial" : "");
        const std::filesystem::path finalPath = outputRoot_ / artifactStem;
        if (std::filesystem::exists(finalPath, error))
        {
            result.error = "output directory already exists";
            return result;
        }
        if (error)
        {
            result.error = "output directory check failed: " + error.message();
            return result;
        }
        TemporaryDirectory transaction;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto candidate = outputRoot_ / ("." + artifactStem + ".tmp." + std::to_string(stamp) + "." + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error))
            {
                transaction.path = candidate;
                break;
            }
            if (error == std::errc::file_exists)
                error.clear();
            else if (error)
                break;
        }
        if (transaction.path.empty())
        {
            result.error = "temporary output directory creation failed";
            return result;
        }
        const std::filesystem::path &temporary = transaction.path;
        const std::string reflectionJson = ReflectionJson();
        const std::string manifestJson = ManifestJson(report, status);
        const std::string diagnosticsJson = DiagnosticsJson(report, status);
        const std::string runtimeJson = RuntimeJson();
        // 视图引用本次调用持有的字符串，避免再复制整份大型反射 JSON。
        const std::array<std::pair<std::string_view, std::string_view>, 8> files = {{
            {"BasicTypes.hpp", basicTypes},
            {"Types.hpp", types},
            {"Enums.hpp", enums},
            {"Functions.hpp", functions},
            {"reflection.json", reflectionJson},
            {"manifest.json", manifestJson},
            {"diagnostics.json", diagnosticsJson},
            {"runtime.json", runtimeJson},
        }};
        for (const auto &[name, content] : files)
        {
            std::ofstream stream(temporary / name, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                result.error = "output file open failed: " + std::string(name);
                return result;
            }
            if (content.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
            {
                result.error = "output file size exceeds stream limit: " + std::string(name);
                return result;
            }
            stream.write(content.data(), static_cast<std::streamsize>(content.size()));
            stream.flush();
            if (!stream.good())
            {
                result.error = "output file write or flush failed: " + std::string(name);
                return result;
            }
            stream.close();
            if (stream.fail())
            {
                result.error = "output file close failed: " + std::string(name);
                return result;
            }
            ++result.filesWritten;
        }
        std::filesystem::rename(temporary, finalPath, error);
        if (error)
        {
            result.error = "output directory commit failed";
            return result;
        }
        transaction.committed = true;
        result.outputPath = finalPath;
        result.status = status;
        return result;
    }
} // namespace anduefker::generation
