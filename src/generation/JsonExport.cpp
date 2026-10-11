#include "anduefker/generation/JsonExport.hpp"
#include "anduefker/ue/ContainerLayout.hpp"

#include <algorithm>
#include <iomanip>
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

        void ContainerStorage(std::ostream &stream, const ir::ContainerStorageIR &storage)
        {
            stream << "{\"allocator\":\"" << ir::ContainerAllocatorKindName(storage.allocator)
                   << "\",\"allocator_status\":\"" << EscapeJson(storage.allocatorStatus)
                   << "\",\"header_status\":\"" << EscapeJson(storage.headerStatus)
                   << "\",\"element_layout_status\":\"" << EscapeJson(storage.layoutStatus)
                   << "\",\"layout_id\":\"" << EscapeJson(storage.layoutId)
                   << "\",\"instance_traversal_validated\":false";
            if (storage.elementStride >= 0)
                stream << ",\"value_offset\":" << storage.valueOffset
                       << ",\"hash_next_offset\":" << storage.hashNextOffset
                       << ",\"hash_index_offset\":" << storage.hashIndexOffset
                       << ",\"set_element_size\":" << storage.setElementSize
                       << ",\"element_alignment\":" << storage.elementAlignment
                       << ",\"element_stride\":" << storage.elementStride;
            stream << '}';
        }

        void TypeReference(std::ostream &stream, const ir::TypeReferenceIR &reference, size_t depth, size_t &remaining)
        {
            if (depth >= 32 || remaining == 0)
            {
                stream << "{\"status\":\"traversal-limit\"}";
                return;
            }
            --remaining;
            stream << "{\"metadata_address\":\"" << Hex(reference.metadataAddress)
                   << "\",\"immediate_owner\":\"" << Hex(reference.immediateOwner)
                   << "\",\"owner_is_uobject\":" << (reference.ownerIsUObject ? "true" : "false")
                   << ",\"array_dim\":" << reference.arrayDim
                   << ",\"kind\":\"" << KindName(reference.kind) << "\",\"class\":\"" << EscapeJson(reference.reflectedClass)
                   << "\",\"element_size\":" << reference.elementSize << ",\"referenced_object\":\"" << Hex(reference.referencedObject)
                   << "\",\"secondary_object\":\"" << Hex(reference.secondaryObject) << "\",\"details_resolved\":"
                   << (reference.detailsResolved ? "true" : "false")
                   << ",\"node_details_resolved\":" << (reference.nodeDetailsResolved ? "true" : "false");
            if (reference.containerStorage)
            {
                stream << ",\"container_storage\":";
                ContainerStorage(stream, *reference.containerStorage);
            }
            if (reference.delegateStorage != ir::DelegateStorageKind::None)
                stream << ",\"delegate_storage\":{\"kind\":\"" << ir::DelegateStorageKindName(reference.delegateStorage)
                       << "\",\"bindings_expanded\":false,\"binding_location\":\""
                       << (reference.delegateStorage == ir::DelegateStorageKind::SparseMulticast    ? "external"
                           : reference.delegateStorage == ir::DelegateStorageKind::UnknownMulticast ? "unknown"
                                                                                                    : "field")
                       << "\"}";
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
                      const FieldDescriptions *fields = nullptr)
        {
            stream << "{\"address\":\"" << Hex(property.address) << "\",\"name\":\"" << EscapeJson(property.name)
                   << "\",\"class\":\"" << EscapeJson(property.reflectedClass) << "\",\"offset\":" << property.offset
                   << ",\"element_size\":" << property.elementSize << ",\"array_dim\":" << property.arrayDim
                   << ",\"storage_size\":" << static_cast<int64_t>(property.elementSize) * property.arrayDim
                   << ",\"offset_origin\":\"" << (property.isParameter ? "parameter-buffer" : "owner-start") << "\""
                   << ",\"is_parameter\":" << (property.isParameter ? "true" : "false")
                   << ",\"is_return_parameter\":" << (property.isReturnParameter ? "true" : "false")
                   << ",\"is_out_parameter\":" << (property.isOutParameter ? "true" : "false")
                   << ",\"is_reference_parameter\":" << (property.isReferenceParameter ? "true" : "false")
                   << ",\"is_const_parameter\":" << (property.isConstParameter ? "true" : "false")
                   << ",\"flags\":\"" << Hex(property.flags) << "\",\"kind\":\"" << KindName(property.type.kind)
                   << "\",\"referenced_object\":\"" << Hex(property.type.referencedObject)
                   << "\",\"secondary_object\":\"" << Hex(property.type.secondaryObject)
                   << "\",\"status\":\"" << ir::ParseStatusName(property.status) << "\",\"type_details_resolved\":"
                   << (property.typeDetailsResolved ? "true" : "false") << ",\"type\":";
            size_t remaining = 256;
            TypeReference(stream, property.type, 0, remaining);
            if (fields)
            {
                const auto found = fields->find(property.address);
                if (found != fields->end())
                {
                    const auto &field = found->second;
                    stream << ",\"generation\":{\"type_id\":" << field.typeId
                           << ",\"valid_bounds\":" << (field.validBounds ? "true" : "false")
                           << ",\"layout\":\"" << ir::LayoutRepresentationName(field.layout) << "\"}";
                }
            }
            if (property.type.kind == ir::PropertyKind::Bool)
                stream << ",\"bool_layout\":{\"field_size\":" << static_cast<unsigned int>(property.boolean.fieldSize)
                       << ",\"byte_offset\":" << static_cast<unsigned int>(property.boolean.byteOffset)
                       << ",\"byte_mask\":" << static_cast<unsigned int>(property.boolean.byteMask)
                       << ",\"field_mask\":" << static_cast<unsigned int>(property.boolean.fieldMask) << '}';
            stream << ",\"diagnostics\":";
            Strings(stream, property.diagnostics);
            stream << '}';
        }

        void Function(std::ostream &stream, const ir::FunctionIR &function, const FieldDescriptions *fields,
                      const CppSymbols *symbols)
        {
            stream << "{\"address\":\"" << Hex(function.address) << "\",\"name\":\"" << EscapeJson(function.name)
                   << "\",\"full_name\":\"" << EscapeJson(function.fullName)
                   << "\",\"class\":\"" << EscapeJson(function.reflectedClass)
                   << "\",\"outer_address\":\"" << Hex(function.outerAddress)
                   << "\",\"outer_full_name\":\"" << EscapeJson(function.outerFullName)
                   << "\",\"outer_class\":\"" << EscapeJson(function.outerClass)
                   << "\",\"object_flags\":\"" << Hex(function.objectFlags)
                   << "\",\"header_readable\":" << (function.headerReadable ? "true" : "false")
                   << ",\"discovered_from_children\":" << (function.discoveredFromChildren ? "true" : "false")
                   << ",\"referenced_as_signature\":" << (function.referencedAsSignature ? "true" : "false")
                   << ",\"native_flag\":" << (function.nativeFlag ? "true" : "false")
                   << ",\"exec_entry\":\"" << Hex(function.execEntry) << "\",\"exec_entry_rva\":";
            if (function.execEntryRva)
                stream << '"' << Hex(*function.execEntryRva) << '"';
            else
                stream << "null";
            stream << ",\"native_exec_rva\":";
            if (function.nativeExecRva)
                stream << '"' << Hex(*function.nativeExecRva) << '"';
            else
                stream << "null";
            stream << ",\"entry_kind\":\"" << function.EntryKind()
                   << "\",\"entry_observation\":{\"readable\":" << (function.entryReadable ? "true" : "false")
                   << ",\"executable\":" << (function.entryExecutable ? "true" : "false")
                   << ",\"in_module\":" << (function.entryInModule ? "true" : "false") << '}'
                   << ",\"flags\":\"" << Hex(function.flags) << "\",\"status\":\"" << ir::ParseStatusName(function.status)
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
                Property(stream, function.parameters[parameter], fields);
            }
            stream << "],\"locals\":[";
            for (size_t local = 0; local < function.locals.size(); ++local)
            {
                if (local != 0)
                    stream << ',';
                Property(stream, function.locals[local], fields);
            }
            stream << "],\"layout_analysis\":";
            LayoutAnalysis(stream, function.layout);
            if (symbols)
            {
                if (const auto found = symbols->functions.find(function.address); found != symbols->functions.end())
                    stream << ",\"generation\":{\"cpp_name\":\"" << EscapeJson(found->second) << "\"}";
            }
            stream << ",\"layout_conflicts\":";
            Strings(stream, function.layoutConflicts);
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
               << ",\"read_failure_samples_omitted\":" << (capture.readFailures > capture.readFailureSamples.size() ? capture.readFailures - capture.readFailureSamples.size() : 0)
               << ",\"read_failure_samples\":[";
        for (size_t index = 0; index < capture.readFailureSamples.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &failure = capture.readFailureSamples[index];
            stream << "{\"address\":\"" << Hex(failure.address) << "\",\"read_error\":" << failure.error
                   << ",\"requested\":" << failure.requested << ",\"transferred\":" << failure.transferred << '}';
        }
        stream << "],\"attempt_history\":[";
        for (size_t index = 0; index < capture.history.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &attempt = capture.history[index];
            stream << "{\"attempt\":" << attempt.number << ",\"elapsed_ms\":" << attempt.elapsedMs
                   << ",\"observed_ranges\":" << attempt.observedRanges << ",\"changed_ranges\":" << attempt.changedRanges
                   << ",\"unreadable_ranges\":" << attempt.unreadableRanges
                   << ",\"limit_exceeded\":" << (attempt.limitExceeded ? "true" : "false")
                   << ",\"generation_changed\":" << (attempt.generationChanged ? "true" : "false")
                   << ",\"count_address\":\"" << Hex(attempt.countAddress) << "\",\"initial_count\":" << attempt.initialCount
                   << ",\"enumerated_count\":" << attempt.enumeratedCount << ",\"final_count\":" << attempt.finalCount
                   << ",\"count_delta\":" << (attempt.finalCount < 0 ? 0 : static_cast<int64_t>(attempt.finalCount) - attempt.initialCount)
                   << ",\"remaining_slots\":" << (attempt.finalCount > attempt.enumeratedCount ? attempt.finalCount - attempt.enumeratedCount : 0)
                   << ",\"tail_rounds\":" << attempt.tailRounds
                   << ",\"additional_types\":" << attempt.additionalTypes << ",\"additional_enums\":" << attempt.additionalEnums
                   << ",\"read_failures\":" << attempt.readFailures
                   << ",\"failures\":" << attempt.failures << ",\"identity_failures\":" << attempt.identityFailures
                   << ",\"coverage_complete\":" << (attempt.coverageComplete ? "true" : "false")
                   << ",\"reason\":\"" << EscapeJson(attempt.reason) << "\",\"changes\":[";
            for (size_t sample = 0; sample < attempt.changes.size(); ++sample)
            {
                if (sample != 0)
                    stream << ',';
                const auto &change = attempt.changes[sample];
                stream << "{\"address\":\"" << Hex(change.address) << "\",\"size\":" << change.size
                       << ",\"read_error\":" << change.readError << ",\"transferred\":" << change.transferred;
                const auto bytes = [&](const char *name, const std::vector<uint8_t> &value)
                {
                    static constexpr char hex[] = "0123456789ABCDEF";
                    stream << ",\"" << name << "\":\"";
                    for (uint8_t byte : value)
                        stream << hex[byte >> 4] << hex[byte & 15];
                    stream << '"';
                };
                bytes("before_hex", change.before);
                bytes("after_hex", change.after);
                stream << '}';
            }
            stream << "],\"read_failure_samples\":[";
            for (size_t sample = 0; sample < attempt.readFailureSamples.size(); ++sample)
            {
                if (sample != 0)
                    stream << ',';
                const auto &failure = attempt.readFailureSamples[sample];
                stream << "{\"address\":\"" << Hex(failure.address) << "\",\"read_error\":" << failure.error
                       << ",\"requested\":" << failure.requested << ",\"transferred\":" << failure.transferred << '}';
            }
            stream << "],\"diagnostics\":";
            Strings(stream, attempt.diagnostics);
            stream << '}';
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
        const auto collect = [&](const ir::TypeIR *type, const ir::FunctionIR *function,
                                 const ir::PropertyIR &property, const char *scope)
        {
            unresolvedRoots += property.typeDetailsResolved ? 0u : 1u;
            for (const auto &detail : property.detailDiagnostics)
            {
                ++total;
                ++counts[detail.reason + ":" + detail.normalizedClass];
                samples.push_back({type, function, &property, &detail, scope});
            }
        };
        for (const auto &type : reflection.types)
        {
            for (const auto &property : type.properties)
                collect(&type, nullptr, property, "type-field");
        }
        for (const auto &[address, function] : reflection.functions)
        {
            (void)address;
            for (const auto &property : function.parameters)
                collect(nullptr, &function, property, "function-parameter");
            for (const auto &property : function.locals)
                collect(nullptr, &function, property, "function-local");
        }
        stream << "{\"unresolved_roots_in_ir\":" << unresolvedRoots << ",\"total\":" << total
               << ",\"records_omitted\":0"
               << ",\"counts_by_reason_and_class\":{";
        bool first = true;
        for (const auto &[reason, count] : counts)
        {
            if (!first)
                stream << ',';
            first = false;
            stream << '"' << EscapeJson(reason) << "\":" << count;
        }
        stream << "},\"records\":[";
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

    void WriteDelegateDiagnosticsJson(std::ostream &stream, const ir::ReflectionIR &reflection)
    {
        const size_t exported = static_cast<size_t>(std::count_if(
            reflection.delegateSignatures.begin(), reflection.delegateSignatures.end(),
            [](const auto &signature)
            { return signature.exported; }));
        stream << "{\"referenced\":" << reflection.delegateSignatures.size()
               << ",\"exported\":" << exported << ",\"missing_definitions\":"
               << (reflection.delegateSignatures.size() - exported) << ",\"counts_by_definition_status\":{";
        std::map<std::string, size_t> statuses;
        for (const auto &signature : reflection.delegateSignatures)
            ++statuses[signature.definitionStatus];
        bool first = true;
        for (const auto &[status, count] : statuses)
        {
            if (!first)
                stream << ',';
            first = false;
            stream << '"' << EscapeJson(status) << "\":" << count;
        }
        stream << "},\"records\":[";
        for (size_t index = 0; index < reflection.delegateSignatures.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &signature = reflection.delegateSignatures[index];
            stream << "{\"address\":\"" << Hex(signature.address)
                   << "\",\"outer\":\"" << Hex(signature.outerAddress)
                   << "\",\"exported\":" << (signature.exported ? "true" : "false")
                   << ",\"owner_exported\":" << (signature.ownerExported ? "true" : "false")
                   << ",\"found_in_children\":" << (signature.foundInChildren ? "true" : "false")
                   << ",\"children_root_readable\":" << (signature.childrenRootReadable ? "true" : "false")
                   << ",\"children_status\":" << signature.childrenStatus
                   << ",\"definition_status\":\"" << EscapeJson(signature.definitionStatus)
                   << "\",\"discovery\":\"" << EscapeJson(signature.discovery) << '"';
            if (signature.childrenRootReadable)
                stream << ",\"children_root\":\"" << Hex(signature.childrenRoot) << '"';
            if (!signature.exported)
                stream << ",\"full_name\":\"" << EscapeJson(signature.fullName)
                       << "\",\"class\":\"" << EscapeJson(signature.reflectedClass)
                       << "\",\"outer_class\":\"" << EscapeJson(signature.outerClass)
                       << "\",\"outer_full_name\":\"" << EscapeJson(signature.outerFullName)
                       << "\",\"outer_readable\":" << (signature.outerReadable ? "true" : "false");
            stream << '}';
        }
        stream << "]}";
    }

    void WriteContainerStorageJson(std::ostream &stream, const ir::ReflectionIR &reflection)
    {
        stream << "{\"metadata_coverage\":\"all-visited-nodes\",\"metadata_location\":\"property.type.container_storage (including nested nodes)\",\"instance_traversal_validated\":false,\"max_raw_observations\":512,\"max_normal_owners_per_shape\":2,\"exceptional_fields\":\"individual-within-global-budget\",\"max_raw_candidates\":8192,\"priority\":\"final-representation-gaps-first\",\"sparse_check\":\"conditional-formula-with-recorded-alignment-and-pair-extent\",\"source_layouts\":[";
        bool firstLayout = true;
        for (auto kind : {ir::PropertyKind::Array, ir::PropertyKind::Set})
            if (const auto layout = ::anduefker::ue::DescribeHeapContainer(kind, reflection.containerPointerWidth))
            {
                if (!firstLayout)
                    stream << ',';
                firstLayout = false;
                stream << "{\"id\":\"" << layout->id << "\",\"size\":" << layout->size
                       << ",\"alignment\":" << layout->alignment << ",\"offsets\":{";
                for (size_t index = 0; index < layout->offsets.size(); ++index)
                {
                    if (index != 0)
                        stream << ',';
                    stream << '"' << layout->offsets[index].first << "\":" << layout->offsets[index].second;
                }
                stream << "}}";
            }
        const auto counts = [&](const char *name, const auto &values)
        {
            stream << ",\"" << name << "\":{";
            bool firstValue = true;
            for (const auto &[key, value] : values)
            {
                if (!firstValue)
                    stream << ',';
                firstValue = false;
                stream << '"' << EscapeJson(key) << "\":" << value;
            }
            stream << '}';
        };
        stream << ']';
        counts("allocator_counts", reflection.containerAllocatorCounts);
        counts("header_status_counts", reflection.containerHeaderCounts);
        counts("element_layout_status_counts", reflection.containerElementLayoutCounts);
        stream << ",\"candidates_by_class\":{";
        bool first = true;
        for (const auto &[kind, count] : reflection.containerStorageCandidates)
        {
            if (!first)
                stream << ',';
            first = false;
            stream << '"' << EscapeJson(kind) << "\":" << count;
        }
        stream << "},\"not_observed_by_reason\":{";
        first = true;
        for (const auto &[reason, count] : reflection.containerStorageNotObserved)
        {
            if (!first)
                stream << ',';
            first = false;
            stream << '"' << EscapeJson(reason) << "\":" << count;
        }
        stream << "},\"observations\":[";
        for (size_t index = 0; index < reflection.containerStorageObservations.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const auto &observation = reflection.containerStorageObservations[index];
            std::ostringstream bytes;
            bytes << std::hex << std::setfill('0');
            for (uint8_t byte : observation.bytes)
                bytes << std::setw(2) << static_cast<unsigned int>(byte);
            stream << "{\"property\":\"" << Hex(observation.propertyAddress)
                   << "\",\"owner\":\"" << Hex(observation.ownerAddress)
                   << "\",\"owner_is_uobject\":" << (observation.ownerIsUObject ? "true" : "false")
                   << ",\"name\":\"" << EscapeJson(observation.propertyName)
                   << "\",\"class\":\"" << EscapeJson(observation.propertyClass)
                   << "\",\"member\":\"" << EscapeJson(observation.member)
                   << "\",\"basis\":\"" << EscapeJson(observation.basis)
                   << "\",\"offset\":" << observation.offset << ",\"reference_offset\":" << observation.referenceOffset
                   << ",\"property_data_end\":" << observation.propertyDataEnd << ",\"address\":\"" << Hex(observation.address)
                   << "\",\"storage_size\":" << observation.storageSize << ",\"inner_size\":" << observation.innerSize
                   << ",\"inner_class\":\"" << EscapeJson(observation.innerClass)
                   << "\",\"value_size\":" << observation.valueSize << ",\"value_class\":\"" << EscapeJson(observation.valueClass) << '"'
                   << ",\"readable\":" << (observation.readable ? "true" : "false")
                   << ",\"read_error\":" << observation.readError << ",\"requested\":" << observation.requested
                   << ",\"transferred\":" << observation.transferred
                   << ",\"status\":\"" << EscapeJson(observation.status)
                   << "\",\"sample_reason\":\"" << EscapeJson(observation.sampleReason) << "\",\"bytes\":\"" << bytes.str() << '"';
            stream << ",\"flag_candidates\":[";
            for (size_t candidate = 0; candidate < observation.flagCandidates.size(); ++candidate)
            {
                if (candidate != 0)
                    stream << ',';
                const auto &flag = observation.flagCandidates[candidate];
                stream << "{\"offset\":" << flag.offset << ",\"width\":" << static_cast<unsigned int>(flag.width)
                       << ",\"raw\":" << flag.raw << ",\"known_value\":" << (flag.knownValue ? "true" : "false")
                       << ",\"basis\":\"" << EscapeJson(flag.basis) << "\",\"width_selected\":false}";
            }
            stream << ']';
            if (observation.sparseFormulaMatches)
                stream << ",\"sparse_formula_matches\":" << (*observation.sparseFormulaMatches ? "true" : "false");
            stream << ",\"storage\":";
            ContainerStorage(stream, observation.storage);
            stream << '}';
        }
        stream << "]}";
    }

    void WriteReflectionJson(std::ostream &stream, const ir::ReflectionIR &reflection, const ReflectionIdentity &identity,
                             const FieldDescriptions *fields, const CppSymbols *symbols)
    {
        stream << "{\n\"schema_version\":9,\n\"status\":\"" << ir::ParseStatusName(reflection.status)
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
        stream << ",\n\"generation_contract\":{\"purpose\":\"inspection-and-analysis\",\"target_abi_verified\":false,\"authoritative_layout\":\"reflected-size-offset-mask\",\"layout_issues\":\"owner.layout_analysis.issues\"}";
        stream << ",\n\"generation_types\":[";
        std::map<size_t, const CppPropertyType *> generationTypes;
        if (fields)
            for (const auto &[address, field] : *fields)
            {
                (void)address;
                generationTypes.try_emplace(field.typeId, &field.type);
            }
        bool firstGenerationType = true;
        for (const auto &[id, type] : generationTypes)
        {
            if (!firstGenerationType)
                stream << ',';
            firstGenerationType = false;
            stream << "{\"id\":" << id << ",\"semantic_type\":\"" << EscapeJson(type->semanticName)
                   << "\",\"cpp_type\":\"" << EscapeJson(type->name)
                   << "\",\"storage\":\"" << PropertyStorageKindName(type->storage) << "\",\"reason\":\"" << EscapeJson(type->failureReason)
                   << "\",\"reason_path\":\"" << EscapeJson(type->failurePath)
                   << "\",\"semantics_resolved\":" << (type->semanticsResolved ? "true" : "false") << '}';
        }
        stream << "],\n\"types\":[\n";
        for (size_t index = 0; index < reflection.types.size(); ++index)
        {
            if (index != 0)
                stream << ",\n";
            const auto &type = reflection.types[index];
            stream << "{\"address\":\"" << Hex(type.address) << "\",\"kind\":\"" << (type.kind == ir::TypeKind::Class ? "Class" : "Struct")
                   << "\",\"name\":\"" << EscapeJson(type.name) << "\",\"full_name\":\"" << EscapeJson(type.fullName)
                   << "\",\"size\":" << type.size << ",\"super\":\"" << Hex(type.superAddress) << "\",\"status\":\""
                   << ir::ParseStatusName(type.status) << '"';
            stream << ",\"min_alignment\":{\"value\":" << type.minAlignment
                   << ",\"status\":\"" << EscapeJson(type.minAlignmentStatus) << "\",\"width_selected\":false";
            if (type.minAlignmentRaw)
                stream << ",\"raw_4_bytes\":" << *type.minAlignmentRaw;
            stream << '}';
            if (symbols)
            {
                const auto &declaration = symbols->types.at(type.address);
                stream << ",\"generation\":{\"cpp_name\":\"" << EscapeJson(declaration.name)
                       << "\",\"layout\":\"" << ir::LayoutRepresentationName(declaration.layout)
                       << "\",\"inherits_base\":" << (declaration.inheritsBase ? "true" : "false")
                       << ",\"declaration_dependency_blocked\":" << (declaration.declarationDependencyBlocked ? "true" : "false")
                       << '}';
            }
            stream << ",\"properties\":[";
            for (size_t item = 0; item < type.properties.size(); ++item)
            {
                if (item != 0)
                    stream << ',';
                Property(stream, type.properties[item], fields);
            }
            stream << "],\"layout_analysis\":";
            LayoutAnalysis(stream, type.layout);
            stream << ",\"layout_conflicts\":";
            Strings(stream, type.layoutConflicts);
            stream << ",\"function_addresses\":[";
            for (size_t item = 0; item < type.functionAddresses.size(); ++item)
            {
                if (item != 0)
                    stream << ',';
                stream << '"' << Hex(type.functionAddresses[item]) << '"';
            }
            stream << "]}";
        }
        stream << "\n],\n\"functions\":[\n";
        bool firstFunction = true;
        for (const auto &[address, function] : reflection.functions)
        {
            (void)address;
            if (!firstFunction)
                stream << ",\n";
            firstFunction = false;
            Function(stream, function, fields, symbols);
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
        stream << "\n],\n\"delegate_signatures\":";
        WriteDelegateDiagnosticsJson(stream, reflection);
        stream << ",\n\"container_storage_evidence\":";
        WriteContainerStorageJson(stream, reflection);
        stream << "\n}\n";
    }
} // namespace anduefker::generation
