#include "anduefker/reflection/ReflectionReader.hpp"
#include "anduefker/ir/ReflectionLayout.hpp"
#include "anduefker/ue/BoolLayout.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace anduefker::reflection
{
    namespace
    {
        constexpr size_t kMaxFunctionDefinitions = 2 * 1024 * 1024;

        std::optional<uintptr_t> Add(uintptr_t base, int32_t offset)
        {
            if (offset < 0 || base > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return base + static_cast<uintptr_t>(offset);
        }

    } // namespace

    ReflectionReader::ReflectionReader(IMemorySource &memory,
                                       const RuntimeBinding &binding,
                                       const EngineSchema &schema,
                                       uintptr_t moduleBase,
                                       uintptr_t moduleEnd,
                                       std::function<void(const std::string &)> progress,
                                       std::function<void(const std::string &)> diagnostic)
        : memory_(memory, diagnostic),
          schema_(schema),
          moduleBase_(moduleBase),
          moduleEnd_(moduleEnd),
          objects_(memory_, binding, schema),
          progress_(std::move(progress)),
          diagnostic_(std::move(diagnostic))
    {
    }

    PropertyKind ReflectionReader::PropertyKindFromName(const std::string &name) const
    {
        const std::string normalized = NormalizeRuntimeFieldName(name);
        if (normalized == "BoolProperty")
            return PropertyKind::Bool;
        if (normalized == "ByteProperty")
            return PropertyKind::Byte;
        if (normalized == "Int8Property")
            return PropertyKind::Int8;
        if (normalized == "Int16Property")
            return PropertyKind::Int16;
        if (normalized == "IntProperty" || normalized == "Int32Property")
            return PropertyKind::Int32;
        if (normalized == "Int64Property")
            return PropertyKind::Int64;
        if (normalized == "UInt16Property")
            return PropertyKind::UInt16;
        if (normalized == "UInt32Property")
            return PropertyKind::UInt32;
        if (normalized == "UInt64Property")
            return PropertyKind::UInt64;
        if (normalized == "FloatProperty")
            return PropertyKind::Float;
        if (normalized == "DoubleProperty")
            return PropertyKind::Double;
        if (normalized == "NameProperty")
            return PropertyKind::Name;
        if (normalized == "StrProperty")
            return PropertyKind::String;
        if (normalized == "TextProperty")
            return PropertyKind::Text;
        if (normalized == "ObjectProperty" || normalized == "ObjectPtrProperty" || normalized == "ObjectPropertyBase")
            return PropertyKind::Object;
        if (normalized == "SoftObjectProperty")
            return PropertyKind::SoftObject;
        if (normalized == "WeakObjectProperty")
            return PropertyKind::WeakObject;
        if (normalized == "LazyObjectProperty")
            return PropertyKind::LazyObject;
        if (normalized == "ClassProperty" || normalized == "ClassPtrProperty")
            return PropertyKind::Class;
        if (normalized == "SoftClassProperty")
            return PropertyKind::SoftClass;
        if (normalized == "StructProperty")
            return PropertyKind::Struct;
        if (normalized == "EnumProperty")
            return PropertyKind::Enum;
        if (normalized == "ArrayProperty")
            return PropertyKind::Array;
        if (normalized == "SetProperty")
            return PropertyKind::Set;
        if (normalized == "MapProperty")
            return PropertyKind::Map;
        if (normalized == "InterfaceProperty")
            return PropertyKind::Interface;
        if (normalized == "DelegateProperty")
            return PropertyKind::Delegate;
        if (normalized == "MulticastDelegateProperty" || normalized == "MulticastInlineDelegateProperty" ||
            normalized == "MulticastSparseDelegateProperty")
            return PropertyKind::MulticastDelegate;
        if (normalized == "FieldPathProperty")
            return PropertyKind::FieldPath;
        if (normalized == "OptionalProperty")
            return PropertyKind::Optional;
        if (normalized == "Utf8StrProperty" || normalized == "AnsiStrProperty")
            return PropertyKind::String;
        return PropertyKind::Unknown;
    }

    void ReflectionReader::RecordPropertyDetail(const PropertyMetadata &metadata, PropertyIR &property,
                                                const std::string &reason, bool headerAvailable) const
    {
        ::anduefker::ir::PropertyDetailDiagnostic diagnostic;
        diagnostic.headerAvailable = headerAvailable;
        diagnostic.address = metadata.address;
        diagnostic.classAddress = metadata.classAddress;
        diagnostic.nextAddress = metadata.nextAddress;
        diagnostic.immediateOwner = metadata.ownerAddress;
        diagnostic.ownerIsUObject = metadata.ownerIsUObject;
        diagnostic.name = metadata.name;
        diagnostic.reflectedClass = metadata.className;
        diagnostic.normalizedClass = metadata.normalizedClassName;
        diagnostic.reason = reason;
        diagnostic.detailsStatus = ::anduefker::ue::PropertyDetailsStatusName(metadata.detailsStatus);
        diagnostic.offset = metadata.offset;
        diagnostic.elementSize = metadata.elementSize;
        diagnostic.arrayDim = metadata.arrayDim;
        diagnostic.flags = metadata.flags;
        diagnostic.referencedAddress = metadata.referencedAddress;
        diagnostic.secondaryAddress = metadata.secondaryAddress;
        diagnostic.boolean = {metadata.boolLayout[0], metadata.boolLayout[1], metadata.boolLayout[2], metadata.boolLayout[3]};
        for (size_t index = 0; index < metadata.detailReadCount; ++index)
        {
            const auto &read = metadata.detailReads[index];
            diagnostic.reads.push_back({read.member, read.offset, read.address, read.rawValue,
                                        static_cast<int32_t>(read.read.error), read.read.requested, read.read.transferred,
                                        ::anduefker::ue::PropertyDetailsStatusName(read.status)});
        }
        diagnostic.referencedClass = "not-observed";
        diagnostic.secondaryClass = "not-observed";
        diagnostic.objectPropertyClassNull = metadata.objectPropertyClassNull;
        diagnostic.objectPropertyClassPointerAddress = metadata.objectPropertyClassPointerAddress;
        // 仅在已完成读取与基本指针检查后补充类型身份；不尝试解码未知对象句柄。
        const auto referenceClass = [&](uintptr_t address, bool secondary) -> std::string
        {
            if (address == 0 || metadata.detailsStatus != PropertyMetadata::DetailsStatus::Complete)
                return "not-observed";
            const auto kind = PropertyKindFromName(metadata.className);
            if (kind == PropertyKind::FieldPath)
            {
                const auto cls = objects_.FieldClass(address);
                return cls ? cls->name : "<invalid-field-class>";
            }
            if (kind == PropertyKind::Map || (!secondary && (kind == PropertyKind::Array || kind == PropertyKind::Set ||
                                                             kind == PropertyKind::Enum || kind == PropertyKind::Optional)))
            {
                const auto field = objects_.Field(address);
                return field ? field->className : "<invalid-field>";
            }
            const auto cls = objects_.ClassName(address);
            return cls ? *cls : "<unreadable>";
        };
        diagnostic.referencedClass = referenceClass(metadata.referencedAddress, false);
        diagnostic.secondaryClass = referenceClass(metadata.secondaryAddress, true);
        property.detailDiagnostics.push_back(std::move(diagnostic));
    }

    TypeReferenceIR ReflectionReader::ReadTypeReference(const PropertyMetadata &metadata, PropertyIR &property,
                                                        ReflectionStats &stats, std::unordered_set<uintptr_t> &path,
                                                        size_t depth, size_t &remaining) const
    {
        TypeReferenceIR result;
        result.metadataAddress = metadata.address;
        result.immediateOwner = metadata.ownerAddress;
        result.ownerIsUObject = metadata.ownerIsUObject;
        result.arrayDim = metadata.arrayDim;
        result.kind = PropertyKindFromName(metadata.className);
        result.reflectedClass = metadata.className;
        result.elementSize = metadata.elementSize;
        result.referencedObject = metadata.referencedAddress;
        result.secondaryObject = metadata.secondaryAddress;
        if (depth >= 32 || remaining == 0 || !path.insert(metadata.address).second)
        {
            property.diagnostics.push_back("nested property cycle or traversal limit: address=" + std::to_string(metadata.address));
            RecordPropertyDetail(metadata, property, "cycle-or-traversal-limit");
            ++stats.failures;
            return result;
        }
        --remaining;
        result.detailsResolved = metadata.detailsStatus == PropertyMetadata::DetailsStatus::Complete &&
                                 result.kind != PropertyKind::Unknown && metadata.elementSize > 0;
        if (metadata.detailsStatus != PropertyMetadata::DetailsStatus::Complete)
        {
            // 成员读取失败或必需引用为空时，仅记录证据，不能再以其结果做对象/子属性解引用。
            property.diagnostics.push_back("property details status=" + std::to_string(static_cast<int>(metadata.detailsStatus)) +
                                           " address=" + std::to_string(metadata.address));
            if (metadata.detailsStatus != PropertyMetadata::DetailsStatus::UnsupportedLayout)
                ++stats.failures;
            if (result.kind == PropertyKind::Unknown)
                ++stats.unknownProperties;
            const std::string detailReason = metadata.objectPropertyClassNull
                                                 ? "object-property-class-metadata-null"
                                                 : ::anduefker::ue::PropertyDetailsStatusName(metadata.detailsStatus);
            RecordPropertyDetail(metadata, property, detailReason);
            path.erase(metadata.address);
            return result;
        }
        if (result.kind == PropertyKind::Unknown)
            ++stats.unknownProperties;
        const auto nested = [&](uintptr_t address) -> std::shared_ptr<TypeReferenceIR>
        {
            if (address == 0)
                return {};
            const auto child = objects_.Property(address);
            if (!child)
            {
                ++stats.failures;
                property.diagnostics.push_back("nested property header unreadable: address=" + std::to_string(address));
                PropertyMetadata missing;
                missing.address = address;
                missing.name = "<unreadable>";
                missing.className = "<unreadable>";
                missing.normalizedClassName = "<unreadable>";
                missing.detailsStatus = PropertyMetadata::DetailsStatus::Unreadable;
                RecordPropertyDetail(missing, property, "nested-header-unreadable", false);
                return {};
            }
            if (schema_.features.useFProperty && (child->ownerIsUObject || child->ownerAddress != metadata.address))
            {
                ++stats.failures;
                property.diagnostics.push_back("nested property owner mismatch: address=" + std::to_string(address));
                RecordPropertyDetail(*child, property, "nested-owner-mismatch");
                return {};
            }
            return std::make_shared<TypeReferenceIR>(ReadTypeReference(*child, property, stats, path, depth + 1, remaining));
        };
        const auto objectMatches = [&](uintptr_t address, ::anduefker::ue::DefinitionKind expected)
        {
            if (address == 0)
                return false;
            const auto cls = objects_.Class(address);
            const auto kind = cls ? objects_.DefinitionKindForClass(*cls) : std::nullopt;
            return kind && *kind == expected;
        };
        bool semanticMatch = true;
        bool enclosingContainer = false;
        switch (result.kind)
        {
        case PropertyKind::Object:
        case PropertyKind::Class:
        case PropertyKind::SoftObject:
        case PropertyKind::SoftClass:
        case PropertyKind::WeakObject:
        case PropertyKind::LazyObject:
        case PropertyKind::Interface:
            semanticMatch = objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Class);
            break;
        case PropertyKind::Struct:
            semanticMatch = objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Struct);
            break;
        case PropertyKind::Byte:
            semanticMatch = result.referencedObject == 0 || objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Enum);
            break;
        case PropertyKind::Array:
        case PropertyKind::Set:
        case PropertyKind::Optional:
            result.inner = nested(result.referencedObject);
            enclosingContainer = true;
            semanticMatch = result.inner && result.inner->detailsResolved;
            break;
        case PropertyKind::Map:
            result.key = nested(result.referencedObject);
            result.value = nested(result.secondaryObject);
            enclosingContainer = true;
            semanticMatch = result.key && result.value && result.key->detailsResolved && result.value->detailsResolved;
            break;
        case PropertyKind::Enum:
            result.inner = nested(result.referencedObject);
            semanticMatch = result.inner && result.inner->detailsResolved && result.inner->elementSize == result.elementSize &&
                            objectMatches(result.secondaryObject, ::anduefker::ue::DefinitionKind::Enum);
            break;
        case PropertyKind::Delegate:
        case PropertyKind::MulticastDelegate:
        {
            const auto cls = objects_.ClassName(result.referencedObject);
            semanticMatch = cls && IsFunctionFieldKind(::anduefker::ue::FieldKindFromRuntimeName(*cls, false));
            if (semanticMatch && !delegateSignatures_.contains(result.referencedObject))
            {
                if (pendingSignatures_.size() >= kMaxFunctionDefinitions)
                {
                    semanticMatch = false;
                    ++stats.failures;
                    property.diagnostics.push_back("delegate signature worklist budget exhausted: signature=" +
                                                   std::to_string(result.referencedObject));
                    break;
                }
                ::anduefker::ir::DelegateSignatureObservation observation;
                observation.address = result.referencedObject;
                observation.reflectedClass = *cls;
                observation.fullName = objects_.FullName(observation.address).value_or("<unreadable>");
                const auto outer = objects_.Outer(observation.address);
                observation.outerReadable = outer.has_value();
                observation.outerAddress = outer.value_or(0);
                observation.outerClass = outer && *outer != 0 ? objects_.ClassName(*outer).value_or("<unreadable>") : "<none>";
                observation.outerFullName = outer && *outer != 0 ? objects_.FullName(*outer).value_or("<unreadable>") : "<none>";
                delegateSignatures_.emplace(observation.address, std::move(observation));
                pendingSignatures_.push_back(result.referencedObject);
            }
            break;
        }
        case PropertyKind::Bool:
            semanticMatch = ::anduefker::ue::IsValidBoolLayout(metadata.boolLayout, metadata.elementSize);
            break;
        case PropertyKind::FieldPath:
            semanticMatch = metadata.detailsStatus == PropertyMetadata::DetailsStatus::Complete &&
                            objects_.IsValidFieldClass(result.referencedObject);
            break;
        default:
            break;
        }
        result.nodeDetailsResolved = result.detailsResolved && (enclosingContainer
                                                                    ? (result.kind == PropertyKind::Map ? result.key && result.value : result.inner != nullptr)
                                                                    : semanticMatch);
        if (!semanticMatch)
        {
            property.diagnostics.push_back("property type semantics unresolved: address=" + std::to_string(metadata.address));
            result.detailsResolved = false;
        }
        if (!result.detailsResolved)
        {
            std::string reason = "semantic-mismatch";
            if (result.kind == PropertyKind::Unknown)
                reason = "unknown-property-kind";
            else if (metadata.elementSize <= 0)
                reason = "invalid-element-size";
            else if (metadata.objectPropertyClassNull)
                reason = "object-property-class-metadata-null";
            RecordPropertyDetail(metadata, property, reason);
        }
        // 经过验证的子项标头/Owner 足以构成一个原始的候选窗口
        // 不受支持的子项语义绝不能掩盖外围容器的证据
        if (metadata.elementSize > 0 &&
            (((result.kind == PropertyKind::Array || result.kind == PropertyKind::Set) && result.inner) ||
             (result.kind == PropertyKind::Map && result.key && result.value)))
        {
            if (observedContainers_.insert(metadata.address).second)
            {
                ++containerCandidates_[metadata.normalizedClassName];
                if (pendingContainers_.size() < 8192)
                    pendingContainers_.push_back({metadata, result});
                else
                    ++containerNotObserved_[metadata.normalizedClassName + ":candidate-queue-budget"];
            }
        }
        path.erase(metadata.address);
        return result;
    }

    std::optional<PropertyIR> ReflectionReader::ReadProperty(uintptr_t field, ReflectionStats &stats) const
    {
        const auto metadata = objects_.Property(field);
        if (!metadata)
            return std::nullopt;
        PropertyIR result;
        result.address = field;
        result.name = metadata->name;
        result.reflectedClass = metadata->className;
        result.offset = metadata->offset;
        result.elementSize = metadata->elementSize;
        result.arrayDim = metadata->arrayDim;
        result.flags = metadata->flags;
        result.isParameter = (result.flags & 0x80u) != 0;
        result.isReturnParameter = (result.flags & 0x400u) != 0;
        result.isOutParameter = (result.flags & 0x100u) != 0;
        result.isReferenceParameter = (result.flags & 0x08000000u) != 0;
        result.isConstParameter = (result.flags & 0x2u) != 0;
        result.boolean = {metadata->boolLayout[0], metadata->boolLayout[1], metadata->boolLayout[2], metadata->boolLayout[3]};
        std::unordered_set<uintptr_t> path;
        size_t remaining = 256;
        result.type = ReadTypeReference(*metadata, result, stats, path, 0, remaining);
        result.typeDetailsResolved = result.type.detailsResolved;
        if (!result.typeDetailsResolved)
        {
            ++stats.unresolvedTypeDetails;
            result.status = ParseStatus::Partial;
        }
        ++stats.parsedProperties;
        return result;
    }

    void ReflectionReader::ReadProperties(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        PropertyChain chain = ReadPropertyChain(first, type.address, stats, type.layoutConflicts);
        type.properties = std::move(chain.properties);
        if (!chain.complete ||
            std::any_of(type.properties.begin(), type.properties.end(), [](const PropertyIR &property)
                        { return property.status != ParseStatus::Complete; }))
            type.status = ParseStatus::Partial;
    }

    ReflectionReader::PropertyChain ReflectionReader::ReadPropertyChain(uintptr_t first, uintptr_t owner,
                                                                        ReflectionStats &stats,
                                                                        std::vector<std::string> &diagnostics) const
    {
        PropertyChain result;
        const FieldChainResult chain = objects_.FieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            result.complete = false;
            diagnostics.push_back("property field chain status=" + std::to_string(static_cast<int>(chain.status)) +
                                  " owner=" + std::to_string(owner) + " root=" + std::to_string(first));
        }
        for (const FieldMetadata &field : chain.fields)
        {
            if (memory_.LimitExceeded())
            {
                result.complete = false;
                ++stats.failures;
                diagnostics.push_back("capture observation limit reached in property chain");
                break;
            }
            if (!IsPropertyFieldKind(field.kind))
            {
                continue;
            }
            if (schema_.features.useFProperty &&
                (!field.ownerIsUObject || field.ownerAddress != owner))
            {
                ++stats.failures;
                result.complete = false;
                diagnostics.push_back("property owner mismatch: property=" + field.name +
                                      " address=" + std::to_string(field.address) +
                                      " expected_owner=" + std::to_string(owner) +
                                      " actual_owner=" + std::to_string(field.ownerAddress));
                continue;
            }
            const auto property = ReadProperty(field.address, stats);
            if (property)
                result.properties.push_back(*property);
            else
            {
                ++stats.failures;
                result.complete = false;
                diagnostics.push_back("property header unreadable: property=" + field.name +
                                      " class=" + field.className + " address=" + std::to_string(field.address));
            }
        }
        return result;
    }

    void ReflectionReader::ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const
    {
        std::vector<PropertyMetadata> properties;
        const PropertyChain chain = ReadPropertyChain(first, function.address, stats, function.layoutConflicts);
        const std::vector<PropertyIR> &parsedProperties = chain.properties;
        for (const PropertyIR &property : parsedProperties)
        {
            PropertyMetadata metadata;
            metadata.arrayDim = property.arrayDim;
            metadata.elementSize = property.elementSize;
            metadata.offset = property.offset;
            metadata.flags = property.flags;
            properties.push_back(std::move(metadata));
        }

        const FunctionParameterSummary summary = AnalyzeFunctionParameters(
            properties, function.flags, schema_.features.functionDefaultsContinueAfterInitializer);
        function.parameters.clear();
        function.locals.clear();
        const size_t scannedPropertyCount = std::min(summary.scannedPropertyCount, parsedProperties.size());
        for (size_t index = 0; index < scannedPropertyCount; ++index)
        {
            if ((parsedProperties[index].flags & ::anduefker::ue::kCPFParm) != 0)
                function.parameters.push_back(parsedProperties[index]);
        }
        for (const PropertyIR &property : parsedProperties)
            if ((property.flags & ::anduefker::ue::kCPFParm) == 0)
                function.locals.push_back(property);
        function.derivedNumParams = summary.count;
        function.derivedParamSize = summary.paramEnd;
        function.defaultInitializerCount = summary.defaultInitializerCount;
        function.parameterSemanticsValid = summary.valid && chain.complete;
        const uint16_t expectedReturnOffset = summary.returnOffset >= 0
                                                  ? static_cast<uint16_t>(summary.returnOffset)
                                                  : std::numeric_limits<uint16_t>::max();
        function.parameterSemanticsConsistent = function.parameterSemanticsValid &&
                                                function.headerNumParams == summary.count &&
                                                function.headerParamSize == summary.paramEnd &&
                                                function.returnValueOffset == expectedReturnOffset;
        if (!function.parameterSemanticsConsistent)
        {
            function.layoutConflicts.push_back("function header parameters do not match derived semantics: header_count=" +
                                               std::to_string(function.headerNumParams) +
                                               " derived_count=" + std::to_string(summary.count) +
                                               " header_size=" + std::to_string(function.headerParamSize) +
                                               " derived_size=" + std::to_string(summary.paramEnd) +
                                               " header_return=" + std::to_string(function.returnValueOffset) +
                                               " derived_return=" + std::to_string(expectedReturnOffset));
            ++stats.layoutConflicts;
        }
        if (!function.layoutConflicts.empty() ||
            std::any_of(function.parameters.begin(), function.parameters.end(), [](const PropertyIR &property)
                        { return property.status != ParseStatus::Complete; }))
            function.status = ParseStatus::Partial;
    }

    FunctionIR *ReflectionReader::ReadFunction(uintptr_t address, ReflectionIR &ir) const
    {
        if (const auto found = ir.functions.find(address); found != ir.functions.end())
            return &found->second;
        if (address == 0 || ir.functions.size() >= kMaxFunctionDefinitions)
        {
            ++ir.stats.failures;
            ir.diagnostics.push_back("function registry budget or invalid address: address=" + std::to_string(address));
            return nullptr;
        }
        FunctionIR &function = ir.functions.try_emplace(address).first->second;
        function.address = address;
        const auto fail = [&](const std::string &reason)
        {
            ++ir.stats.failures;
            function.status = ParseStatus::Failed;
            function.layoutConflicts.push_back(reason);
            return &function;
        };
        if (memory_.LimitExceeded())
            return fail("capture observation limit reached before function read");

        const auto field = objects_.UField(address);
        if (field)
        {
            function.name = field->name;
            function.reflectedClass = field->className;
            function.fullName = field->className + " " + field->name;
        }
        if (!field || !IsFunctionFieldKind(field->kind))
            return fail("function field header unreadable or not a UFunction");
        const auto classAddress = objects_.Class(address);
        const auto outerAddress = objects_.Outer(address);
        const auto internalIndex = objects_.InternalIndex(address);
        const auto objectFlags = objects_.Flags(address);
        if (!classAddress || !outerAddress || !internalIndex || !objectFlags)
            return fail("function object header unreadable: class_readable=" + std::to_string(classAddress.has_value()) +
                        " outer_readable=" + std::to_string(outerAddress.has_value()) +
                        " index_readable=" + std::to_string(internalIndex.has_value()) +
                        " flags_readable=" + std::to_string(objectFlags.has_value()));
        function.outerAddress = *outerAddress;
        function.objectFlags = *objectFlags;
        const auto fullName = objects_.FullName(address);
        if (fullName)
            function.fullName = *fullName;
        else
        {
            ++ir.stats.failures;
            function.status = ParseStatus::Partial;
            function.layoutConflicts.push_back("function full name could not be resolved; using field identity");
        }
        if (*outerAddress != 0)
        {
            function.outerClass = objects_.ClassName(*outerAddress).value_or("<unreadable>");
            function.outerFullName = objects_.FullName(*outerAddress).value_or("<unreadable>");
        }
        if ((*objectFlags & (::anduefker::ue::kRFUnavailableDefinition | ::anduefker::ue::kRFClassDefaultObject)) != 0)
            return fail("function definition unavailable: object_flags=" + std::to_string(*objectFlags));
        const auto slot = objects_.Objects().ReadObject(*internalIndex, true);
        const auto internalFlags = slot.IsValid() ? objects_.Objects().ReadInternalFlags(slot) : std::nullopt;
        if (!slot.IsValid() || slot.address != address || !internalFlags || (*internalFlags & 0x80000000u) != 0 ||
            objects_.InternalIndex(address, true) != internalIndex ||
            objects_.Class(address, true) != classAddress)
        {
            ++ir.stats.identityFailures;
            memory_.Invalidate();
            return fail("function object identity changed or pending construction: index=" + std::to_string(*internalIndex));
        }
        const auto readMember = [&](int32_t offset, auto &value)
        {
            const auto member = Add(address, offset);
            return member && memory_.Read(*member, value);
        };
        if (!readMember(schema_.ufunction.functionFlags, function.flags) ||
            !readMember(schema_.ufunction.numParams, function.headerNumParams) ||
            !readMember(schema_.ufunction.paramSize, function.headerParamSize) ||
            !readMember(schema_.ufunction.returnValueOffset, function.returnValueOffset))
            return fail("function header unreadable: flags_offset=" + std::to_string(schema_.ufunction.functionFlags) +
                        " count_offset=" + std::to_string(schema_.ufunction.numParams) +
                        " size_offset=" + std::to_string(schema_.ufunction.paramSize) +
                        " return_offset=" + std::to_string(schema_.ufunction.returnValueOffset));

        function.headerReadable = true;
        function.numParams = function.headerNumParams;
        function.paramSize = function.headerParamSize;
        function.nativeFlag = (function.flags & ::anduefker::ue::kFUNCNative) != 0;
        function.entryReadable = readMember(schema_.ufunction.nativeFunction, function.execEntry);
        function.entryInModule = function.entryReadable && function.execEntry >= moduleBase_ && function.execEntry < moduleEnd_;
        function.entryExecutable = function.entryReadable && function.execEntry != 0 &&
                                   memory_.IsExecutable(function.execEntry, sizeof(uintptr_t));
        if (function.entryInModule)
            function.execEntryRva = function.execEntry - moduleBase_;
        if (function.nativeFlag && function.entryInModule && function.entryExecutable)
            function.nativeExecRva = function.execEntryRva;
        if (!function.entryReadable || (function.nativeFlag && !function.entryExecutable))
        {
            ++ir.stats.failures;
            function.status = ParseStatus::Partial;
            function.layoutConflicts.push_back("function exec entry unreadable or invalid: offset=" +
                                               std::to_string(schema_.ufunction.nativeFunction) +
                                               " entry=" + std::to_string(function.execEntry) +
                                               " native_flag=" + std::to_string(function.nativeFlag));
        }
        const auto parameters = objects_.StructProperties(address);
        if (parameters)
            ReadFunctionParameters(*parameters, function, ir.stats);
        else
        {
            ++ir.stats.failures;
            function.status = ParseStatus::Partial;
            function.layoutConflicts.push_back("function parameter chain root unreadable: offset=" +
                                               std::to_string(schema_.features.useFProperty ? schema_.ustruct.childProperties : schema_.ustruct.children));
        }
        ++ir.stats.parsedFunctions;
        return &function;
    }

    void ReflectionReader::ReadFunctions(uintptr_t first, TypeIR &type, ReflectionIR &ir) const
    {
        const FieldChainResult chain = objects_.UFieldsWithStatus(first, 65536);
        functionChains_[type.address] = {first, chain.status};
        if (!chain.Complete())
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("function field chain status=" + std::to_string(static_cast<int>(chain.status)) +
                                           " root=" + std::to_string(first));
        }
        for (const FieldMetadata &field : chain.fields)
        {
            if (!IsFunctionFieldKind(field.kind))
                continue;
            FunctionIR *function = ReadFunction(field.address, ir);
            if (!function)
            {
                type.status = ParseStatus::Partial;
                break;
            }
            function->discoveredFromChildren = true;
            type.functionAddresses.push_back(field.address);
            if (function->status != ParseStatus::Complete)
                type.status = ParseStatus::Partial;
            if (function->outerAddress != type.address)
            {
                ++ir.stats.failures;
                type.status = ParseStatus::Partial;
                type.layoutConflicts.push_back("function Children owner mismatch: address=" + std::to_string(field.address) +
                                               " actual_outer=" + std::to_string(function->outerAddress));
            }
        }
    }

    void ReflectionReader::CloseDelegateSignatures(ReflectionIR &ir) const
    {
        // 读取一个签名可以在其参数中发现更多签名
        // 应当处理并清空一个有界的待办工作列表，而不是进行递归的函数读取或依赖映射顺序的扫描
        while (nextSignature_ < pendingSignatures_.size())
        {
            FunctionIR *function = ReadFunction(pendingSignatures_[nextSignature_++], ir);
            if (function)
                function->referencedAsSignature = true;
            if (memory_.LimitExceeded())
                break;
        }
    }

    void ReflectionReader::CollectContainerStorage() const
    {
        const auto limitedRepresentation = [](const auto &self, const TypeReferenceIR &reference, size_t depth) -> bool
        {
            if (depth >= 32 || !reference.detailsResolved)
                return true;
            switch (reference.kind)
            {
            case PropertyKind::Text:
            case PropertyKind::WeakObject:
            case PropertyKind::LazyObject:
            case PropertyKind::SoftObject:
            case PropertyKind::SoftClass:
            case PropertyKind::Delegate:
            case PropertyKind::MulticastDelegate:
            case PropertyKind::FieldPath:
            case PropertyKind::Optional:
                return true;
            default:
                return (reference.inner && self(self, *reference.inner, depth + 1)) ||
                       (reference.key && self(self, *reference.key, depth + 1)) ||
                       (reference.value && self(self, *reference.value, depth + 1));
            }
        };
        const auto priority = [&](const ContainerCandidate &candidate)
        {
            return !candidate.reference.detailsResolved ? 0 : limitedRepresentation(limitedRepresentation, candidate.reference, 0) ? 1
                                                                                                                                   : 2;
        };
        for (auto &candidate : pendingContainers_)
            candidate.priority = priority(candidate);
        // 结构表征缺口优先享有 budget，常规容器依然需要分配器证据
        std::stable_sort(pendingContainers_.begin(), pendingContainers_.end(), [](const auto &left, const auto &right)
                         { return left.priority < right.priority; });
        for (const auto &candidate : pendingContainers_)
            ObserveContainerStorage(candidate.metadata, candidate.reference);
        pendingContainers_.clear();
    }

    void ReflectionReader::ObserveContainerStorage(const PropertyMetadata &metadata, const TypeReferenceIR &reference) const
    {
        if (containerObservations_.size() >= 512)
        {
            ++containerNotObserved_[metadata.normalizedClassName + ":global-observation-budget"];
            return;
        }
        const auto *element = reference.kind == PropertyKind::Map ? reference.key.get() : reference.inner.get();
        const auto *value = reference.value.get();
        const std::string shape = metadata.normalizedClassName + ":" + std::to_string(metadata.elementSize) +
                                  ":" + (element ? element->reflectedClass : "none") +
                                  ":" + std::to_string(element ? element->elementSize : 0) + ":" + std::to_string(element ? element->arrayDim : 0) +
                                  ":" + (value ? value->reflectedClass : "none") +
                                  ":" + std::to_string(value ? value->elementSize : 0) + ":" + std::to_string(value ? value->arrayDim : 0) +
                                  ":" + std::to_string(reference.detailsResolved);
        auto &owners = containerSampleOwners_[shape];
        // 这些是发现阶段的观测结果，而非选定的 ABI 字段
        // 必须对形状多样性与 Owner 样本的数量进行边界限制，同时必须报告每一个被省略的观测项
        if (owners.size() >= 2 || owners.contains(metadata.ownerAddress))
        {
            ++containerNotObserved_[metadata.normalizedClassName + ":sample-budget-or-repeated-owner"];
            return;
        }
        owners.insert(metadata.ownerAddress);
        ::anduefker::ir::ContainerStorageObservation observation;
        observation.propertyAddress = metadata.address;
        observation.ownerAddress = metadata.ownerAddress;
        observation.ownerIsUObject = metadata.ownerIsUObject;
        observation.propertyName = metadata.name;
        observation.propertyClass = metadata.normalizedClassName;
        observation.innerClass = element ? element->reflectedClass : "";
        observation.valueClass = value ? value->reflectedClass : "";
        observation.storageSize = metadata.elementSize;
        observation.propertyDataEnd = schema_.property.subtypeStart;
        observation.referenceOffset = reference.kind == PropertyKind::Array ? schema_.propertySubtypes.arrayInner : reference.kind == PropertyKind::Map ? schema_.propertySubtypes.mapBase
                                                                                                                                                        : schema_.propertySubtypes.setElement;
        const auto childExtent = [](const TypeReferenceIR *child)
        {
            const int64_t size = child ? static_cast<int64_t>(child->elementSize) * child->arrayDim : 0;
            return child && child->elementSize > 0 && child->arrayDim > 0 && size <= INT32_MAX ? static_cast<int32_t>(size) : 0;
        };
        observation.innerSize = childExtent(element);
        observation.valueSize = childExtent(value);
        int32_t referenceOffset = -1;
        size_t referenceCount = 1;
        size_t windowSize = 0;
        if (reference.kind == PropertyKind::Array)
        {
            referenceOffset = schema_.propertySubtypes.arrayInner;
            observation.member = "ArrayFlags-candidates";
            // 4.25-5.2 ：先 Inner，后 int flags；5.3+ ：先字节 flags，后 Inner
            // 通过 Inner+4 读取经过验证的属性数据末尾，而不是盲目地使用 Inner-8
            // 派生成员可能会复用基类的尾部填充，此处不选择任何候选方案
            const int64_t end = static_cast<int64_t>(referenceOffset) + static_cast<int64_t>(sizeof(uintptr_t)) + 4;
            if (schema_.property.subtypeStart >= 0 && schema_.property.subtypeStart <= referenceOffset &&
                end - schema_.property.subtypeStart <= 32)
            {
                referenceOffset = schema_.property.subtypeStart;
                referenceCount = 0;
                windowSize = static_cast<size_t>(end - referenceOffset);
                observation.basis = "validated-property-data-end-through-inner+4; flags-before-inner:uint8 or after-inner:int32; candidate-only";
            }
            else
            {
                windowSize = 4;
                observation.basis = "after-inner:int32-candidate-only; before-inner-candidate-unavailable:unresolved-property-data-end";
            }
            if (!schema_.features.useFProperty)
                observation.status = "not-applicable-to-uproperty";
        }
        else if (reference.kind == PropertyKind::Map)
        {
            referenceOffset = schema_.propertySubtypes.mapBase;
            referenceCount = 2;
            observation.member = "MapLayout-and-possible-MapFlags";
            observation.basis = "after-validated-key-value; sparse=24 compact=12; flag-width=1-or-4; candidate-only";
            windowSize = schema_.features.useFProperty ? 28 : 24;
        }
        else
        {
            referenceOffset = schema_.propertySubtypes.setElement;
            observation.member = "SetLayout";
            observation.basis = "after-validated-element; sparse=20 compact=8; candidate-only";
            windowSize = 20;
        }
        const size_t delta = referenceCount * sizeof(uintptr_t);
        observation.requested = windowSize;
        if (observation.status.empty() && referenceOffset >= 0 &&
            static_cast<size_t>(referenceOffset) <= static_cast<size_t>(INT32_MAX) - delta)
        {
            observation.offset = referenceOffset + static_cast<int32_t>(delta);
            const auto address = Add(metadata.address, observation.offset);
            observation.address = address.value_or(0);
            if (!address || memory_.LimitExceeded() || !memory_.IsReadable(*address, windowSize))
                observation.status = "candidate-window-unreadable-or-budget-exhausted";
            else
            {
                observation.bytes.resize(windowSize);
                const auto read = memory_.ReadBytes(*address, observation.bytes.data(), windowSize);
                observation.readError = static_cast<int32_t>(read.error);
                observation.transferred = read.transferred;
                observation.readable = read.Ok();
                observation.status = read.Ok() ? "observed-not-selected" : "candidate-read-failed";
                if (!read.Ok())
                    observation.bytes.clear();
            }
        }
        else if (observation.status.empty())
            observation.status = "candidate-offset-unrepresentable";
        if (observation.readable && schema_.features.useFProperty)
        {
            const auto flagCandidate = [&](int32_t offset, uint8_t width, const char *basis)
            {
                if (offset < observation.offset)
                    return;
                const size_t index = static_cast<size_t>(offset - observation.offset);
                if (index > observation.bytes.size() || width > observation.bytes.size() - index)
                    return;
                uint32_t raw = 0;
                std::memcpy(&raw, observation.bytes.data() + index, width);
                observation.flagCandidates.push_back({basis, offset, width, raw, raw <= 1});
            };
            if (reference.kind == PropertyKind::Array)
            {
                if (schema_.property.subtypeStart >= observation.offset &&
                    schema_.property.subtypeStart < observation.referenceOffset)
                    flagCandidate(schema_.property.subtypeStart, 1, "UE5.3+-flags-before-inner-property-data-end");
                if (observation.referenceOffset <= INT32_MAX - static_cast<int32_t>(sizeof(uintptr_t)))
                    flagCandidate(observation.referenceOffset + static_cast<int32_t>(sizeof(uintptr_t)), 4,
                                  "UE4.25-5.2-flags-after-inner");
            }
            else if (reference.kind == PropertyKind::Map)
            {
                // UE 中同时存在这两种布局族；不能仅凭大小来选择其中任何一个
                for (const auto &[size, basis] : {std::pair{12, "compact-map-layout-tail"}, std::pair{24, "sparse-map-layout-tail"}})
                    if (observation.offset <= INT32_MAX - size)
                    {
                        flagCandidate(observation.offset + size, 1, basis);
                        flagCandidate(observation.offset + size, 4, basis);
                    }
            }
        }
        if (observation.readable && reference.kind != PropertyKind::Array)
        {
            const auto word = [&](size_t index)
            {
                int32_t result = 0;
                std::memcpy(&result, observation.bytes.data() + index * sizeof(result), sizeof(result));
                return static_cast<int64_t>(result);
            };
            const size_t start = reference.kind == PropertyKind::Map ? 1 : 0;
            const int64_t valueOffset = start == 1 ? word(0) : 0;
            const int64_t payloadEnd = start == 1 ? valueOffset + observation.valueSize : observation.innerSize;
            const bool pairFits = start == 0 || valueOffset >= observation.innerSize;
            const auto alignmentValid = [](int64_t alignment)
            {
                return alignment > 0 && (alignment & (alignment - 1)) == 0;
            };
            observation.sparseShapeConsistent =
                observation.innerSize > 0 && (start == 0 || observation.valueSize > 0) && pairFits &&
                word(start) >= payloadEnd && word(start) % 4 == 0 && word(start + 1) == word(start) + 4 &&
                word(start + 2) >= word(start + 1) + 4 && alignmentValid(word(start + 3)) && word(start + 3) >= 4 &&
                word(start + 4) >= word(start + 2) && word(start + 4) >= 8;
            observation.compactShapeConsistent =
                observation.innerSize > 0 && (start == 0 || observation.valueSize > 0) && pairFits &&
                word(start) >= payloadEnd && alignmentValid(word(start + 1));
        }
        containerObservations_.push_back(std::move(observation));
    }

    std::optional<TypeIR> ReflectionReader::ReadType(uintptr_t object, TypeKind kind, ReflectionIR &ir) const
    {
        const auto name = objects_.Name(object);
        const auto className = objects_.ClassName(object);
        if (!name || !className)
        {
            ir.diagnostics.push_back("type identity unreadable: address=" + std::to_string(object) +
                                     " name_readable=" + std::to_string(name.has_value()) +
                                     " class_readable=" + std::to_string(className.has_value()));
            return std::nullopt;
        }
        const auto size = objects_.StructSize(object);
        const auto super = objects_.StructSuper(object);
        if (!size || *size < 0 || !super)
        {
            ir.diagnostics.push_back("type header invalid or unreadable: name=" + *name +
                                     " address=" + std::to_string(object) +
                                     " size_offset=" + std::to_string(schema_.ustruct.propertiesSizeOffset) +
                                     " size=" + (size ? std::to_string(*size) : "unreadable") +
                                     " super_offset=" + std::to_string(schema_.ustruct.superStruct) +
                                     " super_readable=" + std::to_string(super.has_value()));
            return std::nullopt;
        }

        TypeIR type;
        type.address = object;
        type.superAddress = *super;
        type.kind = kind;
        type.name = *name;
        const auto fullName = objects_.FullName(object);
        type.fullName = fullName ? *fullName : (*className + " " + *name);
        if (!fullName)
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("type full name could not be resolved");
        }
        type.size = *size;
        const auto properties = objects_.StructProperties(object);
        if (properties)
            ReadProperties(*properties, type, ir.stats);
        else
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("property chain root unreadable");
        }
        const auto children = objects_.StructChildren(object);
        if (children)
            ReadFunctions(*children, type, ir);
        else
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("function chain root unreadable");
        }
        if (!type.layoutConflicts.empty())
            type.status = ParseStatus::Partial;
        return type;
    }

    EnumIR ReflectionReader::ReadEnum(uintptr_t object, ReflectionStats &stats) const
    {
        EnumIR result;
        result.address = object;
        const auto name = objects_.Name(object);
        const auto fullName = objects_.FullName(object);
        result.name = name ? *name : "<unreadable>";
        result.fullName = fullName ? *fullName : "Enum " + result.name;
        const auto failure = [&](const std::string &message)
        {
            result.status = ParseStatus::Partial;
            result.diagnostics.push_back(message);
            ++stats.failures;
            ++stats.enumReadFailures;
        };
        if (!name || !fullName)
            failure("enum identity could not be resolved");
        const auto readTail = [&](int32_t offset, bool byte, uint8_t &out)
        {
            const auto address = Add(object, offset);
            if (!address)
                return false;
            if (byte)
                return memory_.Read(*address, out);
            uint32_t value = 0;
            if (!memory_.Read(*address, value) || value > UINT8_MAX)
                return false;
            out = static_cast<uint8_t>(value);
            return true;
        };
        if (schema_.uenum.cppForm < 0 || !readTail(schema_.uenum.cppForm, schema_.features.enumCppFormIsByte, result.cppForm) || result.cppForm > 2)
            failure("enum CppForm is unreadable or invalid");
        if (schema_.features.enumHasFlags &&
            (schema_.uenum.flags < 0 || !readTail(schema_.uenum.flags, schema_.features.enumFlagsIsByte, result.flags)))
            failure("enum flags are unreadable or invalid");
        const auto values = objects_.ReadEnumValues(object);
        result.expectedValues = values.expectedCount;
        for (const EnumValueMetadata &value : values.values)
            result.values.push_back({value.name, value.value});
        if (!values.Complete())
            failure("enum values incomplete: status=" + std::to_string(static_cast<int>(values.status)) +
                    " expected=" + std::to_string(values.expectedCount) + " read=" + std::to_string(values.values.size()));
        return result;
    }

    ReflectionIR ReflectionReader::Read()
    {
        ReflectionIR result;
        std::vector<::anduefker::ir::CaptureInfo::Attempt> history;
        for (uint32_t attempt = 1; attempt <= 2; ++attempt)
        {
            const auto started = std::chrono::steady_clock::now();
            if (progress_)
                progress_("capture attempt=" + std::to_string(attempt) + " started");
            if (attempt > 1 && !memory_.RefreshAddressSpace())
            {
                result.status = result.types.empty() ? ParseStatus::Failed : ParseStatus::Partial;
                result.capture.observationsStable = false;
                result.diagnostics.push_back("address-space refresh failed before capture retry");
                break;
            }
            memory_.Reset();
            ::anduefker::memory::CaptureValidation validation;
            ::anduefker::ir::CaptureInfo::Attempt details;
            result = ReadAttempt(validation, details);
            std::unordered_set<uintptr_t> exportedTypes;
            for (const auto &type : result.types)
                exportedTypes.insert(type.address);
            for (auto &[address, observation] : delegateSignatures_)
            {
                const auto definition = result.functions.find(address);
                observation.exported = definition != result.functions.end() && definition->second.headerReadable;
                observation.ownerExported = exportedTypes.contains(observation.outerAddress);
                observation.foundInChildren = definition != result.functions.end() && definition->second.discoveredFromChildren;
                observation.definitionStatus = definition == result.functions.end() ? "not-read" : ParseStatusName(definition->second.status);
                observation.discovery = observation.foundInChildren ? "children+property-reference" : "property-reference";
                if (const auto chain = functionChains_.find(observation.outerAddress); chain != functionChains_.end())
                {
                    observation.childrenRootReadable = true;
                    observation.childrenRoot = chain->second.first;
                    observation.childrenStatus = static_cast<int32_t>(chain->second.second);
                }
                result.delegateSignatures.push_back(observation);
            }
            result.containerStorageObservations = std::move(containerObservations_);
            result.containerStorageCandidates = containerCandidates_;
            result.containerStorageNotObserved = containerNotObserved_;
            if (diagnostic_)
            {
                diagnostic_("reflection_evidence attempt=" + std::to_string(attempt) + " begin");
                LogEvidence(result);
                for (const auto &message : result.diagnostics)
                    diagnostic_("reflection: " + message);
                diagnostic_("reflection_evidence attempt=" + std::to_string(attempt) + " end");
            }
            memory_.CopyReadFailures(validation);
            details.number = attempt;
            details.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - started)
                                                          .count());
            details.observedRanges = validation.observedRanges;
            details.changedRanges = validation.changedRanges;
            details.unreadableRanges = validation.unreadableRanges;
            details.limitExceeded = validation.limitExceeded;
            details.generationChanged = validation.generationChanged;
            details.readFailures = validation.readFailures;
            details.failures = result.stats.failures;
            details.identityFailures = result.stats.identityFailures;
            const size_t diagnosticSamples = std::min<size_t>(result.diagnostics.size(), 32);
            details.diagnostics.assign(result.diagnostics.begin(), result.diagnostics.begin() + diagnosticSamples);
            for (const auto &failure : validation.readFailureSamples)
                details.readFailureSamples.push_back({failure.address, static_cast<int32_t>(failure.error),
                                                      failure.requested, failure.transferred});
            for (const auto &change : validation.changes)
                details.changes.push_back({change.address, change.size, static_cast<int32_t>(change.read.error),
                                           change.read.transferred, change.before, change.after});
            if (details.reason.empty())
                details.reason = validation.Stable() ? "observed-bytes-stable" : "observed-bytes-changed-or-unreadable";
            history.push_back(std::move(details));
            if (progress_)
                progress_("capture attempt=" + std::to_string(attempt) + " elapsed_ms=" +
                          std::to_string(history.back().elapsedMs) + " reason=" + history.back().reason);
            result.capture.observationsStable = validation.Stable() && validation.observedRanges != 0 && history.back().coverageComplete;
            result.capture.limitExceeded = validation.limitExceeded;
            result.capture.generationChanged = validation.generationChanged;
            result.capture.observedRanges = validation.observedRanges;
            result.capture.observedBytes = validation.observedBytes;
            result.capture.changedRanges = validation.changedRanges;
            result.capture.unreadableRanges = validation.unreadableRanges;
            result.capture.attempts = attempt;
            result.capture.readFailures = validation.readFailures;
            for (const auto &failure : validation.readFailureSamples)
                result.capture.readFailureSamples.push_back({failure.address, static_cast<int32_t>(failure.error),
                                                             failure.requested, failure.transferred});
            if (!result.capture.observationsStable)
            {
                result.status = result.types.empty() ? ParseStatus::Failed : ParseStatus::Partial;
                result.diagnostics.push_back("capture validation: changed=" + std::to_string(validation.changedRanges) +
                                             " unreadable=" + std::to_string(validation.unreadableRanges) +
                                             " limit_exceeded=" + std::to_string(validation.limitExceeded) +
                                             " generation_changed=" + std::to_string(validation.generationChanged));
                for (uintptr_t address : validation.failedAddresses)
                    result.diagnostics.push_back("capture validation failed at address=" + std::to_string(address));
                if (!validation.limitExceeded && attempt == 1)
                    continue;
            }
            break;
        }
        result.capture.history = std::move(history);
        result.diagnostics.push_back("capture attempts=" + std::to_string(result.capture.attempts) +
                                     "; consistency covers observed bytes, not an atomic process snapshot");
        for (const auto &failure : result.capture.readFailureSamples)
            result.diagnostics.push_back("capture read failure: address=" + std::to_string(failure.address) +
                                         " requested=" + std::to_string(failure.requested) +
                                         " transferred=" + std::to_string(failure.transferred) +
                                         " read_error=" + std::to_string(failure.error));
        if (result.capture.readFailures > result.capture.readFailureSamples.size())
            result.diagnostics.push_back("capture read failure samples omitted=" +
                                         std::to_string(result.capture.readFailures - result.capture.readFailureSamples.size()));
        return result;
    }

    void ReflectionReader::LogEvidence(const ReflectionIR &reflection) const
    {
        if (!diagnostic_)
            return;
        std::unordered_set<uintptr_t> owners;
        const auto logOwner = [&](uintptr_t address, const std::string &name)
        {
            if (owners.insert(address).second)
            {
                std::ostringstream line;
                line << "reflection_owner address=0x" << std::hex << address << " full_name=" << std::quoted(name);
                diagnostic_(line.str());
            }
        };
        std::map<std::string, size_t> entryKinds;
        using EntryKey = std::tuple<uintptr_t, bool, bool, bool>;
        std::map<EntryKey, size_t> entryIds;
        std::map<EntryKey, size_t> entryCounts;
        for (const auto &[address, function] : reflection.functions)
        {
            (void)address;
            logOwner(function.outerAddress, function.outerFullName);
            ++entryKinds[function.EntryKind()];
            const EntryKey key{function.execEntry, function.entryReadable, function.entryExecutable, function.entryInModule};
            ++entryCounts[key];
        }
        diagnostic_("function_entry_summary definitions=" + std::to_string(reflection.functions.size()) +
                    " parsed=" + std::to_string(reflection.stats.parsedFunctions) +
                    " rva_semantics=reflected-exec-entry-not-business-implementation");
        for (const auto &[kind, count] : entryKinds)
            diagnostic_("function_entry_kind kind=" + kind + " count=" + std::to_string(count));
        for (const auto &[key, count] : entryCounts)
        {
            const auto &[entry, readable, executable, inModule] = key;
            if (count == 1)
                continue;
            const size_t id = entryIds.size();
            entryIds.emplace(key, id);
            std::ostringstream line;
            line << "exec_entry_context id=" << id << " entry=0x" << std::hex << entry << std::dec
                 << " readable=" << readable << " executable=" << executable << " in_module=" << inModule
                 << " functions=" << count;
            if (inModule)
                line << " rva=0x" << std::hex << (entry - moduleBase_);
            diagnostic_(line.str());
        }
        diagnostic_("delegate_signature_summary total=" + std::to_string(reflection.delegateSignatures.size()) +
                    " exported=" + std::to_string(std::count_if(reflection.delegateSignatures.begin(), reflection.delegateSignatures.end(), [](const auto &item)
                                                                { return item.exported; })));
        std::map<std::string, size_t> definitionStatuses;
        for (const auto &signature : reflection.delegateSignatures)
            ++definitionStatuses[signature.definitionStatus];
        for (const auto &[status, count] : definitionStatuses)
            diagnostic_("delegate_signature_definition_status status=" + status + " count=" + std::to_string(count));
        for (const auto &signature : reflection.delegateSignatures)
        {
            std::ostringstream line;
            line << "delegate_signature address=0x" << std::hex << signature.address << " outer=0x" << signature.outerAddress
                 << " children_root=0x" << signature.childrenRoot << std::dec
                 << " exported=" << signature.exported << " owner_exported=" << signature.ownerExported
                 << " found_in_children=" << signature.foundInChildren << " children_root_readable=" << signature.childrenRootReadable
                 << " children_status=" << signature.childrenStatus << " definition_status=" << signature.definitionStatus
                 << " discovery=" << signature.discovery;
            if (!signature.exported)
                line << " full_name=" << std::quoted(signature.fullName) << " class=" << signature.reflectedClass
                     << " outer_class=" << signature.outerClass << " outer_full_name=" << std::quoted(signature.outerFullName)
                     << " outer_readable=" << signature.outerReadable;
            diagnostic_(line.str());
        }
        const auto propertyEvidence = [&](const PropertyIR &property, const std::string &owner, uintptr_t ownerAddress, const char *scope)
        {
            for (const auto &message : property.diagnostics)
            {
                std::ostringstream line;
                line << "property_diagnostic owner=" << std::quoted(owner) << " owner_address=" << ownerAddress
                     << " scope=" << scope << " property=" << std::quoted(property.name)
                     << " address=" << property.address << " message=" << std::quoted(message);
                diagnostic_(line.str());
            }
            for (const auto &detail : property.detailDiagnostics)
            {
                std::ostringstream line;
                line << "property_detail owner=" << std::quoted(owner) << " scope=" << scope
                     << " root=" << std::quoted(property.name) << " root_address=" << property.address
                     << " node=" << std::quoted(detail.name) << " address=" << detail.address
                     << " class=" << std::quoted(detail.reflectedClass) << " class_address=" << detail.classAddress
                     << " next=" << detail.nextAddress << " immediate_owner=" << detail.immediateOwner
                     << " owner_is_uobject=" << detail.ownerIsUObject << " header_available=" << detail.headerAvailable
                     << " reason=" << detail.reason << " status=" << detail.detailsStatus
                     << " offset=" << detail.offset << " element_size=" << detail.elementSize << " array_dim=" << detail.arrayDim
                     << " flags=" << detail.flags << " reference=" << detail.referencedAddress << " secondary=" << detail.secondaryAddress
                     << " reference_class=" << std::quoted(detail.referencedClass) << " secondary_class=" << std::quoted(detail.secondaryClass)
                     << " object_class_null=" << detail.objectPropertyClassNull
                     << " object_class_pointer_address=" << detail.objectPropertyClassPointerAddress
                     << " bool_field_size=" << static_cast<unsigned int>(detail.boolean.fieldSize)
                     << " bool_byte_offset=" << static_cast<unsigned int>(detail.boolean.byteOffset)
                     << " bool_byte_mask=" << static_cast<unsigned int>(detail.boolean.byteMask)
                     << " bool_field_mask=" << static_cast<unsigned int>(detail.boolean.fieldMask);
                diagnostic_(line.str());
                for (const auto &read : detail.reads)
                    diagnostic_("property_detail_read node=" + std::to_string(detail.address) +
                                " member=" + read.member + " selected_offset=" + std::to_string(read.offset) +
                                " address=" + std::to_string(read.address) + " raw=" + std::to_string(read.rawValue) +
                                " error=" + std::to_string(read.error) + " requested=" + std::to_string(read.requested) +
                                " transferred=" + std::to_string(read.transferred) + " status=" + read.status);
            }
            size_t remaining = 256;
            const auto references = [&](const auto &self, const TypeReferenceIR &reference, const std::string &path, size_t depth) -> void
            {
                if (depth >= 32 || remaining == 0)
                {
                    diagnostic_("delegate_reference traversal_limit=1 root=" + std::to_string(property.address) + " path=" + path);
                    return;
                }
                --remaining;
                if (reference.kind == PropertyKind::Delegate || reference.kind == PropertyKind::MulticastDelegate)
                {
                    logOwner(ownerAddress, owner);
                    std::ostringstream line;
                    line << "delegate_reference owner=0x" << std::hex << ownerAddress << std::dec << " scope=" << scope
                         << " root=" << std::quoted(property.name) << " root_address=" << property.address
                         << " path=" << path << " node=" << reference.metadataAddress
                         << " class=" << reference.reflectedClass << " signature=" << reference.referencedObject
                         << " details_resolved=" << reference.detailsResolved;
                    diagnostic_(line.str());
                }
                if (reference.inner)
                    self(self, *reference.inner, path + ".inner", depth + 1);
                if (reference.key)
                    self(self, *reference.key, path + ".key", depth + 1);
                if (reference.value)
                    self(self, *reference.value, path + ".value", depth + 1);
            };
            references(references, property.type, "type", 0);
        };
        for (const auto &type : reflection.types)
        {
            for (const auto &message : type.layoutConflicts)
                diagnostic_("type_conflict owner=" + type.fullName + " address=" + std::to_string(type.address) + " message=" + message);
            for (const auto &property : type.properties)
                propertyEvidence(property, type.fullName, type.address, "type-field");
        }
        for (const auto &[address, function] : reflection.functions)
        {
            const EntryKey key{function.execEntry, function.entryReadable, function.entryExecutable, function.entryInModule};
            std::ostringstream line;
            line << "function_entry address=0x" << std::hex << address << " owner=0x" << function.outerAddress
                 << " flags=0x" << function.flags << std::dec;
            if (const auto id = entryIds.find(key); id != entryIds.end())
                line << " entry_id=" << id->second;
            else
            {
                line << " entry=0x" << std::hex << function.execEntry << std::dec
                     << " readable=" << function.entryReadable << " executable=" << function.entryExecutable
                     << " in_module=" << function.entryInModule;
                if (function.execEntryRva)
                    line << " rva=0x" << std::hex << *function.execEntryRva << std::dec;
            }
            line << " name=" << std::quoted(function.name) << " class=" << function.reflectedClass
                 << " native=" << function.nativeFlag << " native_exec_available=" << function.nativeExecRva.has_value()
                 << " header_readable=" << function.headerReadable
                 << " params=" << static_cast<unsigned int>(function.headerNumParams) << '/' << function.headerParamSize
                 << " return=" << function.returnValueOffset
                 << " consistent=" << function.parameterSemanticsConsistent
                 << " status=" << ParseStatusName(function.status);
            if (function.defaultInitializerCount != 0)
                line << " default_initializers=" << function.defaultInitializerCount;
            if (!function.parameterSemanticsConsistent)
                line << " derived_params=" << function.derivedNumParams << '/' << function.derivedParamSize
                     << " semantics_valid=" << function.parameterSemanticsValid;
            diagnostic_(line.str());
            for (const auto &message : function.layoutConflicts)
                diagnostic_("function_conflict address=" + std::to_string(address) + " message=" + message);
            for (const auto &property : function.parameters)
                propertyEvidence(property, function.fullName, address, "function-parameter");
            for (const auto &property : function.locals)
                propertyEvidence(property, function.fullName, address, "function-local");
        }
        diagnostic_("container_storage_probe abi_selected=0 max_observations=512 max_owners_per_shape=2 max_candidates=8192 priority=representation-gaps-first shape_checks=necessary-not-sufficient");
        for (const auto &[kind, count] : reflection.containerStorageCandidates)
            diagnostic_("container_storage_candidates class=" + kind + " count=" + std::to_string(count));
        for (const auto &[reason, count] : reflection.containerStorageNotObserved)
            diagnostic_("container_storage_not_observed reason=" + reason + " count=" + std::to_string(count));
        for (const auto &observation : reflection.containerStorageObservations)
        {
            std::ostringstream line;
            line << "container_storage_candidate property=0x" << std::hex << observation.propertyAddress
                 << " owner=0x" << observation.ownerAddress << " address=0x" << observation.address << std::dec
                 << " owner_is_uobject=" << observation.ownerIsUObject
                 << " name=" << std::quoted(observation.propertyName) << " class=" << observation.propertyClass
                 << " member=" << observation.member << " offset=" << observation.offset
                 << " reference_offset=" << observation.referenceOffset << " property_data_end=" << observation.propertyDataEnd
                 << " storage_size=" << observation.storageSize << " inner_size=" << observation.innerSize
                 << " inner_class=" << observation.innerClass << " value_size=" << observation.valueSize
                 << " value_class=" << observation.valueClass << " readable=" << observation.readable
                 << " read_error=" << observation.readError << " requested=" << observation.requested
                 << " transferred=" << observation.transferred
                 << " status=" << observation.status << " basis=" << std::quoted(observation.basis);
            if (observation.sparseShapeConsistent)
                line << " sparse_shape_consistent=" << *observation.sparseShapeConsistent;
            if (observation.compactShapeConsistent)
                line << " compact_shape_consistent=" << *observation.compactShapeConsistent;
            line << " bytes=" << std::hex << std::setfill('0');
            for (uint8_t byte : observation.bytes)
                line << std::setw(2) << static_cast<unsigned int>(byte);
            diagnostic_(line.str());
            for (const auto &flag : observation.flagCandidates)
                diagnostic_("container_flag_candidate property=" + std::to_string(observation.propertyAddress) +
                            " offset=" + std::to_string(flag.offset) + " width=" + std::to_string(flag.width) +
                            " raw=" + std::to_string(flag.raw) + " known_value=" + std::to_string(flag.knownValue) +
                            " basis=" + flag.basis + " selected=0");
        }
        for (const auto &enumeration : reflection.enums)
            for (const auto &message : enumeration.diagnostics)
                diagnostic_("enum_diagnostic owner=" + enumeration.fullName + " address=" + std::to_string(enumeration.address) + " message=" + message);
    }

    ReflectionIR ReflectionReader::ReadAttempt(::anduefker::memory::CaptureValidation &validation,
                                               ::anduefker::ir::CaptureInfo::Attempt &details)
    {
        ReflectionIR result;
        delegateSignatures_.clear();
        pendingSignatures_.clear();
        nextSignature_ = 0;
        functionChains_.clear();
        observedContainers_.clear();
        pendingContainers_.clear();
        containerSampleOwners_.clear();
        containerCandidates_.clear();
        containerNotObserved_.clear();
        containerObservations_.clear();
        ::anduefker::memory::CaptureObservationScope observe(memory_);
        if (!objects_.Initialize())
        {
            result.status = ParseStatus::Failed;
            ++result.stats.failures;
            result.diagnostics.push_back("reflection object store initialization failed");
            details.reason = "object-store-initialization-failed";
            return result;
        }
        result.stats.objectSlots = objects_.Count();
        details.initialCount = objects_.Count();
        constexpr int32_t maxObjectSlots = 2 * 1024 * 1024;
        constexpr int32_t maxAdditionalSlots = 65536;
        constexpr uint32_t maxTailRounds = 3;
        if (objects_.Count() > maxObjectSlots)
        {
            result.stats.unvisitedObjects = objects_.Count();
            ++result.stats.failures;
            details.reason = "initial-object-slot-budget-exceeded";
            return result;
        }

        constexpr int32_t maxObjectSamplesPerReason = 8;
        const auto recordObjectDiagnostic = [&](int32_t count, const auto &message)
        {
            if (diagnostic_)
                diagnostic_(message());
            if (count <= maxObjectSamplesPerReason)
                result.diagnostics.push_back(message());
            else
                ++result.stats.objectDiagnosticSamplesOmitted;
        };
        const auto objectIdentity = [&](int32_t index, uintptr_t object, const std::string &className,
                                        uint32_t flags)
        {
            const auto name = objects_.Name(object);
            return "index=" + std::to_string(index) + " address=" + std::to_string(object) +
                   " class=" + className + " name=" + (name ? *name : "<unreadable>") +
                   " flags=" + std::to_string(flags);
        };

        std::unordered_set<uintptr_t> seenTypes;
        std::unordered_map<uintptr_t, std::optional<::anduefker::ue::DefinitionKind>> classKinds;
        std::unordered_map<std::string, size_t> failureReasons;
        auto lastProgress = std::chrono::steady_clock::now();
        const auto recordFailure = [&](const std::string &reason, int32_t index, uintptr_t address)
        {
            const size_t count = ++failureReasons[reason];
            if (diagnostic_)
                diagnostic_("reflection_object_failure reason=" + reason +
                            " index=" + std::to_string(index) + " address=" + std::to_string(address));
            if (count <= 8)
                result.diagnostics.push_back("reflection object failure: reason=" + reason +
                                             " index=" + std::to_string(index) + " address=" + std::to_string(address));
        };
        for (int32_t index = 0;; ++index)
        {
            if (index == objects_.Count())
            {
                details.enumeratedCount = index;
                if (progress_)
                    progress_("capture: validating observed bytes; enumerated=" + std::to_string(index));
                CloseDelegateSignatures(result);
                CollectContainerStorage();
                validation = memory_.Validate();
                const auto boundary = objects_.RefreshObjectCount();
                details.countAddress = boundary.countAddress;
                details.finalCount = boundary.count;
                if (!boundary.valid)
                {
                    details.reason = boundary.reason;
                    ++result.stats.failures;
                    break;
                }
                if (!validation.Stable())
                {
                    details.reason = "observed-bytes-changed-or-unreadable";
                    break;
                }
                if (boundary.count == index)
                {
                    details.coverageComplete = true;
                    break;
                }
                result.stats.objectSlots = boundary.count;
                if (details.tailRounds >= maxTailRounds || boundary.count > maxObjectSlots ||
                    boundary.count - details.initialCount > maxAdditionalSlots)
                {
                    result.stats.unvisitedObjects = boundary.count - index;
                    ++result.stats.failures;
                    details.reason = "object-tail-budget-exceeded";
                    break;
                }
                ++details.tailRounds;
                if (progress_)
                    progress_("capture: count grew from=" + std::to_string(index) + " to=" + std::to_string(boundary.count) +
                              "; reading tail round=" + std::to_string(details.tailRounds));
            }
            if (progress_ && index % 256 == 0 && std::chrono::steady_clock::now() - lastProgress >= std::chrono::seconds(1))
            {
                progress_("reflection visited=" + std::to_string(index) + "/" + std::to_string(objects_.Count()) +
                          " types=" + std::to_string(result.stats.parsedTypes));
                lastProgress = std::chrono::steady_clock::now();
            }
            if (memory_.LimitExceeded())
            {
                result.stats.unvisitedObjects = objects_.Count() - index;
                ++result.stats.failures;
                result.diagnostics.push_back("capture observation limit reached; remaining objects were not visited");
                details.reason = "observation-budget-exceeded";
                validation = memory_.Validate();
                break;
            }
            const ObjectReadResult objectResult = objects_.Objects().ReadObject(index);
            if (!objectResult.IsValid())
            {
                ++result.stats.skippedObjects;
                if (objectResult.status == ObjectReadStatus::Empty)
                {
                    ++result.stats.emptyObjectSlots;
                    recordObjectDiagnostic(result.stats.emptyObjectSlots, [&]
                                           { return "empty object slot: index=" + std::to_string(index) +
                                                    " read_address=" + std::to_string(objectResult.readAddress); });
                }
                else
                {
                    ++result.stats.objectReadFailures;
                    ++result.stats.failures;
                    recordObjectDiagnostic(result.stats.objectReadFailures, [&]
                                           { return "object slot could not be read: index=" + std::to_string(index) +
                                                    " reason=" + ::anduefker::ue::ObjectReadStatusName(objectResult.status) +
                                                    " read_address=" + std::to_string(objectResult.readAddress) +
                                                    " pointer=" + std::to_string(objectResult.address) +
                                                    " read_error=" + std::to_string(static_cast<int>(objectResult.readError)); });
                }
                continue;
            }
            const uintptr_t object = objectResult.address;
            ++result.stats.validObjects;
            const auto className = objects_.ClassName(object);
            if (!className)
            {
                ++result.stats.classNameReadFailures;
                ++result.stats.failures;
                recordObjectDiagnostic(result.stats.classNameReadFailures, [&]
                                       { return "object class name could not be read: index=" + std::to_string(index) +
                                                " address=" + std::to_string(object); });
                continue;
            }
            const auto classAddress = objects_.Class(object);
            if (!classAddress)
            {
                ++result.stats.failures;
                recordFailure("class-pointer-unreadable", index, object);
                continue;
            }
            auto classification = classKinds.find(*classAddress);
            if (classification == classKinds.end())
            {
                ::anduefker::memory::CaptureObservationScope observe(memory_);
                classification = classKinds.emplace(*classAddress, objects_.DefinitionKindForClass(*classAddress)).first;
            }
            if (!classification->second)
            {
                ++result.stats.failures;
                ++result.stats.classNameReadFailures;
                recordFailure("class-hierarchy-unresolved", index, object);
                continue;
            }
            const auto kind = *classification->second;
            if (index >= details.initialCount)
            {
                const auto tailIndex = objects_.InternalIndex(object);
                const auto tailFlags = objects_.Flags(object);
                const auto internalFlags = objects_.Objects().ReadInternalFlags(objectResult);
                if (!tailIndex || *tailIndex != index || !tailFlags ||
                    !internalFlags || (*internalFlags & 0x80000000u) != 0 || // PendingConstruction in supported layouts.
                    (*tailFlags & ::anduefker::ue::kRFUnavailableDefinition) != 0 ||
                    objects_.InternalIndex(object, true) != tailIndex || objects_.Class(object, true) != classAddress ||
                    objects_.Objects().ReadObject(index, true).address != object)
                {
                    ++result.stats.failures;
                    ++result.stats.skippedIncompleteObjects;
                    recordFailure("new-object-not-ready-or-identity-changed", index, object);
                    memory_.Invalidate();
                    continue;
                }
            }
            if (kind == ::anduefker::ue::DefinitionKind::Other)
                continue;
            ::anduefker::memory::CaptureObservationScope observe(memory_);
            const auto verifiedObject = objects_.Objects().ReadObject(index, true);
            const auto observedIndex = objects_.InternalIndex(object);
            const auto internalIndex = objects_.InternalIndex(object, true);
            const auto verifiedClass = objects_.Class(object, true);
            if (!verifiedObject.IsValid() || verifiedObject.address != object || !internalIndex || *internalIndex != index ||
                observedIndex != internalIndex ||
                !verifiedClass || *verifiedClass != *classAddress)
            {
                ++result.stats.failures;
                ++result.stats.identityFailures;
                memory_.Invalidate();
                recordObjectDiagnostic(result.stats.identityFailures, [&]
                                       { return "reflection object identity mismatch: index=" + std::to_string(index) +
                                                " address=" + std::to_string(object); });
                continue;
            }
            const auto objectFlags = objects_.Flags(object);
            if (!objectFlags)
            {
                ++result.stats.failures;
                recordFailure("object-flags-unreadable", index, object);
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFClassDefaultObject) != 0)
            {
                ++result.stats.skippedClassDefaultObjects;
                ++result.stats.skippedObjects;
                recordObjectDiagnostic(result.stats.skippedClassDefaultObjects, [&]
                                       { return "skipped reflection class default object: " +
                                                objectIdentity(index, object, *className, *objectFlags); });
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFUnavailableDefinition) != 0)
            {
                ++result.stats.skippedIncompleteObjects;
                ++result.stats.skippedObjects;
                ++result.stats.failures;
                recordObjectDiagnostic(result.stats.skippedIncompleteObjects, [&]
                                       { return "reflection definition is not ready or is being destroyed: " +
                                                objectIdentity(index, object, *className, *objectFlags); });
                continue;
            }
            if (kind == ::anduefker::ue::DefinitionKind::Enum)
            {
                result.enums.push_back(ReadEnum(object, result.stats));
                ++result.stats.parsedEnums;
                if (index >= details.initialCount)
                    ++details.additionalEnums;
                continue;
            }
            if (!seenTypes.insert(object).second)
                continue;

            const auto type = ReadType(object, kind == ::anduefker::ue::DefinitionKind::Class ? TypeKind::Class : TypeKind::Struct, result);
            if (!type)
            {
                ++result.stats.failures;
                recordFailure("type-header-unavailable", index, object);
                continue;
            }
            result.types.push_back(*type);
            ++result.stats.parsedTypes;
            if (index >= details.initialCount)
                ++details.additionalTypes;
        }
        for (const auto &candidate : pendingContainers_)
            ++containerNotObserved_[candidate.metadata.normalizedClassName + ":capture-ended-before-probe"];
        pendingContainers_.clear();
        if (!details.coverageComplete && details.reason.empty())
            details.reason = "enumeration-coverage-incomplete";
        result.diagnostics.push_back("object enumeration: initial_count=" + std::to_string(details.initialCount) +
                                     " enumerated_count=" + std::to_string(details.enumeratedCount) +
                                     " final_count=" + std::to_string(details.finalCount) +
                                     " tail_rounds=" + std::to_string(details.tailRounds) + " reason=" + details.reason);

        if (result.stats.objectDiagnosticSamplesOmitted != 0)
            result.diagnostics.push_back("object diagnostic samples omitted=" +
                                         std::to_string(result.stats.objectDiagnosticSamplesOmitted) +
                                         "; sample limit per reason=" + std::to_string(maxObjectSamplesPerReason));
        for (const auto &[reason, count] : failureReasons)
            result.diagnostics.push_back("reflection object failure summary: reason=" + reason +
                                         " total=" + std::to_string(count) +
                                         " samples_omitted=" + std::to_string(count > 8 ? count - 8 : 0));

        std::unordered_map<uintptr_t, size_t> enumByAddress;
        for (size_t index = 0; index < result.enums.size(); ++index)
            enumByAddress.emplace(result.enums[index].address, index);
        const auto inferEnumType = [&](const auto &self, const TypeReferenceIR &reference, size_t depth) -> void
        {
            if (depth >= 32)
                return;
            if (reference.kind == PropertyKind::Enum && reference.inner && reference.inner->detailsResolved)
            {
                const auto index = enumByAddress.find(reference.secondaryObject);
                if (index != enumByAddress.end())
                {
                    EnumUnderlyingType underlying = EnumUnderlyingType::Unknown;
                    switch (reference.inner->kind)
                    {
                    case PropertyKind::Int8:
                        underlying = EnumUnderlyingType::Int8;
                        break;
                    case PropertyKind::Byte:
                        underlying = EnumUnderlyingType::UInt8;
                        break;
                    case PropertyKind::Int16:
                        underlying = EnumUnderlyingType::Int16;
                        break;
                    case PropertyKind::UInt16:
                        underlying = EnumUnderlyingType::UInt16;
                        break;
                    case PropertyKind::Int32:
                        underlying = EnumUnderlyingType::Int32;
                        break;
                    case PropertyKind::UInt32:
                        underlying = EnumUnderlyingType::UInt32;
                        break;
                    case PropertyKind::Int64:
                        underlying = EnumUnderlyingType::Int64;
                        break;
                    case PropertyKind::UInt64:
                        underlying = EnumUnderlyingType::UInt64;
                        break;
                    default:
                        break;
                    }
                    EnumIR &enumeration = result.enums[index->second];
                    if (enumeration.underlyingType == EnumUnderlyingType::Unknown)
                        enumeration.underlyingType = underlying;
                    else if (enumeration.underlyingType != underlying)
                    {
                        enumeration.status = ParseStatus::Partial;
                        enumeration.diagnostics.push_back("conflicting enum underlying property types");
                        ++result.stats.layoutConflicts;
                    }
                }
            }
            if (reference.inner)
                self(self, *reference.inner, depth + 1);
            if (reference.key)
                self(self, *reference.key, depth + 1);
            if (reference.value)
                self(self, *reference.value, depth + 1);
        };
        for (const TypeIR &type : result.types)
            for (const PropertyIR &property : type.properties)
                inferEnumType(inferEnumType, property.type, 0);
        for (const auto &[address, function] : result.functions)
        {
            (void)address;
            for (const PropertyIR &parameter : function.parameters)
                inferEnumType(inferEnumType, parameter.type, 0);
            for (const PropertyIR &local : function.locals)
                inferEnumType(inferEnumType, local.type, 0);
        }

        std::unordered_map<std::string, size_t> detailCounts;
        const auto noteDetails = [&](const PropertyIR &property, const std::string &owner, uintptr_t ownerAddress)
        {
            for (const auto &detail : property.detailDiagnostics)
            {
                if (++detailCounts[detail.reason + ":" + detail.normalizedClass] <= 8)
                    result.diagnostics.push_back("property detail failure: owner=" + owner +
                                                 " owner_address=" + std::to_string(ownerAddress) +
                                                 " root=" + property.name + " property=" + detail.name +
                                                 " property_class=" + detail.reflectedClass +
                                                 " address=" + std::to_string(detail.address) +
                                                 " reason=" + detail.reason +
                                                 " referenced_address=" + std::to_string(detail.referencedAddress) +
                                                 " secondary_address=" + std::to_string(detail.secondaryAddress));
            }
        };
        for (const TypeIR &type : result.types)
            for (const auto &property : type.properties)
                noteDetails(property, type.fullName, type.address);
        for (const auto &[address, function] : result.functions)
        {
            (void)address;
            for (const auto &property : function.parameters)
                noteDetails(property, function.fullName, function.address);
            for (const auto &property : function.locals)
                noteDetails(property, function.fullName, function.address);
        }
        for (const auto &[reason, count] : detailCounts)
            result.diagnostics.push_back("property detail summary: reason_and_class=" + reason + " total=" + std::to_string(count) +
                                         " samples_omitted=" + std::to_string(count > 8 ? count - 8 : 0));

        ::anduefker::ir::AnalyzeReflectionLayouts(result);
        if (result.stats.parsedTypes == 0)
            result.status = ParseStatus::Failed;
        else if (result.stats.failures != 0 || result.stats.unknownProperties != 0 ||
                 result.stats.unresolvedTypeDetails != 0 || result.stats.layoutConflicts != 0)
            result.status = ParseStatus::Partial;
        else
            result.status = ParseStatus::Complete;
        return result;
    }
} // namespace anduefker::reflection
