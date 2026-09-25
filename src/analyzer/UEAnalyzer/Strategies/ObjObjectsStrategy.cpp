#include "ObjObjectsStrategy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../Memory/IMemory.h"
#include "../Core/UEConstants.h"

#include <iterator>

namespace anduefker::analyzer
{
	StructureEvidence VerifyObjectArrayShape(const StructureVerifier &Verifier, uint64_t Address)
	{
		StructureEvidence E;

		if (!Verifier.IsPlausibleGlobalLocation(Address))
		{
			E.Verdict = EVerdict::NotWritableData;
			E.Why = EVerdictToString(E.Verdict);
			return E;
		}

		const GlobalAccessHarvester &Harvester = Verifier.Harvester();

		// A global nothing references is not a global. The walk below reads the
		// *neighbourhood* - it accepts any address whose header window happens to
		// contain a hot table - so without this check, an address no instruction
		// computes could still pass by sitting near one that does.
		if (!Harvester.Find(Address))
		{
			E.Verdict = EVerdict::Unreferenced;
			E.Why = EVerdictToString(E.Verdict);
			return E;
		}

		// Walk the header. An access recorded at Address+N is code reading field N of
		// whatever lives here, and the widths say what those fields are: the element
		// or chunk table is a pointer, the counts are 32-bit.
		for (int64_t Off = 0; Off <= kStructHeaderWindow; Off += 4)
		{
			const AccessInfo *Info = Harvester.Find(Address + static_cast<uint64_t>(Off));
			if (!Info)
				continue;

			if (Info->WideAccesses > 0)
			{
				++E.WideFieldCount;
				if (Off == 0)
					E.bPointerAtZero = true;
				// A pointer-width field anywhere in the header counts: the table
				// offset varies across engine versions and reordered forks.
				E.bIndexed = true;
				if (Info->WideAccesses > E.PrimaryFieldAccesses)
				{
					E.PrimaryFieldAccesses = Info->WideAccesses;
					E.PrimaryFieldOffset = Off;
				}
			}
			if (Info->NarrowAccesses > 0)
			{
				++E.NarrowFieldCount;
				E.Fields.push_back(Off);
			}
			if (Info->NearbyConstants.count(kNumElementsPerChunk_4_20) ||
				Info->NearbyConstants.count(kNumElementsPerChunk_4_21))
				E.bChunkConstant = true;
		}

		// The decisive shape: something in the header is read as a pointer, and at
		// least two distinct 32-bit fields sit alongside it. An unrelated global that
		// merely happens to share a function with an anchor has neither.
		if (!E.bIndexed)
		{
			E.Verdict = EVerdict::Rejected;
			E.Why = "no pointer-width field in the header";
			return E;
		}

		// ...and that pointer field must be *hot*. This is the one property of the
		// object array that no fork can change: every UObject lookup in the game
		// goes through the element table, so it is among the most-read globals in
		// the binary.
		//
		// Deliberately not tied to +0: reordered forks move the table to other
		// offsets, so requiring a fixed offset would encode the stock layout and
		// miss them. Hotness is a property of use, not of layout.
		if (E.PrimaryFieldAccesses < Verifier.HotTableThreshold())
		{
			char Buf[128];
			std::snprintf(Buf, sizeof(Buf), "pointer field at +%lld is cold (%u reads; the element table is one of the "
											"hottest globals in the image)",
						  (long long)E.PrimaryFieldOffset,
						  E.PrimaryFieldAccesses);
			E.Why = Buf;
			return E;
		}
		// One 32-bit field is enough - some builds keep only a single element count.
		// The hotness test above carries the weight here; the counts corroborate,
		// they do not decide.
		//
		// Asked only where the answer can distinguish anything. Where a pointer is
		// itself four bytes wide, a count and a table pointer are the same width and
		// are recorded as the same kind of access, so the absence of a 32-bit field
		// says nothing about the layout - it is a property of the target, not of
		// this global. Rejecting on it there would refuse every object array on such
		// a build, which is why the corroborating test is skipped rather than
		// weakened.
		if constexpr (sizeof(uintptr_t) > 4)
		{
			if (E.NarrowFieldCount < 1)
			{
				E.Verdict = EVerdict::Rejected;
				E.Why = "no 32-bit field beside the table; expected the element counts";
				return E;
			}
		}

		// A chunk-size constant in code touching the table means the table indexes
		// chunks. Its absence is weak evidence of a flat array rather than proof,
		// so the fixed reading is offered with correspondingly low confidence.
		E.Layout = E.bChunkConstant ? LayoutClassification{"FChunkedFixedUObjectArray", 0.90f}
									: LayoutClassification{"FFixedUObjectArray", 0.50f};

		char Buf[192];
		std::snprintf(Buf, sizeof(Buf), "table at +%lld read %u times + %d 32-bit fields%s", (long long)E.PrimaryFieldOffset, E.PrimaryFieldAccesses, E.NarrowFieldCount, E.bChunkConstant ? " + chunk constant" : "");
		E.Verdict = EVerdict::Accepted;
		E.Why = Buf;
		E.Passed = true;
		return E;
	}

	namespace
	{

		/**
		 * @brief Strings that occur in functions touching FUObjectArray.
		 *
		 * Ranking only: breadth matters here and no address is claimed.
		 *
		 * Deliberately absent, each found to carry GC vocabulary rather than
		 * array evidence:
		 *
		 *   GCObjectReferencer      FGCObject bookkeeping, not FUObjectArray
		 *   DeferredFinalizeObjects marks the purge, not the array
		 *   DebugCanvasObject       a GC root, unrelated to the array itself
		 */
		const AnchorString kProximity[] = {
			{"Unable to add more objects to disregard for GC pool", 1.00f},
			{"VERIFYGC", 1.00f},
			{"NOVERIFYGC", 1.00f},
			{"SHOWPENDINGKILLS", 0.90f},
			{"CreateClustersFromPackage", 0.85f},
			{"finishing all objects", 0.80f},
			{"GETALLSTATE", 0.80f},
		};

