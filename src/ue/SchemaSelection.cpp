#include "anduefker/ue/SchemaResolver.hpp"

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
        int32_t bestScore = std::numeric_limits<int32_t>::min();
        for (size_t index = 0; index < result.candidates.size(); ++index)
        {
            const auto &candidate = result.candidates[index];
            if (!candidate.accepted)
                continue;
            if (!result.accepted || candidate.score > bestScore)
            {
                result.accepted = true;
                result.ambiguous = false;
                result.layoutAmbiguous = false;
                result.selectedIndex = index;
                bestScore = candidate.score;
            }
            else if (candidate.score == bestScore)
            {
                result.ambiguous = true;
                if (!schemas[index].HasSameReflectionLayout(schemas[result.selectedIndex]))
                    result.layoutAmbiguous = true;
            }
        }
        if (result.accepted && result.layoutAmbiguous)
        {
            result.accepted = false;
            result.candidates[result.selectedIndex].accepted = false;
            result.candidates[result.selectedIndex].failures.push_back("schema selection is ambiguous across distinct layout variants");
        }
        return result;
    }
} // namespace anduefker::ue
