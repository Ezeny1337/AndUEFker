#pragma once

#include <memory>

#include "StrategyBase.h"

namespace anduefker::analyzer
{

	/**
	 * @brief The name table - FNamePool / NamePoolData, or legacy GNames.
	 *
	 * One strategy covers both structures. They score the same candidates, and the
	 * only thing that would separate them is an engine-version prior - a claim
	 * this tool has no way to check. Which one an address reads as is reported
	 * instead, through StructureEvidence::Layout.
	 *
	 * The name table is much harder to find statically than the object array, and
	 * the scoring reflects that: no single signal separates it. What does show is
	 * a distinctive profile - the table is typically reached at exactly one
	 * offset, its access count sits in a narrow band rather than being large,
	 * and it is referenced from code that also mentions FName's own diagnostic
	 * strings.
	 */
	class NameStrategy final : public StrategyBase
	{
	public:
		const char *Name() const override { return "Names"; }

		std::span<const AnchorString> ProximityAnchors() const override;
		std::span<const Anchor> ResolutionAnchors() const override;

		/// FNamePool / TNameEntryArray: a base symbol, optionally with a block array.
		StructureEvidence Verify(const StructureVerifier &Verifier, uint64_t Address) const override;

		/// Weights tuning this strategy.
		struct FWeights
		{
			/// The table is reached at one offset in nearly every build.
			int MaxOffsets = 2;
			/// Declared but not currently consulted by Score.
			float MinAccesses = 3.0f;
			float AnchorProximity = 3.0f;
			float SingleOffset = 1.0f;
			/// Access count sits in a tight band; distance from it is penalised.
			float CountPeakLog2 = 5.0f;
			float CountBand = 0.3f;
			/// A chunk constant belongs to the object array, so it argues against.
			float ChunkPenalty = 3.0f;
			float WideBonus = 1.0f;
			float ScoreScale = 8.0f;
		};

		FWeights Weights;

		std::vector<Candidate> Score(const AnalysisContext &Ctx) const override;
	};

	/// Creates the strategy.
	///
	/// Returns the concrete type so callers can set Weights directly; it converts
	/// to StrategyPtr wherever only the interface is needed.
	std::unique_ptr<NameStrategy> MakeNameStrategy();

} // namespace anduefker::analyzer
