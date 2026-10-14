#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/app/RuntimeContext.hpp"

namespace anduefker::generation
{
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::ir::ReflectionIR;

    struct ArtifactProvenance
    {
        std::string producerCommit = "unknown";
        std::string producerVersion = "unknown";
        std::string runId;
        // 与会话日志文件名共用的本地时间戳（YYYYMMDDHHMMSS），用于产物目录命名
        std::string runTimestamp;
        std::string producerWorktree = "unknown";
        int32_t targetPid = -1;
        uint64_t addressSpaceGeneration = 0;
    };

    struct ArtifactResult
    {
        ParseStatus status = ParseStatus::Failed;
        std::filesystem::path outputPath;
        size_t filesWritten = 0;
        size_t opaqueFields = 0;
        size_t omittedFields = 0;
        size_t layoutWarnings = 0;
        ParseStatus reflectionStatus = ParseStatus::Failed;
        ParseStatus sdkStatus = ParseStatus::Failed;
        std::string error;
        std::vector<std::string> generationDiagnostics;
    };

    class ArtifactWriter
    {
    public:
        ArtifactWriter(const RuntimeContext &context,
                       const ReflectionIR &reflection,
                       std::filesystem::path outputRoot,
                       std::string packageName,
                       ArtifactProvenance provenance = {},
                       std::function<void(const std::string &)> diagnostic = {});

        [[nodiscard]] ArtifactResult Write() const;

    private:
        const RuntimeContext &context_;
        const ReflectionIR &reflection_;
        std::filesystem::path outputRoot_;
        std::string packageName_;
        ArtifactProvenance provenance_;
        std::function<void(const std::string &)> diagnostic_;
    };
} // namespace anduefker::generation
