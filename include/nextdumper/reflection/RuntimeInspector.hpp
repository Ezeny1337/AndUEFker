#pragma once

#include "nextdumper/module/ModuleImage.hpp"
#include "nextdumper/ir/ReflectionIR.hpp"
#include "nextdumper/ue/EngineSchema.hpp"
#include "nextdumper/memory/MemorySource.hpp"
#include "nextdumper/binding/RuntimeBinding.hpp"

namespace nextdumper::reflection
{
    // 仅在定义捕获结束后执行，预算和稳定性均与 ReflectionReader 分离
    [[nodiscard]] ir::RuntimeEvidence InspectRuntimeValues(memory::IMemorySource &memory,
                                                           const binding::RuntimeBinding &binding, const ue::EngineSchema &schema,
                                                           const module::ModuleImage &module, const ir::ReflectionIR &reflection,
                                                           ir::SparseRootEvidence sparseRoot);
} // namespace nextdumper::reflection
