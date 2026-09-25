#include "GUObjectArrayStrategy.h"

#include <cmath>
#include <cstdio>

#include "../Analysis/GlobalAccessHarvester.h"
#include "../Analysis/StringAnchors.h"
#include "../Analysis/StructureVerifier.h"
#include "../Core/Scoring.h"
#include "ObjObjectsStrategy.h"

namespace anduefker::analyzer
{
	StructureEvidence GUObjectArrayStrategy::Verify(const StructureVerifier &Verifier, uint64_t Address) const
	{
		// Start from the array's own evidence: a container holds one, so everything
		// that makes an object array recognisable has to be visible from here too.
		StructureEvidence E = VerifyObjectArrayShape(Verifier, Address);
		if (!E.Passed)
			return E;

		// A struct holding a pointer member is at least pointer-aligned, so an
		// address that is not cannot be one. This is the ABI, not a heuristic.
		//
		// The image's pointer width, not a hardcoded 8: a 32-bit image is analysed
		// by the 32-bit build, so uintptr_t is the image's own pointer width here.
		if (Address % sizeof(uintptr_t) != 0)
		{
			E.Passed = false;
			E.Why = "not pointer-aligned, so it cannot hold a pointer member";
			return E;
		}

		const AccessInfo *Self = Verifier.Harvester().Find(Address);
		if (!Self)
		{
			E.Passed = false;
			E.Why = "no recorded access";
			return E;
		}

		// Named by the code, one way or the other. AddressTaken is the address being
		// computed; IndirectBase is it being read out of a relocated slot, which is
		// the only way several builds ever reach it.
		if (!Self->HasKind(EAccessKind::AddressTaken) && !Self->HasKind(EAccessKind::IndirectBase))
		{
			E.Passed = false;
			E.Why = "never named: no address-take and no load from a relocation slot";
			return E;
		}

		// The table has to be a member, not the object itself. A container whose hot
		// pointer sits at +0 is the array, and reporting it as the container would
		// hand back an address one struct too low.
		if (E.PrimaryFieldOffset <= 0)
		{
			E.Verdict = EVerdict::Rejected;
			E.Passed = false;
			E.Why = "the table is at +0, so this is the array, not its container";
			return E;
		}

		// The container is identified by holding an array, not by being one, so the
		// kind is fixed once the interior table is established.
		E.Layout = {"FUObjectArray", 0.85f};

		char Buf[224];
		std::snprintf(Buf, sizeof(Buf), "named by code, holds a table at +%lld read %u times%s", (long long)E.PrimaryFieldOffset, E.PrimaryFieldAccesses, E.bChunkConstant ? " + chunk constant" : "");
		E.Verdict = EVerdict::Accepted;
		E.Why = Buf;
		return E;
	}

	namespace
	{

		/**
		 * @brief Strings that occur in functions operating on FUObjectArray.
		 *
		 * Ranking only: breadth matters here and no address is claimed. The container
		 * is what these functions hold in `this`, so it has at least as much claim to
		 * them as the array member does.
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
		 * @brief Anchors that resolve to the container, with their measured trust.
		 *
		 * The same strings ObjObjectsStrategy uses, resolved one level out: in an
		 * FUObjectArray member function the container *is* `this`, so the object the
		 * function operates on is the container and the array is a field of it.
		 *
		 * Trust values are scored for this target separately from the member - a
		 * value that stops holding here shows up independently of whether it still
		 * holds for the member. Anchors that resolve correctly too rarely are
		 * deleted, not down-weighted, since a single confident wrong resolution is
		 * worse than an absent one.
		 *
		 * The container is referenced by more distinct strings than the member is,
		 * including a whole GC-settings vocabulary the member never sees, but that
		 * surplus is breadth, not resolution: a settings or lifecycle function
		 * touches too much engine state for the derivation to reliably pick the
		 * array out of it. Those strings still pay off in ProximityAnchors, which
		 * is scored on breadth, and not here.
		 */
		const Anchor kResolution[] = {
			{"Unable to add more objects to disregard for GC pool", 0.94f, EPickRule::Container, "FUObjectArray::AllocateUObjectIndex"},
			{"finishing all objects", 0.86f, EPickRule::Container, "GC purge"},
			{"GETALLSTATE", 1.00f, EPickRule::Container, "UEngine::Exec"},
		};

	} // namespace

	std::span<const AnchorString> GUObjectArrayStrategy::ProximityAnchors() const
	{
		return kProximity;
	}

	std::span<const Anchor> GUObjectArrayStrategy::ResolutionAnchors() const
	{
		return kResolution;
	}

	std::vector<Candidate> GUObjectArrayStrategy::Score(const AnalysisContext &Ctx) const
	{
		std::vector<Candidate> Out;
		if (!Ctx.Harvester || !Ctx.Memory || !Ctx.Module)
			return Out;

		const StructureVerifier Verifier(*Ctx.Module, *Ctx.Harvester);

		for (const auto &[Address, Info] : Ctx.Harvester->GetAccesses())
		{
			// Named by the code, either form. Gating on AddressTaken alone would drop
			// every build that only ever loads the container out of a relocation
			// slot, which is most of them.
			if (!Info.HasKind(EAccessKind::AddressTaken) && !Info.HasKind(EAccessKind::IndirectBase))
				continue;

			const StructureEvidence Struct = Verify(Verifier, Address);
			if (!Struct.Passed)
				continue;

			Candidate C(Address, 0.0f, Name());
			C.FindMethod = EFindMethod::Statistical;
			if (!ApplyCommonSignals(C, Ctx, Info))
				continue;

			char Buf[160];

			C.Add("structure_verified", 0.0f, Struct.Why);

			// How hard the table inside it is worked. This is the container's own
			// version of the array's magnitude signal: the object array is one of the
			// hottest globals in any UE binary, and that heat is visible from the
			// object holding it.
			const float Table = Weights.TableHeat * std::log2(static_cast<float>(Struct.PrimaryFieldAccesses) + 1.0f);
			std::snprintf(Buf, sizeof(Buf), "table at +%lld read %u times", (long long)Struct.PrimaryFieldOffset, Struct.PrimaryFieldAccesses);
			C.Add("table_heat", Table, Buf);

			// Being named a lot is the container's distinguishing property - it is
			// passed as `this` all over the engine, while the array it holds is
			// dereferenced rather than passed.
			const float Named = Weights.NamedByCode * std::log2(static_cast<float>(Info.Count) + 1.0f);
			std::snprintf(Buf, sizeof(Buf), "named %u times", Info.Count);
			C.Add("named", Named, Buf);

			if (const uint32_t Rank = Ctx.RankOf(Address))
			{
				const float RankScore = Weights.AccessRank * Scoring::ScoreAccessRank(Rank);
				std::snprintf(Buf, sizeof(Buf), "rank %u of %u", Rank, Ctx.RankedCount);
				C.Add("access_rank", RankScore, Buf);
			}

			if (Ctx.AnchorSitesByAnchor && !Ctx.AnchorSitesByAnchor->empty())
			{
				const float P = StringAnchors::AnchorProximity(Info.Sites, *Ctx.AnchorSitesByAnchor);
				if (P > 0.0f)
				{
					std::snprintf(Buf, sizeof(Buf), "%.0f%% of sites near GC/init strings", P * 100.0f);
					C.Add("anchor_proximity", Weights.AnchorProximity * P, Buf);
				}
			}

			float Raw = 0.0f;
			for (const Signal &S : C.Signals)
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
