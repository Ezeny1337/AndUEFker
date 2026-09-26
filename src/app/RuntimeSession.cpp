#include "anduefker/app/RuntimeSession.hpp"

#include <KittyMemoryEx.hpp>

#include <filesystem>
#include <fstream>
#include <utility>

#include "anduefker/binding/CommonObjectCollector.hpp"

namespace anduefker::app
{
    using ::anduefker::binding::AddressMeaning;
    using ::anduefker::binding::LocatedAddress;
    using ::anduefker::ir::ParseStatus;

    namespace
    {
        const char *AddressMeaningName(AddressMeaning meaning)
        {
            switch (meaning)
            {
            case AddressMeaning::Direct:
                return "direct";
            case AddressMeaning::PointerSlot:
                return "pointer-slot";
            case AddressMeaning::ResolvedValue:
                return "resolved-value";
            }
            return "unknown";
        }

        const char *ParseStatusName(ParseStatus status)
        {
            switch (status)
            {
            case ParseStatus::Complete:
                return "Complete";
            case ParseStatus::Partial:
                return "Partial";
            case ParseStatus::Failed:
                return "Failed";
            }
            return "Failed";
        }

        const char *LevelName(RuntimeLogLevel level)
        {
            switch (level)
            {
            case RuntimeLogLevel::Debug:
                return "DEBUG";
            case RuntimeLogLevel::Info:
                return "INFO";
            case RuntimeLogLevel::Warning:
                return "WARN";
            case RuntimeLogLevel::Error:
                return "ERROR";
            }
            return "INFO";
        }
    } // namespace

    RuntimeSession::RuntimeSession(RuntimeSessionConfig config) : config_(std::move(config))
    {
        memory_ = std::make_shared<RemoteMemorySource>();
        context_ = RuntimeContext(memory_);
    }

    std::string RuntimeSession::LogPath() const
    {
        if (config_.outputRoot.empty())
            return {};
        return (std::filesystem::path(config_.outputRoot) / "AndUEFker.log").string();
    }

    void RuntimeSession::Note(std::string message)
    {
        Note(RuntimeLogLevel::Info, std::move(message));
    }

    void RuntimeSession::Note(RuntimeLogLevel level, std::string message)
    {
        logEntries_.push_back({level, message});
        diagnostics_.push_back(message);

        // Immediate console output for non-debug messages
        if (level != RuntimeLogLevel::Debug)
        {
            const char *levelName = level == RuntimeLogLevel::Error     ? "ERROR"
                                    : level == RuntimeLogLevel::Warning ? "WARN"
                                                                        : "INFO";
            std::printf("[%s] %s\n", levelName, message.c_str());
            std::fflush(stdout);
        }
    }

