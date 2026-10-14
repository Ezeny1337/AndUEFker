#include "anduefker/app/CompatibilityDiagnosis.hpp"

#include <sstream>
#include <utility>

namespace anduefker::app
{
    namespace
    {
        std::string EscapeJson(const std::string &value)
        {
            static constexpr char hex[] = "0123456789abcdef";
            std::string escaped;
            escaped.reserve(value.size());
            for (const unsigned char ch : value)
            {
                switch (ch)
                {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        escaped += "\\u00";
                        escaped += hex[ch >> 4];
                        escaped += hex[ch & 0x0f];
                    }
                    else
                        escaped += static_cast<char>(ch);
                    break;
                }
            }
            return escaped;
        }
    } // namespace

    const char *CompatibilityStatusName(CompatibilityStatus status)
    {
        switch (status)
        {
        case CompatibilityStatus::NotEvaluated:
            return "not-evaluated";
        case CompatibilityStatus::BaselineCompatible:
            return "baseline-compatible";
        case CompatibilityStatus::ProfileSelected:
            return "profile-selected";
        case CompatibilityStatus::ProfileValidated:
            return "profile-validated";
        case CompatibilityStatus::NeedsInvestigation:
            return "needs-investigation";
        case CompatibilityStatus::ProfileValidationFailed:
            return "profile-validation-failed";
        case CompatibilityStatus::Inconclusive:
            return "inconclusive";
        }
        return "inconclusive";
    }

    const char *DiagnosisDomainName(DiagnosisDomain domain)
    {
        switch (domain)
        {
        case DiagnosisDomain::Runtime:
            return "runtime";
        case DiagnosisDomain::Compatibility:
            return "compatibility";
        case DiagnosisDomain::Profile:
            return "profile";
        }
        return "compatibility";
    }

    void CompatibilityDiagnosis::Add(DiagnosisFinding finding)
    {
        if (findings.size() >= kMaxFindings)
        {
            ++omittedFindings;
            return;
        }
        findings.push_back(std::move(finding));
    }

    std::string CompatibilityDiagnosis::ToJson() const
    {
        std::ostringstream stream;
        stream << "{\n"
               << "  \"schema_version\": 1,\n"
               << "  \"session_status\": \"" << EscapeJson(sessionStatus) << "\",\n"
               << "  \"compatibility_status\": \"" << CompatibilityStatusName(status) << "\",\n"
               << "  \"stage\": \"" << EscapeJson(stage) << "\",\n"
               << "  \"package\": \"" << EscapeJson(packageName) << "\",\n"
               << "  \"module\": \"" << EscapeJson(moduleName) << "\",\n"
               << "  \"architecture\": \"" << EscapeJson(architecture) << "\",\n"
               << "  \"pointer_width\": " << pointerWidth << ",\n"
               << "  \"profile_id\": \"" << EscapeJson(profileId) << "\",\n"
               << "  \"profile_status\": \"" << EscapeJson(profileStatus) << "\",\n"
               << "  \"findings_omitted\": " << omittedFindings << ",\n"
               << "  \"findings\": [";
        for (size_t index = 0; index < findings.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            const DiagnosisFinding &finding = findings[index];
            stream << "\n    {\"domain\":\"" << DiagnosisDomainName(finding.domain)
                   << "\",\"code\":\"" << EscapeJson(finding.code)
                   << "\",\"stage\":\"" << EscapeJson(finding.stage)
                   << "\",\"confidence\":\"" << EscapeJson(finding.confidence)
                   << "\",\"summary\":\"" << EscapeJson(finding.summary)
                   << "\",\"next_action\":\"" << EscapeJson(finding.nextAction) << "\"}";
        }
        if (!findings.empty())
            stream << '\n'
                   << "  ";
        stream << "]\n}\n";
        return stream.str();
    }
} // namespace anduefker::app
