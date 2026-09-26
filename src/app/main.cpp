#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "anduefker/ue/EngineVersion.hpp"
#include "anduefker/app/RuntimeSession.hpp"

namespace
{
    void PrintUsage(const char *program)
    {
        std::fprintf(stderr, "Usage: %s -o <output-dir> -p <package-name> [options]\n", program);
        std::fprintf(stderr, "\nRequired arguments:\n");
        std::fprintf(stderr, "  -o, --output <dir>        Output directory for generated SDK\n");
        std::fprintf(stderr, "  -p, --package <name>      Target package name (e.g., com.example.app)\n");
        std::fprintf(stderr, "\nOptional arguments:\n");
        std::fprintf(stderr, "  --pid <pid>               Target process ID (auto-detected if omitted)\n");
        std::fprintf(stderr, "  --ue-version <version>    UE version (default: 5.6)\n");
        std::fprintf(stderr, "  -h, --help                Show this help message\n");
        std::fprintf(stderr, "\nExample:\n");
        std::fprintf(stderr, "  %s -o ./output -p com.YS.Nicecity\n", program);
        std::fprintf(stderr, "  %s -o ./output -p com.YS.Nicecity --pid 12345 --ue-version 5.6\n", program);
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
    anduefker::app::RuntimeSessionConfig config;
    config.engineVersion = anduefker::ue::ParseEngineVersion("5.6");

    bool hasOutput = false;
    bool hasPackage = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];

        if (arg == "-h" || arg == "--help")
        {
            PrintUsage(argv[0]);
            return 0;
        }

        if (arg == "-o" || arg == "--output")
        {
            if (++index >= argc)
            {
                std::fprintf(stderr, "Error: %s requires an argument\n", arg.c_str());
                PrintUsage(argv[0]);
                return 2;
            }
            config.outputRoot = argv[index];
            hasOutput = true;
            continue;
        }

        if (arg == "-p" || arg == "--package")
        {
            if (++index >= argc)
            {
                std::fprintf(stderr, "Error: %s requires an argument\n", arg.c_str());
                PrintUsage(argv[0]);
                return 2;
            }
            config.packageName = argv[index];
            hasPackage = true;
            continue;
        }

        if (arg == "--pid")
        {
            if (++index >= argc)
            {
                std::fprintf(stderr, "Error: --pid requires an argument\n");
                PrintUsage(argv[0]);
                return 2;
            }
            if (!ParsePid(argv[index], config.pid))
            {
                std::fprintf(stderr, "Error: Invalid PID: %s\n", argv[index]);
                return 2;
            }
            continue;
        }

        if (arg == "--ue-version")
        {
            if (++index >= argc)
            {
                std::fprintf(stderr, "Error: --ue-version requires an argument\n");
                PrintUsage(argv[0]);
                return 2;
            }
            config.engineVersion = anduefker::ue::ParseEngineVersion(argv[index]);
            if (!config.engineVersion.IsValid())
            {
                std::fprintf(stderr, "Error: Invalid UE version: %s\n", argv[index]);
                return 2;
            }
            continue;
        }

        std::fprintf(stderr, "Error: Unknown argument: %s\n", arg.c_str());
        PrintUsage(argv[0]);
        return 2;
    }

    if (!hasOutput || !hasPackage)
    {
        std::fprintf(stderr, "Error: Missing required arguments\n\n");
        PrintUsage(argv[0]);
        return 2;
    }

    anduefker::app::RuntimeSession session(std::move(config));
    const auto status = session.Run();

    std::printf("\n=== Session Summary ===\n");
    std::printf("Status: %d\n", static_cast<int>(status));

    if (!session.LogPath().empty())
        std::printf("Log: %s\n", session.LogPath().c_str());

    if (!session.Failures().empty())
    {
        std::printf("\nFailures:\n");
        for (const std::string &failure : session.Failures())
            std::printf("  - %s\n", failure.c_str());
    }

    if (!session.Artifacts().outputPath.empty())
        std::printf("\nOutput: %s\n", session.Artifacts().outputPath.string().c_str());

    if (status == anduefker::app::RuntimeSessionStatus::ReflectionReady)
        return 0;
    if (status == anduefker::app::RuntimeSessionStatus::ReflectionPartial)
        return 3;
    return 1;
}
