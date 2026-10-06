#include "anduefker/app/RuntimeSession.hpp"

#include <KittyMemoryEx.hpp>

#include <filesystem>
#include <fstream>
#include <exception>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

#include "anduefker/binding/CommonObjectCollector.hpp"
#include "anduefker/ue/SchemaCatalog.hpp"

namespace anduefker::app
{
    using ::anduefker::binding::AddressMeaning;
    using ::anduefker::binding::LocatedAddress;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::memory::ReadStats;
    using ::anduefker::ue::CreateSchemaProbeBootstrap;
    using ::anduefker::ue::EngineProfile;
    using ::anduefker::ue::SchemaCatalog;
    using ::anduefker::ue::SchemaLayoutVariantName;

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

        std::string HexAddress(uintptr_t address)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << address;
            return stream.str();
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

        const char *ModuleArchitectureName(ModuleArchitecture architecture)
        {
            switch (architecture)
            {
            case ModuleArchitecture::Arm32:
                return "ARM32";
            case ModuleArchitecture::Arm64:
                return "ARM64";
            case ModuleArchitecture::Unknown:
                break;
            }
            return "unknown";
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

        // 非 debug 消息立即输出到控制台。
        if (level != RuntimeLogLevel::Debug)
        {
            const char *levelName = level == RuntimeLogLevel::Error     ? "ERROR"
                                    : level == RuntimeLogLevel::Warning ? "WARN"
                                                                        : "INFO";
            std::printf("[%s] %s\n", levelName, message.c_str());
            std::fflush(stdout);
        }
    }

