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
        const auto &identity = context_.Schema().identity;
        stream << "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n";
        stream << "// For inspection and analysis; reflected offsets and sizes are authoritative.\n";
        stream << "// Canonical profile: " << identity.canonicalProfileId << " (" << identity.canonicalVersionRange << ").\n";
        stream << "// Runtime layout: " << identity.layoutProfileId << "; layout confidence: validated.\n";
        stream << "// Engine-version confidence: " << identity.versionConfidence
               << " (selection: " << identity.selectionReason << ").\n";
        stream << "// Layout-compatible profiles:";
        for (const auto &profile : identity.compatibleProfiles)
            stream << " " << profile;
        stream << "\n";
        for (const auto &evidence : identity.evidence)
            stream << "// Version evidence: " << evidence.kind << "=" << evidence.observed
                   << " [" << ::anduefker::ue::VersionEvidenceStrengthName(evidence.strength) << "]\n";
        stream << "// Capture observations stable: " << (reflection_.capture.observationsStable ? "yes" : "no")
               << "; changed ranges: " << reflection_.capture.changedRanges
               << "; read failures: " << reflection_.capture.readFailures << ".\n";
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
        stream << "  \"schema_version\": 2,\n";
        stream << "  \"package\": \"" << EscapeJson(packageName_) << "\",\n";
        stream << "  \"engine\": \"" << EscapeJson(context_.Schema().identity.canonicalVersionRange.empty() ? context_.Schema().validation.familyEvidence : context_.Schema().identity.canonicalVersionRange) << "\",\n";
        stream << "  \"profile\": {\"id\":\""
               << EscapeJson(context_.Schema().validation.profileId) << "\",\"label\":\""
               << EscapeJson(context_.Schema().validation.profileLabel) << "\",\"version_range\":\""
               << EscapeJson(context_.Schema().validation.profileVersionRange) << "\"},\n";
        stream << "  \"schema_identity\":";
        WriteSchemaIdentityJson(stream, context_.Schema().identity);
        stream << ",\n";
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
        stream << "    \"sdk_layout_warnings\": " << report.layoutWarnings << ",\n";
        stream << "    \"sdk_layout_events\": " << report.layoutEvents << "\n";
        stream << "  },\n  \"capture\":";
        WriteCaptureJson(stream, reflection_.capture);
        stream << '\n';
        stream << "}\n";
        return stream.str();
    }

    std::string ArtifactWriter::DiagnosticsJson(const GenerationReport &report, ParseStatus status) const
    {
        std::ostringstream stream;
        stream << "{\n  \"schema_version\": 2,\n  \"status\": \""
               << ParseStatusName(status) << "\",\n";
        stream << "  \"reflection_status\":\"" << ParseStatusName(reflection_.status) << "\",\n";
        stream << "  \"sdk_status\":\"" << ParseStatusName(report.Status()) << "\",\n";
        stream << "  \"schema_identity\":";
        WriteSchemaIdentityJson(stream, context_.Schema().identity);
        stream << ",\n";
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
               << ", \"sdk_layout_events\": " << report.layoutEvents
               << ", \"failures\": " << reflection_.stats.failures << "},\n";
        stream << "  \"capture\":";
        WriteCaptureJson(stream, reflection_.capture);
        stream << ",\n  \"property_diagnostics\":";
        WritePropertyDiagnosticsJson(stream, reflection_);
        stream << ",\n  \"generation_report\":{\"total\":" << report.layoutEvents
               << ",\"warnings\":" << report.layoutWarnings
               << ",\"sample_limit_per_category\":8,\"samples_omitted\":" << (report.layoutEvents > report.events.size() ? report.layoutEvents - report.events.size() : 0)
               << ",\"legacy_messages_omitted\":" << (report.layoutWarnings > report.diagnostics.size() ? report.layoutWarnings - report.diagnostics.size() : 0)
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
            stream << "{\"severity\":\"" << EscapeJson(event.severity)
                   << "\",\"category\":\"" << EscapeJson(event.category)
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

    ArtifactWriter::FieldGenerationPlan ArtifactWriter::BuildFieldGenerationPlan(
        const TypeIR &owner, const FunctionIR *function, const std::vector<PropertyIR> &properties,
        int32_t size, const CppSymbols &symbols) const
    {
        const auto &analysis = function ? function->layout : owner.layout;
        FieldGenerationPlan plan;
        plan.representation = analysis.representation;
        plan.initialOffset = analysis.baseExtentKnown ? analysis.baseExtent : 0;
        plan.size = size;
        plan.fields.reserve(properties.size());
        for (const PropertyIR &property : properties)
        {
            const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
            int64_t end = static_cast<int64_t>(property.offset) + total;
            FieldGenerationEntry entry;
            entry.property = &property;
            entry.validBounds = ::anduefker::ir::IsValidPropertyBounds(property, size, end);
            entry.boolLayout = ::anduefker::ir::IsValidPropertyBoolLayout(property);
            if (property.type.kind != PropertyKind::Bool && property.typeDetailsResolved &&
                property.type.elementSize == property.elementSize)
                entry.cppType = PropertyType(property.type, symbols, context_.Module().pointerWidth,
                                             context_.Schema().fname.size);
            plan.fields.push_back(std::move(entry));
        }
        return plan;
    }

    void ArtifactWriter::WriteFields(std::ostringstream &stream,
                                     const TypeIR &owner,
                                     const FunctionIR *function,
                                     const FieldGenerationPlan &plan,
                                     const CppSymbols &symbols,
                                     GenerationReport &report) const
    {
        size_t &opaqueFields = report.opaqueFields;
        const int32_t size = plan.size;
        const int32_t initialOffset = plan.initialOffset;
        const auto &analysis = function ? function->layout : owner.layout;
        const std::string scope = function ? "function-parameters" : "type-fields";
        for (const auto &issue : analysis.issues)
        {
            LayoutEvent event;
            event.category = LayoutIssueKindName(issue.kind);
            event.message = issue.message;
            event.owner = function ? function->fullName : owner.fullName;
            event.ownerAddress = function ? function->address : owner.address;
            event.scope = scope;
            event.superAddress = function ? 0 : owner.superAddress;
            event.typeSize = size;
            event.initialCursor = initialOffset;
            event.conflictingAddress = issue.conflictingAddress;
            event.cursorSource = "layout-analysis";
            const auto base = symbols.types.find(event.superAddress);
            if (!function && base != symbols.types.end())
            {
                event.super = base->second.name;
                event.baseSize = base->second.size;
            }
            const auto property = std::find_if(plan.fields.begin(), plan.fields.end(), [&](const FieldGenerationEntry &entry)
                                               { return entry.property->address == issue.propertyAddress; });
            if (property != plan.fields.end())
            {
                event.property = property->property->name;
                event.propertyAddress = property->property->address;
                event.offset = property->property->offset;
                event.elementSize = property->property->elementSize;
                event.arrayDim = property->property->arrayDim;
                event.end = static_cast<int64_t>(property->property->offset) +
                            static_cast<int64_t>(property->property->elementSize) * property->property->arrayDim;
                event.boolean = property->property->boolean;
            }
            const auto conflicting = std::find_if(plan.fields.begin(), plan.fields.end(), [&](const FieldGenerationEntry &entry)
                                                  { return entry.property->address == issue.conflictingAddress; });
            if (conflicting != plan.fields.end())
                event.conflictingProperty = conflicting->property->name;
            event.strategy = LayoutRepresentationName(analysis.representation);
            if (issue.kind == LayoutIssueKind::InheritedExtentIntersection && !issue.affectsCompleteness)
                report.Info(std::move(event));
            else
                report.Warn(std::move(event));
        }
        if (analysis.representation == LayoutRepresentation::OffsetDescription)
        {
            LayoutEvent event;
            event.category = "offset-description";
            event.message = "fields use explicit offset descriptions; sequential member order is not asserted";
            event.owner = function ? function->fullName : owner.fullName;
            event.ownerAddress = function ? function->address : owner.address;
            event.scope = scope;
            event.superAddress = function ? 0 : owner.superAddress;
            event.typeSize = size;
            event.initialCursor = initialOffset;
            event.strategy = "offset-description";
            report.Info(std::move(event));
            stream << "    // Representation: offset-description; reflected order is not asserted.\n";
            stream << "    // Native data size is the reflected type/parameter size; unreflected bytes are not inferred as padding.\n";
            size_t ordinal = 0;
            for (const FieldGenerationEntry &entry : plan.fields)
            {
                const PropertyIR &property = *entry.property;
                const std::string member = SanitizeIdentifier(property.name, "Member_") + "_" + std::to_string(ordinal++);
                const std::string typeName = entry.cppType.empty() ? property.reflectedClass : entry.cppType;
                if (!entry.validBounds)
                {
                    stream << "    // Invalid offset description: name=" << property.name << " symbol=" << member << " type=" << typeName
                           << " offset=" << property.offset << " element_size=" << property.elementSize
                           << " array_dim=" << property.arrayDim << "\n";
                    ++opaqueFields;
                    continue;
                }
                stream << "    // OffsetDescription name=" << property.name << " symbol=" << member << " type=" << typeName
                       << " offset=" << Hex(static_cast<uint64_t>(property.offset))
                       << " element_size=" << property.elementSize << " array_dim=" << property.arrayDim << "\n";
                stream << "    static constexpr std::size_t " << member << "_Offset = " << property.offset << ";\n";
                stream << "    static constexpr std::size_t " << member << "_ElementSize = " << property.elementSize << ";\n";
                stream << "    static constexpr std::size_t " << member << "_ArrayDim = " << property.arrayDim << ";\n";
                if (property.type.kind != PropertyKind::Bool &&
                    (entry.cppType.empty() || property.type.kind == PropertyKind::Set || property.type.kind == PropertyKind::Map))
                    ++opaqueFields;
                if (entry.boolLayout)
                {
                    stream << "    static constexpr std::uint8_t " << member << "_ByteOffset = "
                           << static_cast<unsigned int>(property.boolean.byteOffset) << ";\n";
                    stream << "    static constexpr std::uint8_t " << member << "_ByteMask = "
                           << Hex(property.boolean.byteMask) << ";\n";
                    stream << "    static constexpr std::uint8_t " << member << "_Mask = "
                           << Hex(property.boolean.fieldMask) << ";\n";
                }
            }
            return;
        }

        struct BoolStorage
        {
            int32_t end = 0;
            uint64_t masks = 0;
            const PropertyIR *firstProperty = nullptr;
        };
        std::unordered_map<int32_t, BoolStorage> boolStorage;
        std::vector<const FieldGenerationEntry *> ordered;
        ordered.reserve(plan.fields.size());
        for (const FieldGenerationEntry &entry : plan.fields)
            ordered.push_back(&entry);
        std::stable_sort(ordered.begin(), ordered.end(), [](const FieldGenerationEntry *left, const FieldGenerationEntry *right)
                         { return left->property->offset < right->property->offset; });

        int32_t cursor = initialOffset;
        size_t ordinal = 0;
        for (const FieldGenerationEntry *entry : ordered)
        {
            const PropertyIR &property = *entry->property;
            const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
            const int64_t end = static_cast<int64_t>(property.offset) + total;
            const std::string member = SanitizeIdentifier(property.name, "Member_") + "_" + std::to_string(ordinal++);
            if (!entry->validBounds)
            {
                ++report.omittedFields;
                report.Warn("invalid-property-bounds", "field omitted: " + property.name + " address=" + Hex(property.address));
                stream << "    // Field has invalid dimensions or exceeds the reflected size: " << member << "\n";
                continue;
            }

            const bool boolLayout = entry->boolLayout;
            if (boolLayout)
            {
                // ByteOffset 是存储内部的字节位置，不能加到存储起点后再占用整个 FieldSize。
                const uint64_t mask = static_cast<uint64_t>(property.boolean.fieldMask) << (property.boolean.byteOffset * 8);
                auto existing = boolStorage.find(property.offset);
                if (existing != boolStorage.end())
                {
                    if (existing->second.end != end || (existing->second.masks & mask) != 0)
                    {
                        stream << "    // Unexpected bool analysis conflict: " << member << "\n";
                    }
                    existing->second.masks |= mask;
                }
                else
                {
                    if (property.offset < cursor)
                    {
                        stream << "    // Unexpected bool interval overlap after layout analysis: " << member << "\n";
                    }
                    if (property.offset > cursor)
                        stream << "    std::uint8_t UnknownData_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
                    const std::string storageName = "BoolStorage_" + std::to_string(ordinal++);
                    stream << "    std::uint8_t " << storageName << "[" << Hex(static_cast<uint64_t>(total)) << "]; // "
                           << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ")\n";
                    boolStorage.emplace(property.offset, BoolStorage{static_cast<int32_t>(end), mask, &property});
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
                stream << "    // Unexpected field interval overlap after layout analysis: " << member << "\n";
            if (property.offset > cursor)
                stream << "    std::uint8_t UnknownData_" << ordinal++ << "[" << Hex(property.offset - cursor) << "];\n";
            if (entry->cppType.empty())
            {
                stream << "    std::uint8_t " << member << "[" << Hex(static_cast<uint64_t>(total))
                       << "]; // " << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ") opaque\n";
                ++opaqueFields;
            }
            else
            {
                stream << "    " << entry->cppType << " " << member;
                if (property.arrayDim > 1)
                    stream << "[" << property.arrayDim << "]";
                stream << "; // " << Hex(property.offset) << " (" << Hex(static_cast<uint64_t>(total)) << ")\n";
                if (property.type.kind == PropertyKind::Set || property.type.kind == PropertyKind::Map)
                    ++opaqueFields;
            }
            stream << "    static constexpr std::size_t " << member << "_Offset = " << property.offset << ";\n";
            cursor = std::max(cursor, static_cast<int32_t>(end));
        }
        if (cursor < size)
            stream << "    std::uint8_t UnknownTailData[" << Hex(size - cursor) << "];\n";
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
            const auto base = types.find(type.superAddress);
            const bool baseSizeValid = base != types.end() && base->second.size >= 0 && base->second.size <= type.size;
            if (type.superAddress != 0 && !baseSizeValid)
            {
                LayoutEvent event;
                event.severity = "error";
                event.category = "missing-base-extent";
                event.message = "base size unavailable: " + type.fullName;
                event.owner = type.fullName;
                event.ownerAddress = type.address;
                event.superAddress = type.superAddress;
                event.typeSize = type.size;
                event.scope = "type-fields";
                event.strategy = "offset-description";
                if (base != types.end())
                {
                    event.super = base->second.name;
                    event.baseSize = base->second.size;
                }
                report.Warn(std::move(event));
                stream << "// Base extent is unavailable or inconsistent; fields use explicit target offsets where required.\n";
            }
            stream << "// Reflected size: " << Hex(static_cast<uint32_t>(type.size)) << "\n";
            stream << "struct " << typeName;
            if (base != types.end())
                stream << " : public " << base->second.name;
            stream << "\n{\n";
            const FieldGenerationPlan plan = BuildFieldGenerationPlan(type, nullptr, type.properties, type.size, symbols);
            WriteFields(stream, type, nullptr, plan, symbols, report);
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
                const FieldGenerationPlan plan = BuildFieldGenerationPlan(type, &function, function.parameters,
                                                                          function.paramSize, symbols);
                WriteFields(stream, type, &function, plan, symbols, report);
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
        stream << "{\n  \"schema_version\": 2,\n";
        stream << "  \"engine\": \"" << EscapeJson(schema.identity.canonicalVersionRange.empty() ? schema.validation.familyEvidence : schema.identity.canonicalVersionRange) << "\",\n";
        stream << "  \"profile\": {\"id\":\"" << EscapeJson(schema.validation.profileId)
               << "\",\"label\":\"" << EscapeJson(schema.validation.profileLabel)
               << "\",\"version_range\":\"" << EscapeJson(schema.validation.profileVersionRange)
               << "\",\"layout\":\"" << SchemaLayoutVariantName(schema.layout) << "\"},\n";
        stream << "  \"schema_identity\":";
        WriteSchemaIdentityJson(stream, schema.identity);
        stream << ",\n";
        stream << "  \"feature_evidence\": {\"observed_large_world_coordinates\":"
               << (schema.features.largeWorldCoordinates ? "true" : "false")
               << ",\"profile_expected_large_world_coordinates\":"
               << (schema.profileFeatures.largeWorldCoordinates ? "true" : "false")
               << ",\"observed_owner_encoding\":\""
               << (schema.features.fFieldOwnerEncoding == ::anduefker::ue::FFieldOwnerEncoding::TaggedPointer
                       ? "tagged-pointer"
                       : "explicit-discriminator")
               << "\",\"profile_expected_owner_encoding\":\""
               << (schema.profileFeatures.fFieldOwnerEncoding == ::anduefker::ue::FFieldOwnerEncoding::TaggedPointer
                       ? "tagged-pointer"
                       : "explicit-discriminator")
               << "\",\"observed_enum_tail\":\"" << EnumTailLayoutName(schema.features.enumTailLayout)
               << "\",\"profile_expected_enum_tail\":\"" << EnumTailLayoutName(schema.profileFeatures.enumTailLayout)
               << "\",\"optional_property_sample_count\":" << schema.optionalPropertySupport.sampleCount << "},\n";
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
        stream << "    \"property_tail\": {\"layout\":\""
               << (schema.features.propertyTailLayout == ::anduefker::ue::PropertyTailLayout::UProperty ? "uproperty" : schema.features.propertyTailLayout == ::anduefker::ue::PropertyTailLayout::RepNotifyBeforeLinks ? "repnotify-before-links"
                                                                                                                    : schema.features.propertyTailLayout == ::anduefker::ue::PropertyTailLayout::LinksBeforeRepNotify   ? "links-before-repnotify"
                                                                                                                                                                                                                        : "unknown")
               << "\",\"rep_notify\":" << schema.property.repNotify
               << ",\"property_links\":" << schema.property.propertyLinks
               << ",\"property_links_end\":" << schema.property.propertyLinksEnd
               << ",\"subtype_start\":" << schema.property.subtypeStart << "},\n";
        stream << "    \"property_subtypes\": {\"bool_base\":" << schema.propertySubtypes.boolBase
               << ",\"byte_enum\":" << schema.propertySubtypes.byteEnum
               << ",\"object_class\":" << schema.propertySubtypes.objectClass
               << ",\"interface_class\":" << schema.propertySubtypes.interfaceClass
               << ",\"class_meta_class\":" << schema.propertySubtypes.classMetaClass
               << ",\"struct_type\":" << schema.propertySubtypes.structType
               << ",\"array_inner\":" << schema.propertySubtypes.arrayInner
               << ",\"set_element\":" << schema.propertySubtypes.setElement
               << ",\"map_base\":" << schema.propertySubtypes.mapBase
               << ",\"enum_base\":" << schema.propertySubtypes.enumBase
               << ",\"delegate_signature\":" << schema.propertySubtypes.delegateSignature
               << ",\"field_path_class\":" << schema.propertySubtypes.fieldPathClass
               << ",\"optional_value\":" << schema.propertySubtypes.optionalValue << "},\n";
        stream << "    \"optional_property_support\": {\"present_in_profile\":"
               << (schema.optionalPropertySupport.presentInProfile ? "true" : "false")
               << ",\"sample_count\":" << schema.optionalPropertySupport.sampleCount
               << ",\"selected_offset\":" << schema.optionalPropertySupport.selectedOffset
               << ",\"confidence\":\"" << EscapeJson(schema.optionalPropertySupport.confidence)
               << "\",\"source\":\"" << EscapeJson(schema.optionalPropertySupport.source)
               << "\",\"reason\":\"" << EscapeJson(schema.optionalPropertySupport.reason) << "\"},\n";
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
        WriteReflectionJson(stream, reflection_, {context_.Schema().identity.canonicalVersionRange.empty() ? validation.familyEvidence : context_.Schema().identity.canonicalVersionRange, validation.profileId, validation.profileLabel, validation.profileVersionRange, &context_.Schema().identity});
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
            result.generationDiagnostics.push_back("SDK layout event: severity=" + event.severity +
                                                   " category=" + event.category +
                                                   " owner=" + event.owner + " property=" + event.property +
                                                   " address=" + Hex(event.propertyAddress) +
                                                   " offset=" + std::to_string(event.offset) +
                                                   " end=" + std::to_string(event.end) +
                                                   " base_size=" + std::to_string(event.baseSize) +
                                                   " cursor=" + std::to_string(event.cursor) +
                                                   " cursor_source=" + event.cursorSource +
                                                   " strategy=" + event.strategy);
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
