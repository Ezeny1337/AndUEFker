#pragma once

#include <functional>
#include <string>

#include "nextdumper/ir/ReflectionIR.hpp"

namespace nextdumper::reflection::detail
{
    // 仅格式化已捕获 IR，不读取目标内存，不改变解析状态
    void LogReflectionEvidence(const ir::ReflectionIR &reflection, uintptr_t moduleBase,
                               const std::function<void(const std::string &)> &diagnostic);
} // namespace nextdumper::reflection::detail