    bool RuntimeSession::FlushDiagnostics() const
    {
        const std::string path = LogPath();
        if (path.empty())
            return true;
        std::error_code error;
        const std::filesystem::path logPath(path);
        if (logPath.has_parent_path())
            std::filesystem::create_directories(logPath.parent_path(), error);
        if (error)
            return false;
        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream.is_open())
            return false;
        for (const RuntimeLogEntry &entry : logEntries_)
            stream << '[' << LevelName(entry.level) << "] " << entry.message << '\n';
        stream.flush();
        if (!stream.good())
            return false;
        stream.close();
        return !stream.fail();
    }

    RuntimeSessionStatus RuntimeSession::Run()
    {
        RuntimeSessionStatus status = RuntimeSessionStatus::Failed;
        try
        {
            status = RunImpl();
        }
        catch (const std::bad_alloc &)
        {
            std::fprintf(stderr, "Runtime session failed: memory allocation failed\n");
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "Runtime session exception: %s\n", error.what());
            try
            {
                failures_.push_back(error.what());
                Note(RuntimeLogLevel::Error, std::string("Runtime session exception: ") + error.what());
            }
            catch (...)
            {
                std::fprintf(stderr, "Exception details could not be stored\n");
            }
        }
        catch (...)
        {
            std::fprintf(stderr, "Runtime session failed: unknown exception\n");
        }
        try
        {
            if (!FlushDiagnostics())
            {
                std::fprintf(stderr, "Runtime log write or close failed\n");
                status = RuntimeSessionStatus::Failed;
            }
        }
        catch (...)
        {
            std::fprintf(stderr, "Runtime log could not be flushed\n");
            status = RuntimeSessionStatus::Failed;
        }
        return status;
    }

    RuntimeSessionStatus RuntimeSession::RunImpl()
    {
        failures_.clear();
        logEntries_.clear();
        reflection_ = {};
        artifacts_ = {};
        context_ = RuntimeContext(memory_);
        Note("=== Runtime Session Started ===");
        Note("Package=" + config_.packageName + " PID=auto UE=auto");

        const int pid = KittyMemoryEx::getProcessID(config_.packageName);
        if (pid <= 0)
        {
            failures_.push_back("target process was not found");
            Note(RuntimeLogLevel::Error, failures_.back());
            return RuntimeSessionStatus::Failed;
        }
        Note(RuntimeLogLevel::Debug, "Target PID=" + std::to_string(pid));
        if (!memory_->Initialize(static_cast<pid_t>(pid)))
        {
            failures_.push_back("remote memory source initialization failed");
            Note(RuntimeLogLevel::Error, failures_.back());
            return RuntimeSessionStatus::Failed;
        }

        ModuleImage module;
        if (!ModuleCatalog::Discover(*memory_, config_.moduleNames, module))
        {
            failures_.push_back("Unreal module was not found");
            Note(RuntimeLogLevel::Error, failures_.back());
            return RuntimeSessionStatus::Failed;
        }
        context_.SetModule(std::move(module));
        Note("Module=" + context_.Module().name + " base=" + std::to_string(context_.Module().base) +
             " architecture=" + ModuleArchitectureName(context_.Module().architecture) +
             " pointer_width=" + std::to_string(context_.Module().pointerWidth));

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
            return RuntimeSessionStatus::Failed;
        }
        context_.CommitBinding(*binding);
        Note(RuntimeLogLevel::Info, "Runtime binding validated");
        for (const std::string &evidence : context_.Binding().report.evidence)
            Note(RuntimeLogLevel::Debug, "binding: " + evidence);

        EngineSchema schema;
        EngineProfile selectedProfile;
        SchemaResolutionReport schemaReport;
        SchemaSelectionResult schemaSelection;
        const std::vector<EngineProfile> profiles = SchemaCatalog::Profiles();
        Note(RuntimeLogLevel::Info, "Detecting Unreal Engine schema profile...");
        auto schemaBootstrap = CreateSchemaProbeBootstrap(*memory_, context_.Binding());
        if (!schemaBootstrap->IsValid())
        {
            const std::string failure = schemaBootstrap->failure.empty()
                                            ? "schema bootstrap is unavailable"
                                            : schemaBootstrap->failure;
            failures_.push_back(failure);
            Note(RuntimeLogLevel::Error, failure);
            return RuntimeSessionStatus::BindingReady;
        }
        std::vector<EngineSchema> candidateSchemas;
        auto resolveProfiles = [&]()
        {
            schemaSelection = {};
            candidateSchemas.clear();
            candidateSchemas.reserve(profiles.size());
            schemaSelection.candidates.reserve(profiles.size());
            for (const EngineProfile &profile : profiles)
            {
                EngineSchema candidateSchema;
                SchemaResolver resolver(*memory_, context_.Binding(), profile, {},
                                        context_.Module().base, context_.Module().end, schemaBootstrap);
                SchemaCandidateSummary candidateReport = resolver.Resolve(candidateSchema);
                const std::string reason = candidateReport.failures.empty()
                                               ? (candidateReport.evidence.empty() ? "none" : candidateReport.evidence.front())
                                               : candidateReport.failures.front();
                Note(RuntimeLogLevel::Debug, "schema_candidate id=" + profile.id +
                                                 " range=" + profile.versionRange +
                                                 " layout=" + SchemaLayoutVariantName(profile.layout) +
                                                 " stage=" + candidateReport.failureStage +
                                                 " accepted=" + std::to_string(candidateReport.accepted) +
                                                 " score=" + std::to_string(candidateReport.score) +
                                                 " reason=" + reason);
                for (const std::string &evidence : candidateReport.evidence)
                {
                    const bool important = evidence.find("resolved ") != std::string::npos ||
                                           evidence.find("failed") != std::string::npos ||
                                           evidence.find("rejected") != std::string::npos ||
                                           evidence.find("candidate") != std::string::npos ||
                                           evidence.find("sample eligibility") != std::string::npos ||
                                           evidence.find("parameter chain") != std::string::npos ||
                                           evidence.find("case-preserving") != std::string::npos ||
                                           evidence.find("property subtype") != std::string::npos ||
                                           evidence.find("delegate subtype") != std::string::npos;
                    if (important)
                        Note(RuntimeLogLevel::Debug, "schema_evidence id=" + profile.id + " " + evidence);
                }
                schemaSelection.candidates.push_back(std::move(candidateReport));
                candidateSchemas.push_back(std::move(candidateSchema));
            }

            schemaSelection = ::anduefker::ue::SelectSchemaCandidates(std::move(schemaSelection.candidates), candidateSchemas);
        };

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            resolveProfiles();
            bool addressSpaceChanged = false;
            for (const SchemaCandidateSummary &candidate : schemaSelection.candidates)
                addressSpaceChanged = addressSpaceChanged || candidate.addressSpaceChanged;
            if (!addressSpaceChanged || attempt != 0)
                break;
            Note(RuntimeLogLevel::Warning, "Remote address space changed during schema resolution; refreshing and retrying");
            if (!memory_->RefreshAddressSpace())
            {
                Note(RuntimeLogLevel::Error, "remote address-space refresh failed after schema probe invalidation");
                break;
            }
            schemaBootstrap = CreateSchemaProbeBootstrap(*memory_, context_.Binding());
            if (!schemaBootstrap->IsValid())
            {
                Note(RuntimeLogLevel::Error, schemaBootstrap->failure.empty()
                                                 ? "schema bootstrap could not be recreated after address-space refresh"
                                                 : schemaBootstrap->failure);
                break;
            }
        }

        const bool schemaAccepted = schemaSelection.accepted;
        if (schemaAccepted)
        {
            schema = std::move(candidateSchemas[schemaSelection.selectedIndex]);
            schemaReport = schemaSelection.candidates[schemaSelection.selectedIndex];
            selectedProfile = profiles[schemaSelection.selectedIndex];
            if (schemaSelection.ambiguous)
            {
                std::string equivalentProfiles;
                for (const SchemaCandidateSummary &candidate : schemaSelection.candidates)
                {
                    if (candidate.accepted && candidate.score == schemaReport.score)
                        equivalentProfiles += " " + candidate.profileId;
                }
                const std::string evidence = "equivalent resolved layouts; selected=" + selectedProfile.id +
                                             " compatible_profiles=" + equivalentProfiles +
                                             "; exact engine version is not identified by layout alone";
                schemaReport.evidence.push_back(evidence);
                Note(RuntimeLogLevel::Debug, evidence);
            }
        }
        else
        {
            schemaReport.profileId = "none";
            schemaReport.failureStage = "selection";
            schemaReport.failures.push_back(schemaSelection.layoutAmbiguous
                                                ? "schema selection is ambiguous across distinct layout variants"
                                                : "schema selection failed: no profile satisfied semantic validation");
            for (const SchemaCandidateSummary &candidate : schemaSelection.candidates)
            {
                const std::string attribution = "profile=" + candidate.profileId +
                                                " stage=" + candidate.failureStage + " ";
                for (const std::string &evidence : candidate.evidence)
                    schemaReport.evidence.push_back(attribution + evidence);
                for (const std::string &failure : candidate.failures)
                    schemaReport.failures.push_back(attribution + failure);
            }
        }
        if (!schemaAccepted)
        {
            const std::string failure = schemaReport.failures.empty()
                                            ? "schema resolution failed without a diagnostic"
                                            : schemaReport.failures.front();
            Note(RuntimeLogLevel::Error, failure);
            for (size_t index = 1; index < schemaReport.failures.size(); ++index)
                Note(RuntimeLogLevel::Error, schemaReport.failures[index]);
        }
        const ReadStats &memoryStats = memory_->Stats();
        Note(RuntimeLogLevel::Debug, "memory stats operations=" + std::to_string(memoryStats.operations) +
                                         " requested_bytes=" + std::to_string(memoryStats.requestedBytes) +
                                         " transferred_bytes=" + std::to_string(memoryStats.transferredBytes) +
                                         " failures=" + std::to_string(memoryStats.failures));
        if (!schemaAccepted)
        {
            failures_.insert(failures_.end(), schemaReport.failures.begin(), schemaReport.failures.end());
            return RuntimeSessionStatus::BindingReady;
        }
        context_.CommitSchema(std::move(schema));
        Note(RuntimeLogLevel::Info, "Engine schema resolved; profile=" + selectedProfile.id +
                                        " range=" + selectedProfile.versionRange +
                                        " source=runtime-schema-probe");

        if (!memory_->RefreshAddressSpace())
        {
            failures_.push_back("address-space refresh failed before reflection capture");
            Note(RuntimeLogLevel::Error, failures_.back());
            return RuntimeSessionStatus::SchemaReady;
        }
        Note(RuntimeLogLevel::Info, "Collecting common object classes...");
        binding::CommonObjectCollector collector(*memory_, context_.Binding(), context_.Schema());
        std::vector<binding::CommonObjectInfo> commonObjects = collector.Collect();
        Note(RuntimeLogLevel::Info, "Found " + std::to_string(commonObjects.size()) + " common object classes");
        for (const auto &obj : commonObjects)
        {
            Note(RuntimeLogLevel::Debug, "common_object: " + obj.name +
                                             " address=" + HexAddress(obj.address) +
                                             " index=" + std::to_string(obj.index));
        }

        RuntimeBinding updatedBinding = context_.Binding();
        updatedBinding.commonObjects = std::move(commonObjects);
        context_.CommitBinding(std::move(updatedBinding));

        const ReadStats beforeReflection = memory_->Stats();
        ReflectionReader reader(*memory_, context_.Binding(), context_.Schema(),
                                context_.Module().base, context_.Module().end);
        reflection_ = reader.Read();
        const ReadStats afterReflection = memory_->Stats();
        Note(RuntimeLogLevel::Debug, "memory stats stage=reflection operations=" + std::to_string(afterReflection.operations - beforeReflection.operations) +
                                         " requested_bytes=" + std::to_string(afterReflection.requestedBytes - beforeReflection.requestedBytes) +
                                         " transferred_bytes=" + std::to_string(afterReflection.transferredBytes - beforeReflection.transferredBytes) +
                                         " failures=" + std::to_string(afterReflection.failures - beforeReflection.failures));
        Note(RuntimeLogLevel::Info, "Capture observations_stable=" + std::to_string(reflection_.capture.observationsStable) +
                                        " attempts=" + std::to_string(reflection_.capture.attempts) +
                                        " observed_ranges=" + std::to_string(reflection_.capture.observedRanges) +
                                        " changed_ranges=" + std::to_string(reflection_.capture.changedRanges) +
                                        " unreadable_ranges=" + std::to_string(reflection_.capture.unreadableRanges) +
                                        " limit_exceeded=" + std::to_string(reflection_.capture.limitExceeded) + " atomic_snapshot=0");
        reflection_.diagnostics.push_back("reflection status=" + std::string(ParseStatusName(reflection_.status)));
        if (reflection_.stats.failures != 0)
            reflection_.diagnostics.push_back("reflection read failures=" + std::to_string(reflection_.stats.failures));
        if (reflection_.stats.unknownProperties != 0)
            reflection_.diagnostics.push_back("unknown properties=" + std::to_string(reflection_.stats.unknownProperties));
        if (reflection_.stats.unresolvedTypeDetails != 0)
            reflection_.diagnostics.push_back("unresolved type details=" + std::to_string(reflection_.stats.unresolvedTypeDetails));
        if (reflection_.stats.layoutConflicts != 0)
            reflection_.diagnostics.push_back("layout conflicts=" + std::to_string(reflection_.stats.layoutConflicts));
        for (const std::string &diagnostic : reflection_.diagnostics)
            Note(RuntimeLogLevel::Debug, "reflection: " + diagnostic);
        Note(RuntimeLogLevel::Info, "Reflection status=" + std::to_string(static_cast<int>(reflection_.status)) +
                                        " types=" + std::to_string(reflection_.stats.parsedTypes) +
                                        " properties=" + std::to_string(reflection_.stats.parsedProperties) +
                                        " functions=" + std::to_string(reflection_.stats.parsedFunctions) +
                                        " enums=" + std::to_string(reflection_.stats.parsedEnums) +
                                        " unknown_properties=" + std::to_string(reflection_.stats.unknownProperties) +
                                        " unresolved_type_details=" + std::to_string(reflection_.stats.unresolvedTypeDetails) +
                                        " object_slots=" + std::to_string(reflection_.stats.objectSlots) +
                                        " valid_objects=" + std::to_string(reflection_.stats.validObjects) +
                                        " skipped_objects=" + std::to_string(reflection_.stats.skippedObjects) +
                                        " empty_object_slots=" + std::to_string(reflection_.stats.emptyObjectSlots) +
                                        " object_read_failures=" + std::to_string(reflection_.stats.objectReadFailures) +
                                        " class_name_read_failures=" + std::to_string(reflection_.stats.classNameReadFailures) +
                                        " enum_read_failures=" + std::to_string(reflection_.stats.enumReadFailures) +
                                        " identity_failures=" + std::to_string(reflection_.stats.identityFailures) +
                                        " unvisited_objects=" + std::to_string(reflection_.stats.unvisitedObjects) +
                                        " skipped_class_default_objects=" + std::to_string(reflection_.stats.skippedClassDefaultObjects) +
                                        " skipped_incomplete_objects=" + std::to_string(reflection_.stats.skippedIncompleteObjects) +
                                        " object_diagnostic_samples_omitted=" + std::to_string(reflection_.stats.objectDiagnosticSamplesOmitted) +
                                        " failures=" + std::to_string(reflection_.stats.failures));
        if (reflection_.status == ParseStatus::Failed)
        {
            return RuntimeSessionStatus::SchemaReady;
        }
        if (reflection_.status == ParseStatus::Partial)
            Note(RuntimeLogLevel::Warning, "Reflection is partial; output will be written to a .partial artifact");
        if (!config_.outputRoot.empty())
        {
            ArtifactWriter writer(context_, reflection_, config_.outputRoot, config_.packageName);
            artifacts_ = writer.Write();
            for (const auto &diagnostic : artifacts_.generationDiagnostics)
                Note(RuntimeLogLevel::Debug, diagnostic);
            if (artifacts_.status == ParseStatus::Failed)
            {
                failures_.push_back(artifacts_.error);
                Note(RuntimeLogLevel::Error, failures_.back());
                return RuntimeSessionStatus::Failed;
            }
            Note(RuntimeLogLevel::Info, "Artifact status=" + std::string(ParseStatusName(artifacts_.status)) +
                                            " files=" + std::to_string(artifacts_.filesWritten) +
                                            " opaque_fields=" + std::to_string(artifacts_.opaqueFields) +
                                            " reflection_status=" + ParseStatusName(artifacts_.reflectionStatus) +
                                            " sdk_status=" + ParseStatusName(artifacts_.sdkStatus) +
                                            " omitted_fields=" + std::to_string(artifacts_.omittedFields) +
                                            " layout_warnings=" + std::to_string(artifacts_.layoutWarnings) +
                                            " output=" + artifacts_.outputPath.string());
        }
        Note(RuntimeLogLevel::Info, "=== Runtime Session Completed ===");
        if (reflection_.status == ParseStatus::Partial)
            return RuntimeSessionStatus::ReflectionPartial;
        if (artifacts_.status == ParseStatus::Partial)
            return RuntimeSessionStatus::ArtifactPartial;
        return RuntimeSessionStatus::ReflectionReady;
    }
} // namespace anduefker::app
