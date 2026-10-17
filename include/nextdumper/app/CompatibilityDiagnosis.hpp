#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nextdumper::app
{
    enum class CompatibilityStatus
    {
        NotEvaluated,
        BaselineCompatible,
        ProfileSelected,
        ProfileValidated,
        NeedsInvestigation,
        ProfileValidationFailed,
        Inconclusive,
    };

    enum class DiagnosisDomain
    {
        Runtime,
        Compatibility,
        Profile,
    };

    [[nodiscard]] const char *CompatibilityStatusName(CompatibilityStatus status);
    [[nodiscard]] const char *DiagnosisDomainName(DiagnosisDomain domain);

    struct DiagnosisFinding
    {
        DiagnosisDomain domain = DiagnosisDomain::Compatibility;
        std::string code;
        std::string stage;
        std::string confidence = "observed";
        std::string summary;
        std::string nextAction;
    };

    struct CompatibilityDiagnosis
    {
        static constexpr size_t kMaxFindings = 64;

        std::string sessionStatus = "running";
        CompatibilityStatus status = CompatibilityStatus::NotEvaluated;
        std::string stage = "startup";
        std::string packageName;
        std::string moduleName;
        std::string architecture = "unknown";
        unsigned int pointerWidth = 0;
        std::string profileId = "none";
        std::string profileStatus = "not-evaluated";
        std::vector<DiagnosisFinding> findings;
        size_t omittedFindings = 0;

        void Add(DiagnosisFinding finding);
        [[nodiscard]] std::string ToJson() const;
    };
} // namespace nextdumper::app
