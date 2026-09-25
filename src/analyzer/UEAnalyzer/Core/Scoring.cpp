#include "Scoring.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace anduefker::analyzer
{

	namespace Scoring
	{

		float ScoreAccessCount(uint32_t Count)
		{
			if (Count == 0)
				return 0.0f;
			// log2(4096) = 12: an access count in the low thousands already
			// saturates the signal, which is what a hot UE global looks like.
			constexpr float kLogScale = 12.0f;
			const float V = std::log2(static_cast<float>(Count) + 1.0f) / kLogScale;
			return std::min(V, 1.0f);
		}

		float ScoreAccessRank(uint32_t Rank)
		{
			if (Rank == 0)
				return 0.0f;

			// A fitted piecewise curve over the corpus's access-rank distribution:
			// full score inside the top tier, then two widening bands of decay, then
			// a slow floor-bound taper for the long tail. The break points and decay
			// rates are fitted, not derived; verify_truths is what re-checks them
			// as the corpus grows.
			constexpr uint32_t kTopTier = 16;	 ///< Full score through this rank.
			constexpr uint32_t kMidTier = 150;	 ///< End of the first decay band.
			constexpr uint32_t kLongTail = 2000; ///< End of the second decay band.
			constexpr float kMidTierFloor = 0.65f;
			constexpr float kMidTierDecay = 0.35f;
			constexpr float kLongTailFloor = 0.10f;
			constexpr float kLongTailDecay = 0.55f;
			constexpr float kTailTaperPerRank = 1e-5f;

			if (Rank <= kTopTier)
				return 1.0f;
			if (Rank <= kMidTier)
			{
				const float T = static_cast<float>(Rank - kTopTier) / static_cast<float>(kMidTier - kTopTier);
				return 1.0f - kMidTierDecay * T;
			}
			if (Rank <= kLongTail)
			{
				const float T = static_cast<float>(Rank - kMidTier) / static_cast<float>(kLongTail - kMidTier);
				return kMidTierFloor - kLongTailDecay * T;
			}
			return std::max(0.0f, kLongTailFloor - static_cast<float>(Rank - kLongTail) * kTailTaperPerRank);
		}

		void SortByConfidence(std::vector<Candidate> &Candidates)
		{
			std::stable_sort(Candidates.begin(), Candidates.end(), [](const Candidate &A, const Candidate &B)
							 {
				if (A.Confidence != B.Confidence)
					return A.Confidence > B.Confidence;
				return A.Address < B.Address; });
		}

		std::vector<Candidate> FuseCandidates(std::vector<Candidate> All, const ScoringWeights &W)
		{
			(void)W;
			std::unordered_map<uintptr_t, Candidate> Merged;
			Merged.reserve(All.size());

			for (auto &C : All)
			{
				auto It = Merged.find(C.Address);
				if (It == Merged.end())
				{
					Merged.emplace(C.Address, std::move(C));
					continue;
				}

				Candidate &Dst = It->second;
				if (Dst.Method != C.Method)
					Dst.Method += "+" + C.Method;

				// Strategies already produced a fitted confidence; agreement between two
				// of them should not dilute it, so the stronger one is kept.
				Dst.Confidence = std::max(Dst.Confidence, C.Confidence);

				// Union of signals: a signal already present is not counted twice, but a
				// new one from an independent strategy is real added evidence.
				for (auto &S : C.Signals)
				{
					if (!Dst.Has(S.Name))
						Dst.Signals.push_back(std::move(S));
				}
			}

			std::vector<Candidate> Out;
			Out.reserve(Merged.size());
			for (auto &[Addr, C] : Merged)
			{
				(void)Addr;
				Out.push_back(std::move(C));
			}

			SortByConfidence(Out);
			return Out;
		}

	} // namespace Scoring

} // namespace anduefker::analyzer
