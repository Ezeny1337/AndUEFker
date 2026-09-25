#pragma once

#include <cstddef>
#include <vector>

#include "Candidate.h"

namespace anduefker::analyzer
{

	/// How many scored candidates a strategy keeps before trimming.
	///
	/// Beyond this the tail is noise for a query. Diagnostics that must see past it
	/// set AnalysisContext::MaxScored to zero.
	constexpr size_t kMaxScoredCandidates = 256;

	/// Scoring values that belong to the analyzer rather than to any one target.
	///
	/// Per-target weights are owned by the strategy they tune, not listed here, so
	/// adding a strategy needs no edit to this type. To tune one, construct it
	/// through its own factory and set the typed members of its Weights.
	struct ScoringWeights
	{
		/// Confidence a candidate must reach to be offered as an answer.
		float MinConfidence = 0.45f;

		/// Bonus for a candidate whose structure a live-memory validator confirmed.
		/// Applies only when the caller supplied a CandidateValidator.
		float StructureValid = 0.45f;

		static ScoringWeights Default() { return {}; }
	};

	/**
	 * @brief The scoring machinery the strategies share.
	 *
	 * Namespaced rather than loose in UEAnalyzerKitty: these are internals a caller of the
	 * library has no reason to reach for, and grouping them says so.
	 */
	namespace Scoring
	{

		/**
		 * @brief Diminishing-returns curve for a raw reference count.
		 *
		 * ObjObjects is among the most referenced globals in a UE binary, but a global
		 * with 4000 references is not four times better evidence than one with 1000.
		 *
		 * @return A score in [0,1].
		 */
		float ScoreAccessCount(uint32_t Count);

		/**
		 * @brief Score from a global's access-count rank within its own binary.
		 *
		 * Rank travels across binaries in a way raw counts do not: the object array
		 * consistently lands well within the most-referenced globals, so the curve
		 * is shaped to stay high through the first ~150 ranks and fall away after
		 * that.
		 *
		 * @param Rank 1 = most referenced; 0 means "not ranked" and scores nothing.
		 * @return A score in [0,1].
		 */
		float ScoreAccessRank(uint32_t Rank);

		/// Sorts descending by confidence, then ascending by address for determinism.
		void SortByConfidence(std::vector<Candidate> &Candidates);

		/**
		 * @brief Merges candidates for the same address found by different strategies.
		 *
		 * Agreement between independent strategies is itself evidence, so the merged
		 * candidate keeps the union of signals and records both method names.
		 *
		 * @param All Candidates from every strategy, in any order.
		 * @param W   Scoring model; currently consulted only for future tuning hooks.
		 * @return One candidate per address, best first.
		 */
		std::vector<Candidate> FuseCandidates(std::vector<Candidate> All, const ScoringWeights &W);

	} // namespace Scoring

	/**
	 * @brief Result of locating one global.
	 *
	 * The same type comes back whatever the memory backend was, so callers handle a
	 * file image and a live process identically.
	 */
	struct LocateResult
	{
		/// Which global this answers for, named by the strategy that produced it.
		const char *Target = "";
		std::vector<Candidate> Candidates; ///< Best first; empty when nothing was found.

		/// Which mechanism produced the winning candidate, for reporting and for
		/// --method diagnosis.
		EFindMethod FindMethod = EFindMethod::Unknown;

		/// True when these came from a tier that verifies its answers. When false the
		/// entries are leads for a human, not results, and must be labelled as such.
		bool bVerified = false;

		/// Highest-confidence candidate, or nullptr when nothing was found.
		const Candidate *Best() const { return Candidates.empty() ? nullptr : &Candidates.front(); }

		uintptr_t BestAddress() const { return Best() ? Best()->Address : 0; }

		/// True when the best candidate cleared the confidence gate.
		bool IsConfident(const ScoringWeights &W) const
		{
			const Candidate *B = Best();
			return B && B->IsConfident(W.MinConfidence);
		}
	};

} // namespace anduefker::analyzer