		/**
		 * @brief Anchors that resolve to the array, with their measured trust.
		 *
		 * Every one earned its place by precision of resolution, not by how often
		 * it occurs: a common string that resolves wrong more often than it
		 * resolves right is deleted rather than down-weighted, since a candidate
		 * anchor also has to name a function that plausibly *derives* the array,
		 * not merely mention GC vocabulary near it. Naming the right operation
		 * does not guarantee a correct resolution on its own, so plausibility
		 * alone is not sufficient to add an anchor here.
		 */
		const Anchor kResolution[] = {
			{"Unable to add more objects to disregard for GC pool", 0.94f, EPickRule::ContainerMember, "FUObjectArray::AllocateUObjectIndex"},
			{"finishing all objects", 0.86f, EPickRule::ContainerMember, "GC purge"},
			{"GETALLSTATE", 1.00f, EPickRule::ContainerMember, "UEngine::Exec"},
		};

	} // namespace

	std::span<const AnchorString> ObjObjectsStrategy::ProximityAnchors() const
	{
		return kProximity;
	}

	std::span<const Anchor> ObjObjectsStrategy::ResolutionAnchors() const
	{
		// These strings live in FUObjectArray member functions, which reach the array
		// through `this` - so they resolve to a member of the object the function
		// operates on. GUObjectArrayStrategy reads the same evidence for the
		// container itself; the two tables are kept apart deliberately so either can
		// gain an anchor the other should not have.
		return kResolution;
	}

	std::vector<Candidate> ObjObjectsStrategy::Score(const AnalysisContext &Ctx) const
	{
		std::vector<Candidate> Out;
		if (!Ctx.Harvester || !Ctx.Memory)
			return Out;

		for (const auto &[Address, Info] : Ctx.Harvester->GetAccesses())
		{
			const float Wide = static_cast<float>(Info.WideAccesses);
			const float Narrow = static_cast<float>(Info.NarrowAccesses);
			if (Wide < Weights.MinWideAccesses)
				continue;

			// Deliberately *not* filtered on "must be named at +0" the way the anchored
			// tier and the name-table leads are: that would drop object-array truths
			// that are only ever reached as container+offset.
			//
			// Deriving the array out of a located GUObjectArray container instead is
			// not done here, even though the container is generally found more
			// reliably than the member. VerifyObjectArray scans a window and finds
			// the one hot table below the container from every offset, so
			// verification cannot pick between candidate members on its own, and
			// count-based signals point at the table rather than at the struct
			// holding it. Reordered forks move the member to different offsets with
			// no fixed relationship to the container, so the offset is not
			// recoverable from local shape alone; deriving it needs the member's own
			// evidence, which is what this loop computes.

			Candidate C(Address, 0.0f, Name());
			C.FindMethod = EFindMethod::Statistical;
			if (!ApplyCommonSignals(C, Ctx, Info))
				continue;

			char Buf[96];

			const float Magnitude = Weights.Magnitude * std::log2(Wide + 1.0f);
			std::snprintf(Buf, sizeof(Buf), "%u pointer-width accesses", Info.WideAccesses);
			C.Add("access_magnitude", Magnitude, Buf);

			const float Total = Wide + Narrow;
			const float Purity = Total > 0 ? (Wide / Total) : 0.0f;
			std::snprintf(Buf, sizeof(Buf), "%.1f%% pointer-width", Purity * 100.0f);
			C.Add("access_purity", Weights.Purity * Purity, Buf);

			const int Offsets = static_cast<int>(Info.Offsets.size());
			if (Offsets >= Weights.OffsetBandLow && Offsets <= Weights.OffsetBandHigh)
			{
				std::snprintf(Buf, sizeof(Buf), "%d distinct offsets", Offsets);
				C.Add("struct_offsets", Weights.OffsetBand, Buf);
			}

			const bool bHas65K = Info.NearbyConstants.count(kNumElementsPerChunk_4_20) > 0;
			const bool bHas64K = Info.NearbyConstants.count(kNumElementsPerChunk_4_21) > 0;
			if ((bHas65K || bHas64K) && Weights.ChunkConstant != 0.0f)
			{
				C.Add("chunk_constant", Weights.ChunkConstant, bHas65K ? "0x10400 (UE4.20)" : "0x10000 (UE>=4.21)");
			}

			if (Ctx.AnchorSitesByAnchor && !Ctx.AnchorSitesByAnchor->empty())
			{
				const float P = StringAnchors::AnchorProximity(Info.Sites, *Ctx.AnchorSitesByAnchor);
				if (P > 0.0f)
				{
					std::snprintf(Buf, sizeof(Buf), "%.0f%% of sites near GC strings", P * 100.0f);
					C.Add("anchor_proximity", Weights.AnchorProximity, Buf);
				}
			}

			float Raw = 0.0f;
			for (const auto &S : C.Signals)
				Raw += S.Weight;
			C.Confidence = std::clamp(Raw / Weights.ScoreScale, 0.0f, 1.0f);

			if (C.Confidence > 0.0f)
				Out.push_back(std::move(C));
		}

		Scoring::SortByConfidence(Out);
		if (Ctx.MaxScored != 0 && Out.size() > Ctx.MaxScored)
			Out.resize(Ctx.MaxScored);
		return Out;
	}

} // namespace anduefker::analyzer
