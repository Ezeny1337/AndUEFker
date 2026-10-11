#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "anduefker/module/ModuleImage.hpp"
#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::app
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;
    using ::anduefker::ue::EngineSchema;

    class RuntimeContext
    {
    public:
        RuntimeContext() = default;
        explicit RuntimeContext(std::shared_ptr<IMemorySource> memory) : memory_(std::move(memory)) {}

        [[nodiscard]] IMemorySource *Memory() { return memory_.get(); }
        [[nodiscard]] const IMemorySource *Memory() const { return memory_.get(); }
        [[nodiscard]] const module::ModuleImage &Module() const { return module_; }
        [[nodiscard]] const RuntimeBinding &Binding() const { return binding_; }
        [[nodiscard]] const EngineSchema &Schema() const { return schema_; }

        void SetModule(module::ModuleImage module) { module_ = std::move(module); }
        void CommitBinding(RuntimeBinding binding) { binding_ = std::move(binding); }
        void CommitSchema(EngineSchema schema) { schema_ = std::move(schema); }

        [[nodiscard]] bool IsReadyForReflection() const
        {
            return memory_ && module_.IsValid() && binding_.IsValid() && schema_.IsReadyForReflection();
        }

    private:
        std::shared_ptr<IMemorySource> memory_;
        module::ModuleImage module_;
        RuntimeBinding binding_;
        EngineSchema schema_;
    };
} // namespace anduefker::app
