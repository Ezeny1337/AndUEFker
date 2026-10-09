#pragma once

#include <memory>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/binding/BindingBuilder.hpp"
#include "anduefker/generation/ArtifactWriter.hpp"
#include "anduefker/binding/GlobalLocator.hpp"
#include "anduefker/module/ModuleCatalog.hpp"
#include "anduefker/reflection/ReflectionReader.hpp"
#include "anduefker/memory/RemoteMemorySource.hpp"
#include "anduefker/ue/SchemaResolver.hpp"

namespace anduefker::app
{
    using ::anduefker::binding::BindingBuilder;
    using ::anduefker::binding::BindingCandidates;
    using ::anduefker::binding::GlobalLocator;
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::generation::ArtifactResult;
    using ::anduefker::generation::ArtifactWriter;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::memory::RemoteMemorySource;
    using ::anduefker::module::ModuleCatalog;
    using ::anduefker::reflection::ReflectionReader;
    using ::anduefker::ue::EngineSchema;
    using ::anduefker::ue::SchemaCandidateSummary;
    using ::anduefker::ue::SchemaResolutionReport;
    using ::anduefker::ue::SchemaResolver;
    using ::anduefker::ue::SchemaSelectionResult;

    enum class RuntimeLogLevel
    {
        Debug,
        Info,
        Warning,
        Error,
    };

    struct RuntimeLogEntry
    {
        RuntimeLogLevel level = RuntimeLogLevel::Info;
        std::string message;
    };

    enum class RuntimeSessionStatus
    {
        Failed,
        BindingReady,
        SchemaReady,
        ReflectionPartial,
        ReflectionReady,
        ArtifactPartial,
    };

    struct RuntimeSessionConfig
    {
        std::string packageName;
        std::string outputRoot;
        std::vector<std::string> moduleNames = {"libUnreal.so", "libUE4.so"};
        bool detailedDiagnostics = false;
    };

    class RuntimeSession
    {
    public:
        explicit RuntimeSession(RuntimeSessionConfig config);

        [[nodiscard]] RuntimeSessionStatus Run();
        [[nodiscard]] const RuntimeContext &Context() const { return context_; }
        [[nodiscard]] const ReflectionIR &Reflection() const { return reflection_; }
        [[nodiscard]] const std::vector<std::string> &Failures() const { return failures_; }
        [[nodiscard]] const ArtifactResult &Artifacts() const { return artifacts_; }
        [[nodiscard]] std::string LogPath() const;
        [[nodiscard]] const std::vector<RuntimeLogEntry> &LogEntries() const { return logEntries_; }

    private:
        void Note(std::string message);
        void Note(RuntimeLogLevel level, std::string message);
        [[nodiscard]] bool FlushDiagnostics() const;
        [[nodiscard]] RuntimeSessionStatus RunImpl();

        RuntimeSessionConfig config_;
        std::shared_ptr<RemoteMemorySource> memory_;
        RuntimeContext context_;
        ReflectionIR reflection_;
        ArtifactResult artifacts_;
        std::vector<std::string> failures_;
        std::vector<RuntimeLogEntry> logEntries_;
        std::chrono::steady_clock::time_point started_;
        std::ofstream liveLog_;
        bool liveLogFailed_ = false;
    };
} // namespace anduefker::app
