#pragma once

#include <iosfwd>

#include "anduefker/generation/ArtifactWriter.hpp"
#include "GenerationReport.hpp"

namespace anduefker::generation::detail
{
    // 纯序列化接口，只消费已生成的报告，不触发目标进程读取
    [[nodiscard]] std::string ManifestJson(const RuntimeContext &context, const ReflectionIR &reflection,
                                           const std::string &packageName, const ArtifactProvenance &provenance,
                                           const GenerationReport &report, ParseStatus status);
    [[nodiscard]] std::string DiagnosticsJson(const RuntimeContext &context, const ReflectionIR &reflection,
                                              const GenerationReport &report, ParseStatus status);
    [[nodiscard]] std::string RuntimeJson(const RuntimeContext &context);
    void ReflectionJson(std::ostream &stream, const RuntimeContext &context, const ReflectionIR &reflection,
                        const GenerationReport &report, const CppSymbols &symbols);
} // namespace anduefker::generation::detail
