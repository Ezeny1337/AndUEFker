#pragma once

#include <memory>

#include "StrategyBase.h"

namespace anduefker::analyzer
{

	/**
	 * @brief FUObjectArray - GUObjectArray / ObjObjects.
	 *
	 * One strategy covers both layouts. They score identically - the chunk-constant
	 * signal that distinguishes them is fitted to weight 0 - so which one an
	 * address reads as is reported through StructureEvidence::Layout rather than
	 * decided by an engine-version prior this tool cannot check.
	 *
	 * The scoring shape says something real about the target: the object array is
	 * the global whose accesses are almost entirely pointer-width and which is
	 * touched at only a handful of distinct offsets. Raw reference count barely
	 * matters - several engine singletons are hotter.
	 */
	/// The shape check behind ObjObjectsStrategy::Verify(). A free function, not
	/// only a method, so GUObjectArrayStrategy can build the container's
	/// evidence on top of the array's own - a container holds one, so
	/// everything that makes an array recognisable has to be visible from there
	/// too.
	StructureEvidence VerifyObjectArrayShape(const StructureVerifier &Verifier, uint64_t Address);

	class ObjObjectsStrategy final : public StrategyBase
	{
	public:
		const char *Name() const override { return "ObjObjects"; }

		std::span<const AnchorString> ProximityAnchors() const override;
		std::span<const Anchor> ResolutionAnchors() const override;

		/// See VerifyObjectArrayShape() above.
		StructureEvidence Verify(const StructureVerifier &Verifier, uint64_t Address) const override
		{
			return VerifyObjectArrayShape(Verifier, Address);
		}

		/// Weights tuning this strategy.
		///
		/// Two values are easy to "correct" back into being wrong: Purity dominates
		/// Magnitude, because what marks the array is that nearly all of its accesses
		/// are pointer-width rather than that it is the most-referenced global; and
		/// ChunkConstant is weighted at zero, since it is precise but not reliably
		/// present.
		struct FWeights
		{
			/// Minimum pointer-width accesses before a candidate is considered.
			float MinWideAccesses = 4.0f;
			float Magnitude = 0.5f;	 ///< Weight on log2(pointer-width accesses).
			float Purity = 8.0f;	 ///< Weight on wide / (wide + narrow).
			float OffsetBand = 2.0f; ///< Bonus inside the struct-like offset band.
			int OffsetBandLow = 2;
			int OffsetBandHigh = 6;
			float ChunkConstant = 0.0f;
			float AnchorProximity = 1.0f;
			/// Divides the raw score into [0,1]. Presentation only; ranking depends
			/// on the raw ordering.
			float ScoreScale = 18.0f;
		};

		FWeights Weights;

		std::vector<Candidate> Score(const AnalysisContext &Ctx) const override;
	};

	/// Creates the strategy.
	///
	/// Returns the concrete type so callers can set Weights directly; it converts
	/// to StrategyPtr wherever only the interface is needed.
	std::unique_ptr<ObjObjectsStrategy> MakeObjObjectsStrategy();

} // namespace anduefker::analyzer
