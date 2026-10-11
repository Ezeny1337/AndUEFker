#pragma once

#include "anduefker/module/ModuleImage.hpp"
#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::reflection
{
    // 仅在定义捕获结束后执行，预算和稳定性均与 ReflectionReader 分离
    [[nodiscard]] ir::RuntimeEvidence InspectRuntimeValues(memory::IMemorySource &memory,
                                                           const binding::RuntimeBinding &binding, const ue::EngineSchema &schema,
                                                           const module::ModuleImage &module, const ir::ReflectionIR &reflection,
                                                           ir::SparseRootEvidence sparseRoot);
} // namespace anduefker::reflection
