#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "anduefker/ue/EngineVersion.hpp"
#include "anduefker/app/RuntimeSession.hpp"

namespace
{
    void PrintUsage(const char *program)
    {
        std::fprintf(stderr, "Usage: %s <output-root> <package> [pid] [options]\n", program);
        std::fprintf(stderr, "\nOptions:\n");
        std::fprintf(stderr, "  --ue-version <version>    UE version, for example 5.6 (default: 5.6)\n");
        std::fprintf(stderr, "  --help                    Show this help\n");
    }

    bool ParsePid(const std::string &text, int &pid)
    {
        char *end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || value <= 0 || value > 0x7FFFFFFF)
            return false;
        pid = static_cast<int>(value);
        return true;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    anduefker::app::RuntimeSessionConfig config;
    config.outputRoot = argv[1];
    config.packageName = argv[2];
    config.engineVersion = anduefker::ue::ParseEngineVersion("5.6");

    for (int index = 3; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argument == "--ue-version")
        {
            if (++index >= argc)
            {
                std::fprintf(stderr, "--ue-version requires a value\n");
                return 2;
            }
            config.engineVersion = anduefker::ue::ParseEngineVersion(argv[index]);
            if (!config.engineVersion.IsValid())
            {
                std::fprintf(stderr, "Invalid UE version: %s\n", argv[index]);
                return 2;
            }
            continue;
        }
        if (config.pid != 0 || !ParsePid(argument, config.pid))
        {
            std::fprintf(stderr, "Unexpected argument: %s\n", argument.c_str());
            return 2;
        }
    }

    anduefker::app::RuntimeSession session(std::move(config));
    const auto status = session.Run();
    std::printf("Runtime status: %d\n", static_cast<int>(status));
    if (!session.LogPath().empty())
        std::printf("Log: %s\n", session.LogPath().c_str());
    for (const auto &entry : session.LogEntries())
    {
        if (entry.level == anduefker::app::RuntimeLogLevel::Debug)
            continue;
        const char *level = entry.level == anduefker::app::RuntimeLogLevel::Error
                                ? "ERROR"
                            : entry.level == anduefker::app::RuntimeLogLevel::Warning ? "WARN"
                                                                                      : "INFO";
        std::printf("[%s] %s\n", level, entry.message.c_str());
    }
    if (!session.Failures().empty())
    {
        std::printf("Failures:\n");
        for (const std::string &failure : session.Failures())
            std::printf("  %s\n", failure.c_str());
    }
    if (!session.Artifacts().outputPath.empty())
        std::printf("Output: %s\n", session.Artifacts().outputPath.string().c_str());
    if (status == anduefker::app::RuntimeSessionStatus::ReflectionReady)
        return 0;
    if (status == anduefker::app::RuntimeSessionStatus::ReflectionPartial)
        return 3;
    return 1;
}
