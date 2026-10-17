#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "nextdumper/ir/ReflectionIR.hpp"
#include "nextdumper/memory/CaptureMemorySource.hpp"
#include "nextdumper/module/ModuleImage.hpp"
#include "nextdumper/ue/ObjectModelReader.hpp"

namespace nextdumper::reflection::detail
{
    // 使用调用方的有界 capture view，不枚举对象，不决定采样配额
    class RuntimeValueReader
    {
    public:
        RuntimeValueReader(memory::CaptureMemorySource &capture, ue::ObjectModelReader &objects,
                           const ue::EngineSchema &schema, const module::ModuleImage &module, const ir::ReflectionIR &reflection);
        void ReadValue(ir::ValueObservation &value, const ir::TypeReferenceIR &reference);
        void ReadSparse(ir::SparseRootEvidence &root);

    private:
        [[nodiscard]] bool ReadHeader(ir::ValueObservation &value, int32_t size);
        [[nodiscard]] ir::DelegateBinding ReadBinding(uintptr_t address, const std::vector<uint8_t> &bytes,
                                                      int32_t weakOffset, int32_t nameOffset, bool remote) const;
        void ReadInvocation(ir::ValueObservation &value, const std::vector<ir::StorageLayoutIR> &layouts);

        memory::CaptureMemorySource &capture_;
        ue::ObjectModelReader &objects_;
        const ue::EngineSchema &schema_;
        int32_t pointerWidth_ = 0;
        const module::ModuleImage &module_;
        std::vector<ir::StorageLayoutIR> inlineLayouts_;
        std::set<int32_t> weakSizes_;
    };
} // namespace nextdumper::reflection::detail
