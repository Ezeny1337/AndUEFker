#pragma once

#include <memory>

#include "StrategyBase.h"

namespace anduefker::analyzer
{

	/**
	 * @brief GUObjectArray - the FUObjectArray that holds the object array.
	 *
	 * A separate target from ObjObjects, not another name for it. The two are used
	 * differently and that difference is what makes both worth locating: the array
	 * is reached through pointer-width reads, while the container's address is taken
	 * and passed as `this`.
	 *
	 * The container is the more robust of the two on obfuscated builds, where it
	 * can be referenced orders of magnitude more than the array inside it -
	 * exactly the case where locating the member on its own evidence fails.
	 */
	class GUObjectArrayStrategy final : public StrategyBase
	{
	public:
		const char *Name() const override { return "GUObjectArray"; }

		std::span<const AnchorString> ProximityAnchors() const override;
		std::span<const Anchor> ResolutionAnchors() const override;

		/// FUObjectArray: named by code, pointer-aligned, and holding an
		/// ObjObjects-shaped table as an interior member rather than at +0.
		StructureEvidence Verify(const StructureVerifier &Verifier, uint64_t Address) const override;

		/// Weights tuning this strategy. Starting values rather than fitted ones.
		struct FWeights
		{
			float TableHeat = 1.0f;	  ///< How hard the held table is worked.
			float NamedByCode = 1.0f; ///< How often the container is named.
			float AccessRank = 2.0f;
			float AnchorProximity = 3.0f;
			float ScoreScale = 40.0f;
		};

		FWeights Weights;

		std::vector<Candidate> Score(const AnalysisContext &Ctx) const override;
	};

	/// Creates the strategy.
	///
	/// Returns the concrete type so callers can set Weights directly; it converts
	/// to StrategyPtr wherever only the interface is needed.
	std::unique_ptr<GUObjectArrayStrategy> MakeGUObjectArrayStrategy();

} // namespace anduefker::analyzer
