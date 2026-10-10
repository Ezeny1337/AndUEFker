#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_set>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/ue/FunctionSemantics.hpp"
#include "anduefker/ue/ObjectModelReader.hpp"
#include "anduefker/memory/CaptureMemorySource.hpp"

namespace anduefker::reflection
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::ir::EnumIR;
    using ::anduefker::ir::EnumUnderlyingType;
    using ::anduefker::ir::EnumValueIR;
    using ::anduefker::ir::FunctionIR;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::ir::PropertyIR;
    using ::anduefker::ir::PropertyKind;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::ir::ReflectionStats;
    using ::anduefker::ir::TypeIR;
    using ::anduefker::ir::TypeKind;
    using ::anduefker::ir::TypeReferenceIR;
    using ::anduefker::memory::IMemorySource;
    using ::anduefker::ue::AnalyzeFunctionParameters;
    using ::anduefker::ue::EngineSchema;
    using ::anduefker::ue::EnumValueMetadata;
    using ::anduefker::ue::FieldChainResult;
    using ::anduefker::ue::FieldMetadata;
    using ::anduefker::ue::FunctionParameterSummary;
    using ::anduefker::ue::IsFunctionFieldKind;
    using ::anduefker::ue::IsPropertyFieldKind;
    using ::anduefker::ue::NormalizeRuntimeFieldName;
    using ::anduefker::ue::ObjectModelReader;
    using ::anduefker::ue::ObjectReadResult;
    using ::anduefker::ue::ObjectReadStatus;
    using ::anduefker::ue::PropertyMetadata;

    class ReflectionReader
    {
    public:
        ReflectionReader(IMemorySource &memory,
                         const RuntimeBinding &binding,
                         const EngineSchema &schema,
                         uintptr_t moduleBase,
                         uintptr_t moduleEnd,
                         std::function<void(const std::string &)> progress = {},
                         std::function<void(const std::string &)> diagnostic = {});

        [[nodiscard]] ReflectionIR Read();

    private:
        [[nodiscard]] PropertyKind PropertyKindFromName(const std::string &name) const;
        void RecordPropertyDetail(const PropertyMetadata &metadata, PropertyIR &property,
                                  const std::string &reason, bool headerAvailable = true) const;
        [[nodiscard]] std::optional<PropertyIR> ReadProperty(uintptr_t field, ReflectionStats &stats) const;
        [[nodiscard]] TypeReferenceIR ReadTypeReference(const PropertyMetadata &metadata, PropertyIR &property,
                                                        ReflectionStats &stats, std::unordered_set<uintptr_t> &path,
                                                        size_t depth, size_t &remaining) const;
        struct PropertyChain
        {
            std::vector<PropertyIR> properties;
            bool complete = true;
        };
        [[nodiscard]] PropertyChain ReadPropertyChain(uintptr_t first, uintptr_t owner, ReflectionStats &stats,
                                                      std::vector<std::string> &diagnostics) const;
        void ReadProperties(uintptr_t first, TypeIR &type, ReflectionStats &stats) const;
        void ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const;
        [[nodiscard]] FunctionIR *ReadFunction(uintptr_t address, ReflectionIR &ir) const;
        void ReadFunctions(uintptr_t first, TypeIR &type, ReflectionIR &ir) const;
        void CloseDelegateSignatures(ReflectionIR &ir) const;
        void ObserveContainerStorage(const PropertyMetadata &metadata, const TypeReferenceIR &reference) const;
        [[nodiscard]] std::optional<TypeIR> ReadType(uintptr_t object, TypeKind kind, ReflectionIR &ir) const;
        [[nodiscard]] ReflectionIR ReadAttempt(::anduefker::memory::CaptureValidation &validation,
                                               ::anduefker::ir::CaptureInfo::Attempt &details);
        [[nodiscard]] EnumIR ReadEnum(uintptr_t object, ReflectionStats &stats) const;
        void LogEvidence(const ReflectionIR &reflection) const;

        ::anduefker::memory::CaptureMemorySource memory_;
        const EngineSchema &schema_;
        uintptr_t moduleBase_ = 0;
        uintptr_t moduleEnd_ = 0;
        ObjectModelReader objects_;
        std::function<void(const std::string &)> progress_;
        std::function<void(const std::string &)> diagnostic_;
        mutable std::map<uintptr_t, ::anduefker::ir::DelegateSignatureObservation> delegateSignatures_;
        mutable std::vector<uintptr_t> pendingSignatures_;
        mutable size_t nextSignature_ = 0;
        mutable std::map<uintptr_t, std::pair<uintptr_t, ::anduefker::ue::FieldChainStatus>> functionChains_;
        mutable std::unordered_set<uintptr_t> observedContainers_;
        mutable std::map<std::string, std::unordered_set<uintptr_t>> containerSampleOwners_;
        mutable std::map<std::string, size_t> containerCandidates_;
        mutable std::map<std::string, size_t> containerNotObserved_;
        mutable std::vector<::anduefker::ir::ContainerStorageObservation> containerObservations_;
    };
} // namespace anduefker::reflection