    void RuntimeSession::FlushDiagnostics() const
    {
        const std::string path = LogPath();
        if (path.empty())
            return;
        std::error_code error;
        const std::filesystem::path logPath(path);
        if (logPath.has_parent_path())
            std::filesystem::create_directories(logPath.parent_path(), error);
        if (error)
            return;
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream.is_open())
            return;
        for (const RuntimeLogEntry &entry : logEntries_)
            stream << '[' << LevelName(entry.level) << "] " << entry.message << '\n';
    }

    RuntimeSessionStatus RuntimeSession::Run()
    {
        failures_.clear();
        diagnostics_.clear();
        logEntries_.clear();
        reflection_ = {};
        Note("=== Runtime Session Started ===");
        Note("Package=" + config_.packageName + " UE=" + config_.engineVersion.ToString());

        if (config_.pid <= 0)
            config_.pid = KittyMemoryEx::getProcessID(config_.packageName);
        if (config_.pid <= 0)
        {
            failures_.push_back("target process was not found");
            Note(RuntimeLogLevel::Error, failures_.back());
            FlushDiagnostics();
            return RuntimeSessionStatus::Failed;
        }
        if (!memory_->Initialize(static_cast<pid_t>(config_.pid)))
        {
            failures_.push_back("remote memory source initialization failed");
            Note(RuntimeLogLevel::Error, failures_.back());
            FlushDiagnostics();
            return RuntimeSessionStatus::Failed;
        }

        ModuleImage module;
        if (!ModuleCatalog::Discover(*memory_, config_.moduleNames, module))
        {
            failures_.push_back("Unreal module was not found");
            Note(RuntimeLogLevel::Error, failures_.back());
            FlushDiagnostics();
            return RuntimeSessionStatus::Failed;
        }
        context_.SetModule(std::move(module));
        Note("Module=" + context_.Module().name + " base=" + std::to_string(context_.Module().base));

        GlobalLocator locator(*memory_, context_.Module());
        const BindingCandidates candidates = locator.Locate(
            {"GUObjectArray", "GObjects", "ObjObjects"},
            {"GNameBlocksDebug", "GFNameTableForDebuggerVisualizers_MT", "NamePoolData"});
        Note(RuntimeLogLevel::Debug, "Object candidates=" + std::to_string(candidates.objectRoots.size()));
        Note(RuntimeLogLevel::Debug, "Name candidates=" + std::to_string(candidates.nameRoots.size()));
        for (size_t index = 0; index < candidates.objectRoots.size(); ++index)
        {
            const LocatedAddress &candidate = candidates.objectRoots[index];
            Note(RuntimeLogLevel::Debug, "object_candidate[" + std::to_string(index) + "] address=" +
                                             std::to_string(candidate.address) + " meaning=" + AddressMeaningName(candidate.meaning) +
                                             " confidence=" + std::to_string(candidate.confidence) + " source=" + candidate.source);
        }
        for (size_t index = 0; index < candidates.nameRoots.size(); ++index)
        {
            const LocatedAddress &candidate = candidates.nameRoots[index];
            Note(RuntimeLogLevel::Debug, "name_candidate[" + std::to_string(index) + "] address=" +
                                             std::to_string(candidate.address) + " meaning=" + AddressMeaningName(candidate.meaning) +
                                             " confidence=" + std::to_string(candidate.confidence) + " source=" + candidate.source);
        }

        BindingBuilder builder(*memory_);
        const auto binding = builder.Build(candidates);
        if (!binding)
        {
            failures_.push_back("runtime binding failed; static symbol candidates were insufficient");
            Note(RuntimeLogLevel::Error, failures_.back());
            FlushDiagnostics();
            return RuntimeSessionStatus::Failed;
        }
        context_.CommitBinding(*binding);
        Note(RuntimeLogLevel::Info, "Runtime binding validated");
        for (const std::string &evidence : context_.Binding().report.evidence)
            Note(RuntimeLogLevel::Debug, "binding: " + evidence);

        EngineSchema schema;
        SchemaResolver resolver(*memory_, context_.Binding(), config_.engineVersion);
        const SchemaResolutionReport schemaReport = resolver.Resolve(schema);
        for (const std::string &evidence : schemaReport.evidence)
            Note(RuntimeLogLevel::Debug, "schema: " + evidence);
        for (const std::string &failure : schemaReport.failures)
            Note(RuntimeLogLevel::Warning, "schema failure: " + failure);
        const ReadStats &memoryStats = memory_->Stats();
        Note(RuntimeLogLevel::Debug, "memory stats operations=" + std::to_string(memoryStats.operations) +
                                         " requested_bytes=" + std::to_string(memoryStats.requestedBytes) +
                                         " transferred_bytes=" + std::to_string(memoryStats.transferredBytes) +
                                         " failures=" + std::to_string(memoryStats.failures));
        if (!schemaReport.accepted)
        {
            failures_.insert(failures_.end(), schemaReport.failures.begin(), schemaReport.failures.end());
            for (const std::string &failure : schemaReport.failures)
                Note(RuntimeLogLevel::Error, failure);
            FlushDiagnostics();
            return RuntimeSessionStatus::BindingReady;
        }
        context_.CommitSchema(std::move(schema));
        Note(RuntimeLogLevel::Info, "Engine schema resolved");

        Note(RuntimeLogLevel::Info, "Collecting common object classes...");
        binding::CommonObjectCollector collector(*memory_, context_.Binding(), context_.Schema());
        std::vector<binding::CommonObjectInfo> commonObjects = collector.Collect();
        Note(RuntimeLogLevel::Info, "Found " + std::to_string(commonObjects.size()) + " common object classes");
        for (const auto &obj : commonObjects)
        {
            Note(RuntimeLogLevel::Debug, "common_object: " + obj.name +
                                             " address=0x" + std::to_string(obj.address) +
                                             " index=" + std::to_string(obj.index));
        }

        RuntimeBinding updatedBinding = context_.Binding();
        updatedBinding.commonObjects = std::move(commonObjects);
        context_.CommitBinding(std::move(updatedBinding));

        ReflectionReader reader(*memory_, context_.Binding(), context_.Schema(),
                                context_.Module().base, context_.Module().end);
        reflection_ = reader.Read();
        reflection_.diagnostics.push_back("reflection status=" + std::string(ParseStatusName(reflection_.status)));
        if (reflection_.stats.failures != 0)
            reflection_.diagnostics.push_back("reflection read failures=" + std::to_string(reflection_.stats.failures));
        if (reflection_.stats.unknownProperties != 0)
            reflection_.diagnostics.push_back("unknown properties=" + std::to_string(reflection_.stats.unknownProperties));
        if (reflection_.stats.unresolvedTypeDetails != 0)
            reflection_.diagnostics.push_back("unresolved type details=" + std::to_string(reflection_.stats.unresolvedTypeDetails));
        if (reflection_.stats.layoutConflicts != 0)
            reflection_.diagnostics.push_back("layout conflicts=" + std::to_string(reflection_.stats.layoutConflicts));
        Note(RuntimeLogLevel::Info, "Reflection status=" + std::to_string(static_cast<int>(reflection_.status)) +
                                        " types=" + std::to_string(reflection_.stats.parsedTypes) +
                                        " properties=" + std::to_string(reflection_.stats.parsedProperties) +
                                        " functions=" + std::to_string(reflection_.stats.parsedFunctions) +
                                        " enums=" + std::to_string(reflection_.stats.parsedEnums) +
                                        " unknown_properties=" + std::to_string(reflection_.stats.unknownProperties) +
                                        " unresolved_type_details=" + std::to_string(reflection_.stats.unresolvedTypeDetails) +
                                        " failures=" + std::to_string(reflection_.stats.failures));
        if (reflection_.status == ParseStatus::Failed)
        {
            FlushDiagnostics();
            return RuntimeSessionStatus::SchemaReady;
        }
        if (reflection_.status == ParseStatus::Partial)
            Note(RuntimeLogLevel::Warning, "Reflection is partial; output will be written to a .partial artifact");
        if (!config_.outputRoot.empty())
        {
            ArtifactWriter writer(context_, reflection_, config_.outputRoot, config_.packageName);
            artifacts_ = writer.Write();
            if (artifacts_.status == ParseStatus::Failed)
            {
                failures_.push_back(artifacts_.error);
                Note(RuntimeLogLevel::Error, failures_.back());
                FlushDiagnostics();
                return RuntimeSessionStatus::Failed;
            }
            Note(RuntimeLogLevel::Info, "Artifact status=" + std::string(ParseStatusName(artifacts_.status)) +
                                            " files=" + std::to_string(artifacts_.filesWritten) +
                                            " opaque_fields=" + std::to_string(artifacts_.opaqueFields) +
                                            " output=" + artifacts_.outputPath.string());
        }
        Note(RuntimeLogLevel::Info, "=== Runtime Session Completed ===");
        FlushDiagnostics();
        if (reflection_.status == ParseStatus::Partial)
            return RuntimeSessionStatus::ReflectionPartial;
        return RuntimeSessionStatus::ReflectionReady;
    }
} // namespace anduefker::app
