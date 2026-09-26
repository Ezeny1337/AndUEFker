#pragma once

#include <cstdint>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/ue/ObjectModelReader.hpp"

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
    using ::anduefker::ue::EngineSchema;
    using ::anduefker::ue::EnumValueMetadata;
    using ::anduefker::ue::ObjectModelReader;

    class ReflectionReader
    {
    public:
        ReflectionReader(const IMemorySource &memory,
                         const RuntimeBinding &binding,
                         const EngineSchema &schema,
                         uintptr_t moduleBase,
                         uintptr_t moduleEnd);

        [[nodiscard]] ReflectionIR Read();

    private:
        [[nodiscard]] PropertyKind PropertyKindFromName(const std::string &name) const;
        [[nodiscard]] std::optional<PropertyIR> ReadProperty(uintptr_t field, size_t depth, ReflectionStats &stats) const;
        void ReadProperties(uintptr_t first, TypeIR &type, ReflectionStats &stats) const;
        void ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const;
        void ReadFunctions(uintptr_t first, TypeIR &type, ReflectionStats &stats) const;
        [[nodiscard]] std::optional<TypeIR> ReadType(uintptr_t object, TypeKind kind, ReflectionIR &ir) const;

        const IMemorySource &memory_;
        const EngineSchema &schema_;
        uintptr_t moduleBase_ = 0;
        uintptr_t moduleEnd_ = 0;
        ObjectModelReader objects_;
    };
} // namespace anduefker::reflection
