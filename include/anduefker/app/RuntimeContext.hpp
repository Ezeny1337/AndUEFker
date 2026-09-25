#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::app
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;
    using ::anduefker::ue::EngineSchema;

    struct ModuleImage
    {
        struct Segment
        {
            uintptr_t start = 0;
            uintptr_t end = 0;
            uintptr_t fileOffset = 0;
            bool readable = false;
            bool writable = false;
            bool executable = false;
            bool privateMapping = false;
            std::string path;
        };

        uintptr_t base = 0;
        uintptr_t end = 0;
        std::string name;
        std::vector<Segment> segments;

        [[nodiscard]] bool IsValid() const { return base != 0 && end > base; }
        [[nodiscard]] bool Contains(uintptr_t address) const { return IsValid() && address >= base && address < end; }
        [[nodiscard]] uintptr_t ToRva(uintptr_t address) const { return Contains(address) ? address - base : 0; }
        [[nodiscard]] bool IsExecutable(uintptr_t address, size_t size = 1) const;
        [[nodiscard]] bool IsWritable(uintptr_t address, size_t size = 1) const;
    };

    class RuntimeContext
    {
    public:
        RuntimeContext() = default;
        explicit RuntimeContext(std::shared_ptr<IMemorySource> memory) : memory_(std::move(memory)) {}

        [[nodiscard]] IMemorySource *Memory() { return memory_.get(); }
        [[nodiscard]] const IMemorySource *Memory() const { return memory_.get(); }
        [[nodiscard]] const ModuleImage &Module() const { return module_; }
        [[nodiscard]] const RuntimeBinding &Binding() const { return binding_; }
        [[nodiscard]] const EngineSchema &Schema() const { return schema_; }

        void SetModule(ModuleImage module) { module_ = std::move(module); }
        void CommitBinding(RuntimeBinding binding) { binding_ = std::move(binding); }
        void CommitSchema(EngineSchema schema) { schema_ = std::move(schema); }

        [[nodiscard]] bool IsReadyForReflection() const
        {
            return memory_ && module_.IsValid() && binding_.IsValid() && schema_.IsReadyForReflection();
        }

    private:
        std::shared_ptr<IMemorySource> memory_;
        ModuleImage module_;
        RuntimeBinding binding_;
        EngineSchema schema_;
    };
} // namespace anduefker::app
