#pragma once

#include <memory>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "anduefker/app/CompatibilityDiagnosis.hpp"
#include "anduefker/compat/GameProfile.hpp"
#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/app/RuntimeContext.hpp"
#include "anduefker/generation/ArtifactWriter.hpp"

namespace anduefker::memory
{
    class RemoteMemorySource;
}

namespace anduefker::app
{
    using ::anduefker::generation::ArtifactResult;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::memory::RemoteMemorySource;

    enum class RuntimeLogLevel
    {
        Debug,
        Info,
        Warning,
        Error,
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
        [[nodiscard]] const CompatibilityDiagnosis &Diagnosis() const { return diagnosis_; }
        [[nodiscard]] std::string LogPath() const;
        [[nodiscard]] std::string DiagnosisPath() const;

    private:
        void Note(std::string message);
        void Note(RuntimeLogLevel level, std::string message);
        void RecordDiagnosis(DiagnosisDomain domain, std::string code, std::string stage,
                             std::string confidence, std::string summary, std::string nextAction);
        [[nodiscard]] bool WriteDiagnosisReport() const;
        [[nodiscard]] bool FlushDiagnostics();
        [[nodiscard]] RuntimeSessionStatus RunImpl();

        RuntimeSessionConfig config_;
        std::shared_ptr<RemoteMemorySource> memory_;
        std::vector<std::unique_ptr<compat::IGameProfile>> profiles_;
        RuntimeContext context_;
        ReflectionIR reflection_;
        ArtifactResult artifacts_;
        CompatibilityDiagnosis diagnosis_;
        generation::ArtifactProvenance provenance_;
        std::vector<std::string> failures_;
        std::chrono::steady_clock::time_point started_;
        std::ofstream liveLog_;
        bool liveLogFailed_ = false;
        mutable std::string logPath_;
        mutable std::string diagnosisPath_;
    };
} // namespace anduefker::app
