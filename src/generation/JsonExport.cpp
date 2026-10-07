#include "anduefker/generation/JsonExport.hpp"

#include <iterator>
#include <map>
#include <ostream>
#include <sstream>

namespace anduefker::generation
{
    std::string EscapeJson(std::string_view value)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        for (unsigned char character : value)
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
                if (character < 0x20)
                {
                    result += "\\u00";
                    result += hex[character >> 4];
                    result += hex[character & 0xF];
                }
                else
                    result += static_cast<char>(character);
                break;
            }
        }
        return result;
    }

    namespace
    {
        std::string Hex(uint64_t value)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << value;
            return stream.str();
        }

        const char *KindName(ir::PropertyKind kind)
        {
            static constexpr const char *names[] = {"Unknown", "Bool", "Byte", "Int8", "Int16", "Int32", "Int64", "UInt16", "UInt32", "UInt64",
                                                    "Float", "Double", "Name", "String", "Text", "Object", "SoftObject", "WeakObject", "LazyObject", "Class", "SoftClass",
                                                    "Struct", "Enum", "Array", "Set", "Map", "Interface", "Delegate", "MulticastDelegate", "FieldPath", "Optional"};
            const auto index = static_cast<size_t>(kind);
            return index < std::size(names) ? names[index] : "Unknown";
        }

        void Strings(std::ostream &stream, const std::vector<std::string> &values)
        {
            stream << '[';
            for (size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                    stream << ',';
                stream << '"' << EscapeJson(values[index]) << '"';
            }
            stream << ']';
        }

        void TypeReference(std::ostream &stream, const ir::TypeReferenceIR &reference, size_t depth, size_t &remaining)
        {
            if (depth >= 32 || remaining == 0)
            {
                stream << "{\"status\":\"traversal-limit\"}";
                return;
            }
            --remaining;
            stream << "{\"kind\":\"" << KindName(reference.kind) << "\",\"class\":\"" << EscapeJson(reference.reflectedClass)
                   << "\",\"element_size\":" << reference.elementSize << ",\"referenced_object\":\"" << Hex(reference.referencedObject)
                   << "\",\"secondary_object\":\"" << Hex(reference.secondaryObject) << "\",\"details_resolved\":"
                   << (reference.detailsResolved ? "true" : "false");
            const auto child = [&](const char *name, const std::shared_ptr<ir::TypeReferenceIR> &value)
            {
                if (value)
                {
                    stream << ",\"" << name << "\":";
                    TypeReference(stream, *value, depth + 1, remaining);
                }
            };
            child("inner", reference.inner);
            child("key", reference.key);
            child("value", reference.value);
            stream << '}';
        }

        void LayoutIssues(std::ostream &stream, const ir::LayoutAnalysisIR *analysis,
                          uintptr_t propertyAddress)
        {
            stream << '[';
            if (analysis)
            {
                bool first = true;
                for (const auto &issue : analysis->issues)
                {
                    if (issue.propertyAddress != propertyAddress)
                        continue;
                    if (!first)
                        stream << ',';
                    first = false;
                    stream << "{\"kind\":\"" << ir::LayoutIssueKindName(issue.kind)
                           << "\",\"severity\":\"" << (issue.affectsCompleteness ? "error" : "info")
                           << "\",\"affects_completeness\":" << (issue.affectsCompleteness ? "true" : "false")
                           << ",\"conflicting_address\":\"" << Hex(issue.conflictingAddress)
                           << "\",\"message\":\"" << EscapeJson(issue.message) << "\"}";
                }
            }
            stream << ']';
        }

        void LayoutAnalysis(std::ostream &stream, const ir::LayoutAnalysisIR &analysis)
        {
            stream << "{\"analyzed\":" << (analysis.analyzed ? "true" : "false")
                   << ",\"representation\":\"" << ir::LayoutRepresentationName(analysis.representation)
                   << "\",\"type_graph_complete\":" << (analysis.typeGraphComplete ? "true" : "false")
                   << ",\"base_extent_known\":" << (analysis.baseExtentKnown ? "true" : "false")
                   << ",\"base_extent\":" << analysis.baseExtent << ",\"issues\":[";
            for (size_t index = 0; index < analysis.issues.size(); ++index)
            {
                if (index != 0)
                    stream << ',';
                const auto &issue = analysis.issues[index];
                stream << "{\"kind\":\"" << ir::LayoutIssueKindName(issue.kind)
                       << "\",\"severity\":\"" << (issue.affectsCompleteness ? "error" : "info")
                       << "\",\"property_address\":\"" << Hex(issue.propertyAddress)
                       << "\",\"conflicting_address\":\"" << Hex(issue.conflictingAddress)
                       << "\",\"affects_completeness\":" << (issue.affectsCompleteness ? "true" : "false")
                       << ",\"message\":\"" << EscapeJson(issue.message) << "\"}";
            }
            stream << "]}";
        }

        void Property(std::ostream &stream, const ir::PropertyIR &property,
                      const ir::LayoutAnalysisIR *analysis = nullptr)
        {
            stream << "{\"address\":\"" << Hex(property.address) << "\",\"name\":\"" << EscapeJson(property.name)
                   << "\",\"class\":\"" << EscapeJson(property.reflectedClass) << "\",\"offset\":" << property.offset
                   << ",\"element_size\":" << property.elementSize << ",\"array_dim\":" << property.arrayDim
                   << ",\"flags\":\"" << Hex(property.flags) << "\",\"kind\":\"" << KindName(property.type.kind)
                   << "\",\"referenced_object\":\"" << Hex(property.type.referencedObject)
                   << "\",\"secondary_object\":\"" << Hex(property.type.secondaryObject)
                   << "\",\"status\":\"" << ir::ParseStatusName(property.status) << "\",\"type_details_resolved\":"
                   << (property.typeDetailsResolved ? "true" : "false") << ",\"layout_issues\":";
            LayoutIssues(stream, analysis, property.address);
            stream << ",\"type\":";
            size_t remaining = 256;
            TypeReference(stream, property.type, 0, remaining);
            if (property.type.kind == ir::PropertyKind::Bool)
                stream << ",\"bool_layout\":{\"field_size\":" << static_cast<unsigned int>(property.boolean.fieldSize)
                       << ",\"byte_offset\":" << static_cast<unsigned int>(property.boolean.byteOffset)
                       << ",\"byte_mask\":" << static_cast<unsigned int>(property.boolean.byteMask)
                       << ",\"field_mask\":" << static_cast<unsigned int>(property.boolean.fieldMask) << '}';
            stream << ",\"diagnostics\":";
            Strings(stream, property.diagnostics);
            stream << '}';
        }
    } // namespace

    void WriteSchemaIdentityJson(std::ostream &stream, const ::anduefker::ue::SchemaIdentity &identity)
    {
        stream << "{\"layout_profile_id\":\"" << EscapeJson(identity.layoutProfileId)
               << "\",\"layout_profile_label\":\"" << EscapeJson(identity.layoutProfileLabel)
               << "\",\"layout_version_range\":\"" << EscapeJson(identity.layoutVersionRange)
               << "\",\"canonical_profile_id\":\"" << EscapeJson(identity.canonicalProfileId)
               << "\",\"canonical_profile_label\":\"" << EscapeJson(identity.canonicalProfileLabel)
               << "\",\"canonical_version_range\":\"" << EscapeJson(identity.canonicalVersionRange)
               << "\",\"selection_reason\":\"" << EscapeJson(identity.selectionReason)
               << "\",\"version_confidence\":\"" << EscapeJson(identity.versionConfidence)
               << "\",\"layout_score\":" << identity.layoutScore
               << ",\"version_evidence_score\":" << identity.versionEvidenceScore
               << ",\"compatible_profiles\":[";
        for (size_t index = 0; index < identity.compatibleProfiles.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << "\"" << EscapeJson(identity.compatibleProfiles[index]) << "\"";
        }
        stream << "],\"evidence\":[";
        for (size_t index = 0; index < identity.evidence.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &evidence = identity.evidence[index];
            stream << "{\"kind\":\"" << EscapeJson(evidence.kind)
                   << "\",\"observed\":\"" << EscapeJson(evidence.observed)
                   << "\",\"strength\":\"" << ::anduefker::ue::VersionEvidenceStrengthName(evidence.strength)
                   << "\",\"detail\":\"" << EscapeJson(evidence.detail) << "\"}";
        }
        stream << "]}";
    }

    void WriteStatsJson(std::ostream &stream, const ir::ReflectionStats &stats, std::optional<size_t> opaqueFields)
    {
        stream << "{\"object_slots\":" << stats.objectSlots << ",\"valid_objects\":" << stats.validObjects
               << ",\"parsed_types\":" << stats.parsedTypes << ",\"parsed_enums\":" << stats.parsedEnums
               << ",\"parsed_functions\":" << stats.parsedFunctions << ",\"parsed_properties\":" << stats.parsedProperties
               << ",\"unknown_properties\":" << stats.unknownProperties << ",\"unresolved_type_details\":" << stats.unresolvedTypeDetails
               << ",\"layout_conflicts\":" << stats.layoutConflicts << ",\"skipped_objects\":" << stats.skippedObjects
               << ",\"empty_object_slots\":" << stats.emptyObjectSlots << ",\"object_read_failures\":" << stats.objectReadFailures
               << ",\"class_name_read_failures\":" << stats.classNameReadFailures
               << ",\"skipped_class_default_objects\":" << stats.skippedClassDefaultObjects
               << ",\"skipped_incomplete_objects\":" << stats.skippedIncompleteObjects
               << ",\"object_diagnostic_samples_omitted\":" << stats.objectDiagnosticSamplesOmitted
               << ",\"enum_read_failures\":" << stats.enumReadFailures << ",\"identity_failures\":" << stats.identityFailures
               << ",\"unvisited_objects\":" << stats.unvisitedObjects
               << ",\"failures\":" << stats.failures;
        if (opaqueFields)
            stream << ",\"opaque_fields\":" << *opaqueFields;
        stream << '}';
    }

    void WriteCaptureJson(std::ostream &stream, const ir::CaptureInfo &capture)
    {
        stream << "{\"mode\":\"" << (capture.attempts == 0 ? "not-checked" : "observed-bytes")
               << "\",\"atomic_snapshot\":false,\"observations_stable\":" << (capture.observationsStable ? "true" : "false")
               << ",\"limit_exceeded\":" << (capture.limitExceeded ? "true" : "false")
               << ",\"generation_changed\":" << (capture.generationChanged ? "true" : "false")
               << ",\"observed_ranges\":" << capture.observedRanges << ",\"observed_bytes\":" << capture.observedBytes
               << ",\"changed_ranges\":" << capture.changedRanges << ",\"unreadable_ranges\":" << capture.unreadableRanges
               << ",\"attempts\":" << capture.attempts
               << ",\"read_failures\":" << capture.readFailures
               << ",\"read_failure_samples_omitted\":" << (capture.readFailures - capture.readFailureSamples.size())
               << ",\"read_failure_samples\":[";
        for (size_t index = 0; index < capture.readFailureSamples.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &failure = capture.readFailureSamples[index];
            stream << "{\"address\":\"" << Hex(failure.address) << "\",\"read_error\":" << failure.error
                   << ",\"requested\":" << failure.requested << ",\"transferred\":" << failure.transferred << '}';
        }
        stream << "]}";
    }

    void WritePropertyDiagnosticsJson(std::ostream &stream, const ir::ReflectionIR &reflection)
    {
        struct Sample
        {
            const ir::TypeIR *type;
            const ir::FunctionIR *function;
            const ir::PropertyIR *property;
            const ir::PropertyDetailDiagnostic *detail;
            const char *scope;
        };
        std::map<std::string, size_t> counts;
        std::vector<Sample> samples;
        size_t total = 0;
        size_t unresolvedRoots = 0;
        const auto collect = [&](const ir::TypeIR &type, const ir::FunctionIR *function,
                                 const ir::PropertyIR &property, const char *scope)
        {
            unresolvedRoots += property.typeDetailsResolved ? 0u : 1u;
            for (const auto &detail : property.detailDiagnostics)
            {
                ++total;
                if (++counts[detail.reason + ":" + detail.normalizedClass] <= 8)
                    samples.push_back({&type, function, &property, &detail, scope});
            }
        };
        for (const auto &type : reflection.types)
        {
            for (const auto &property : type.properties)
                collect(type, nullptr, property, "type-field");
            for (const auto &function : type.functions)
            {
                for (const auto &property : function.parameters)
                    collect(type, &function, property, "function-parameter");
                for (const auto &property : function.locals)
                    collect(type, &function, property, "function-local");
            }
        }
        stream << "{\"unresolved_roots_in_ir\":" << unresolvedRoots << ",\"total\":" << total
               << ",\"sample_limit_per_reason_and_class\":8,\"samples_omitted\":" << (total - samples.size())
               << ",\"counts_by_reason_and_class\":{";
        bool first = true;
        for (const auto &[reason, count] : counts)
        {
            if (!first)
                stream << ',';
            first = false;
            stream << '"' << EscapeJson(reason) << "\":" << count;
        }
        stream << "},\"samples\":[";
        for (size_t index = 0; index < samples.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const Sample &sample = samples[index];
            const auto &detail = *sample.detail;
            stream << "{\"owner_full_name\":\"" << EscapeJson(sample.function ? sample.function->fullName : sample.type->fullName)
                   << "\",\"owner_address\":\"" << Hex(sample.function ? sample.function->address : sample.type->address)
                   << "\",\"scope\":\"" << sample.scope << "\",\"root_property\":\"" << EscapeJson(sample.property->name)
                   << "\",\"root_address\":\"" << Hex(sample.property->address)
                   << "\",\"property_name\":\"" << EscapeJson(detail.name)
                   << "\",\"property_address\":\"" << Hex(detail.address)
                   << "\",\"property_class_address\":\"" << Hex(detail.classAddress)
                   << "\",\"property_next_address\":\"" << Hex(detail.nextAddress)
                   << "\",\"header_available\":" << (detail.headerAvailable ? "true" : "false")
                   << ",\"immediate_owner\":\"" << Hex(detail.immediateOwner)
                   << "\",\"owner_is_uobject\":" << (detail.ownerIsUObject ? "true" : "false")
                   << ",\"property_class\":\"" << EscapeJson(detail.reflectedClass)
                   << "\",\"normalized_class\":\"" << EscapeJson(detail.normalizedClass)
                   << "\",\"reason\":\"" << EscapeJson(detail.reason)
                   << "\",\"details_status\":\"" << EscapeJson(detail.detailsStatus)
                   << "\",\"offset\":" << detail.offset << ",\"element_size\":" << detail.elementSize
                   << ",\"array_dim\":" << detail.arrayDim << ",\"flags\":\"" << Hex(detail.flags)
                   << "\",\"referenced_address\":\"" << Hex(detail.referencedAddress)
                   << "\",\"secondary_address\":\"" << Hex(detail.secondaryAddress)
                   << "\",\"referenced_class\":\"" << EscapeJson(detail.referencedClass)
                   << "\",\"secondary_class\":\"" << EscapeJson(detail.secondaryClass)
                   << "\",\"object_property_class_null\":" << (detail.objectPropertyClassNull ? "true" : "false")
                   << ",\"object_property_class_pointer_address\":\"" << Hex(detail.objectPropertyClassPointerAddress) << "\""
                   << ",\"bool_layout\":{\"field_size\":" << static_cast<unsigned int>(detail.boolean.fieldSize)
                   << ",\"byte_offset\":" << static_cast<unsigned int>(detail.boolean.byteOffset)
                   << ",\"byte_mask\":" << static_cast<unsigned int>(detail.boolean.byteMask)
                   << ",\"field_mask\":" << static_cast<unsigned int>(detail.boolean.fieldMask) << "},\"reads\":[";
            for (size_t readIndex = 0; readIndex < detail.reads.size(); ++readIndex)
            {
                if (readIndex != 0)
                    stream << ',';
                const auto &read = detail.reads[readIndex];
                stream << "{\"member\":\"" << EscapeJson(read.member) << "\",\"selected_offset\":" << read.offset
                       << ",\"address\":\"" << Hex(read.address) << "\",\"raw_value\":\"" << Hex(read.rawValue)
                       << "\",\"read_error\":" << read.error << ",\"requested\":" << read.requested
                       << ",\"transferred\":" << read.transferred << ",\"status\":\"" << EscapeJson(read.status) << "\"}";
            }
            stream << "],\"messages\":";
            Strings(stream, sample.property->diagnostics);
            stream << '}';
        }
        stream << "]}";
    }

    void WriteReflectionJson(std::ostream &stream, const ir::ReflectionIR &reflection, const ReflectionIdentity &identity)
    {
        stream << "{\n\"schema_version\":3,\n\"status\":\"" << ir::ParseStatusName(reflection.status)
               << "\",\n\"engine\":\"" << EscapeJson(identity.engine) << "\",\n\"profile\":{\"id\":\"" << EscapeJson(identity.profileId)
               << "\",\"label\":\"" << EscapeJson(identity.profileLabel) << "\",\"version_range\":\"" << EscapeJson(identity.versionRange)
               << "\"}";
        if (identity.schemaIdentity)
        {
            stream << ",\n\"schema_identity\":";
            WriteSchemaIdentityJson(stream, *identity.schemaIdentity);
        }
        stream << ",\n\"stats\":";
        WriteStatsJson(stream, reflection.stats);
        stream << ",\n\"capture\":";
        WriteCaptureJson(stream, reflection.capture);
        stream << ",\n\"diagnostics\":";
        Strings(stream, reflection.diagnostics);
        stream << ",\n\"types\":[\n";
        for (size_t index = 0; index < reflection.types.size(); ++index)
        {
            if (index != 0)
                stream << ",\n";
            const auto &type = reflection.types[index];
            stream << "{\"address\":\"" << Hex(type.address) << "\",\"kind\":\"" << (type.kind == ir::TypeKind::Class ? "Class" : "Struct")
                   << "\",\"name\":\"" << EscapeJson(type.name) << "\",\"full_name\":\"" << EscapeJson(type.fullName)
                   << "\",\"size\":" << type.size << ",\"super\":\"" << Hex(type.superAddress) << "\",\"status\":\""
                   << ir::ParseStatusName(type.status) << "\",\"properties\":[";
            for (size_t item = 0; item < type.properties.size(); ++item)
            {
                if (item != 0)
                    stream << ',';
                Property(stream, type.properties[item], &type.layout);
            }
            stream << "],\"layout_analysis\":";
            LayoutAnalysis(stream, type.layout);
            stream << ",\"layout_conflicts\":";
            Strings(stream, type.layoutConflicts);
            stream << ",\"functions\":[";
            for (size_t item = 0; item < type.functions.size(); ++item)
            {
                if (item != 0)
                    stream << ',';
                const auto &function = type.functions[item];
                stream << "{\"address\":\"" << Hex(function.address) << "\",\"name\":\"" << EscapeJson(function.name)
                       << "\",\"full_name\":\"" << EscapeJson(function.fullName) << "\",\"native_rva\":\"" << Hex(function.nativeRva)
                       << "\",\"native_address\":\"" << Hex(function.nativeAddress)
                       << "\",\"flags\":\"" << Hex(function.flags) << "\",\"status\":\"" << ir::ParseStatusName(function.status)
                       << "\",\"num_params\":" << static_cast<unsigned int>(function.numParams) << ",\"param_size\":" << function.paramSize
                       << ",\"return_value_offset\":" << function.returnValueOffset << ",\"header_num_params\":"
                       << static_cast<unsigned int>(function.headerNumParams) << ",\"header_param_size\":" << function.headerParamSize
                       << ",\"derived_num_params\":" << function.derivedNumParams << ",\"derived_param_size\":" << function.derivedParamSize
                       << ",\"default_initializer_count\":" << function.defaultInitializerCount << ",\"parameter_semantics_valid\":"
                       << (function.parameterSemanticsValid ? "true" : "false") << ",\"parameter_semantics_consistent\":"
                       << (function.parameterSemanticsConsistent ? "true" : "false") << ",\"parameters\":[";
                for (size_t parameter = 0; parameter < function.parameters.size(); ++parameter)
                {
                    if (parameter != 0)
                        stream << ',';
                    Property(stream, function.parameters[parameter], &function.layout);
                }
                stream << "],\"locals\":[";
                for (size_t local = 0; local < function.locals.size(); ++local)
                {
                    if (local != 0)
                        stream << ',';
                    Property(stream, function.locals[local], &function.layout);
                }
                stream << "],\"layout_analysis\":";
                LayoutAnalysis(stream, function.layout);
                stream << ",\"layout_conflicts\":";
                Strings(stream, function.layoutConflicts);
                stream << '}';
            }
            stream << "]}";
        }
        stream << "\n],\n\"enums\":[\n";
        for (size_t index = 0; index < reflection.enums.size(); ++index)
        {
            if (index != 0)
                stream << ",\n";
            const auto &enumeration = reflection.enums[index];
            stream << "{\"address\":\"" << Hex(enumeration.address) << "\",\"name\":\"" << EscapeJson(enumeration.name)
                   << "\",\"full_name\":\"" << EscapeJson(enumeration.fullName) << "\",\"cpp_form\":"
                   << static_cast<unsigned int>(enumeration.cppForm) << ",\"flags\":" << static_cast<unsigned int>(enumeration.flags)
                   << ",\"underlying_type\":" << static_cast<int>(enumeration.underlyingType) << ",\"status\":\""
                   << ir::ParseStatusName(enumeration.status) << "\",\"expected_values\":" << enumeration.expectedValues << ",\"diagnostics\":";
            Strings(stream, enumeration.diagnostics);
            stream << ",\"values\":[";
            for (size_t value = 0; value < enumeration.values.size(); ++value)
            {
                if (value != 0)
                    stream << ',';
                stream << "{\"name\":\"" << EscapeJson(enumeration.values[value].name) << "\",\"value\":" << enumeration.values[value].value << '}';
            }
            stream << "]}";
        }
        stream << "\n]\n}\n";
    }
} // namespace anduefker::generation
