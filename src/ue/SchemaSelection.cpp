#include "anduefker/ue/SchemaResolver.hpp"

#include <algorithm>
#include <limits>

namespace anduefker::ue
{
    SchemaSelectionResult SelectSchemaCandidates(std::vector<SchemaCandidateSummary> candidates,
                                                 const std::vector<EngineSchema> &schemas)
    {
        SchemaSelectionResult result;
        result.candidates = std::move(candidates);
        if (schemas.size() != result.candidates.size())
            return result;
        int32_t bestLayoutScore = std::numeric_limits<int32_t>::min();
        for (const auto &candidate : result.candidates)
        {
            if (candidate.accepted)
                bestLayoutScore = std::max(bestLayoutScore, candidate.layoutScore);
        }
        if (bestLayoutScore == std::numeric_limits<int32_t>::min())
            return result;

        std::vector<size_t> layoutCandidates;
        for (size_t index = 0; index < result.candidates.size(); ++index)
        {
            const auto &candidate = result.candidates[index];
            if (candidate.accepted && candidate.layoutScore == bestLayoutScore)
                layoutCandidates.push_back(index);
        }
        for (size_t index : layoutCandidates)
        {
            const auto &candidate = result.candidates[index];
            if (!result.accepted || candidate.versionEvidenceScore > result.candidates[result.selectedIndex].versionEvidenceScore)
            {
                result.accepted = true;
                result.ambiguous = false;
                result.layoutAmbiguous = false;
                result.selectedIndex = index;
                result.selectionReason = candidate.versionEvidenceScore == 0 ? "layout-score" : "version-evidence-tiebreak";
                result.versionConfidence = candidate.versionEvidenceScore >= 5 ? "strong-evidence-not-proof" :
                                           candidate.versionEvidenceScore > 0   ? "medium"
                                                                                : "layout-only";
            }
        }

        if (layoutCandidates.size() > 1)
        {
            int32_t minimumVersionScore = std::numeric_limits<int32_t>::max();
            int32_t maximumVersionScore = std::numeric_limits<int32_t>::min();
            for (const size_t index : layoutCandidates)
            {
                minimumVersionScore = std::min(minimumVersionScore, result.candidates[index].versionEvidenceScore);
                maximumVersionScore = std::max(maximumVersionScore, result.candidates[index].versionEvidenceScore);
            }
            result.selectionReason = maximumVersionScore != minimumVersionScore ? "version-evidence-tiebreak"
                                                                                 : "layout-equivalent-tie";
            for (const size_t index : layoutCandidates)
            {
                if (!schemas[index].HasSameReflectionLayout(schemas[result.selectedIndex]))
                {
                    result.layoutAmbiguous = true;
                    break;
                }
            }
        }

        if (!result.accepted)
            return result;

        for (size_t index = 0; index < result.candidates.size(); ++index)
        {
            const auto &candidate = result.candidates[index];
            if (!candidate.accepted || !schemas[index].HasSameReflectionLayout(schemas[result.selectedIndex]))
                continue;
            result.compatibleIndices.push_back(index);
        }
        if (result.accepted && result.layoutAmbiguous)
        {
            result.accepted = false;
            result.candidates[result.selectedIndex].accepted = false;
            result.candidates[result.selectedIndex].failures.push_back("schema selection is ambiguous across distinct layout variants");
        }
        result.ambiguous = result.compatibleIndices.size() > 1;
        return result;
    }
} // namespace anduefker::ue
