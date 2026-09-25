#include "NameStrategy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../Analysis/StructureVerifier.h"
#include "../Core/UEConstants.h"

namespace anduefker::analyzer
{
	namespace
	{
		/// How far past the header to look for FNamePool's block array. Wide
		/// enough to cover known layouts and forked builds that move the field
		/// further out; no offset in the range is privileged.
		constexpr int64_t kBlockArrayWindow = 0x100;

		/**
		 * @brief True when Address looks like a field inside a larger structure
		 *        we already believe in, rather than a base of its own.
		 *
		 * A big inline FNamePool's interior fields are plausible-looking writable
		 * globals that share every anchored function with the real base. The test
		 * is positional: a more heavily referenced, address-taken global sitting
		 * below this one within plausible range means this one is most likely
		 * inside it.
		 *
		 * @param OutEnclosing Optional: receives the global that made this look
		 *        interior, so a caller can report *why* rather than only that.
		 */
		bool IsInteriorToPool(const StructureVerifier &Verifier, uint64_t Address, const AccessInfo &Self, uint64_t *OutEnclosing = nullptr)
		{
			// Roughly FNamePool's size: its block array alone is 8192 pointers.
			constexpr uint64_t kPoolSpan = 0x20000;
			// Bounded so this stays cheap on a binary with hundreds of thousands of
			// globals; the real pool base is far closer than this limit in practice.
			constexpr size_t kMaxNeighbours = 512;

			const GlobalAccessHarvester &Harvester = Verifier.Harvester();
			const std::vector<uint64_t> &All = Verifier.SortedGlobals();
			// Where Address sits in the list. Fixed for the whole loop, so it is found
			// once rather than re-searched per neighbour.
			const auto Start = std::lower_bound(All.begin(), All.end(), Address);
			auto It = Start;

			for (size_t N = 0; It != All.begin() && N < kMaxNeighbours; ++N)
			{
				--It;
				const uint64_t Below = *It;
				if (Address - Below > kPoolSpan)
					break;

				const AccessInfo *Info = Harvester.Find(Below);
				if (!Info)
					continue;
				// The enclosing structure is address-taken (code passes it around) and
				// is referenced at least as much as the field being tested.
				if (Info->Count < Self.Count ||
					(Info->Kinds & static_cast<uint8_t>(EAccessKind::AddressTaken)) == 0)
					continue;

				// ...and the span between them has to look like a structure's body
				// rather than like ordinary .bss. A genuine interior field sits inside a
				// body that nothing else addresses directly, so few globals are
				// recorded in that span; a global that merely happens to have something
				// hot below it sits in ordinary densely-packed .bss, with many globals
				// per KB. This bytes-per-global density is what separates the two.
				//
				// Global count ratio and the enclosing global's own offset count were
				// both tried and neither reliably separates the cases; density does.
				// Note this makes precision/recall tradeoffs when disambiguating a
				// structure from a neighbour a few bytes below it, since such a
				// neighbour shares the same interior fields at shifted offsets.
				constexpr uint64_t kMinBytesPerGlobal = 1024;

				// All holds the access map's keys, so it is sorted and free of
				// duplicates: everything strictly above Below starts at It + 1, and a
				// second binary search per neighbour would only rediscover that.
				const size_t Between = static_cast<size_t>(Start - (It + 1));
				if (Between != 0 && (Address - Below) / Between < kMinBytesPerGlobal)
					continue;

				if (OutEnclosing)
					*OutEnclosing = Below;
				return true;
			}
			return false;
		}
	} // namespace

	namespace
	{
		// Strings that occur in functions touching the name table, drawn from the
		// engine's own FName vocabulary rather than general UObject/package
		// vocabulary, which carries little signal.
		//
		// The FName statistics strings are rare and unambiguous - they occur in
		// FName's own code and essentially nowhere else, which is what a proximity
		// marker needs.
		//
		// `None`, `*INVALID*` and `_%d` are common throughout a UE binary, so
		// proximity to them might seem to saturate and carry no information - but
		// proximity is a *fraction* of a candidate's own sites, not a raw count. A
		// global touched only by FName code still scores high on a common string,
		// while a global touched everywhere does not, so these do discriminate.
		const AnchorString kProximityAnchors[] = {
			// Rare and unambiguous: FName's own diagnostics.
			{"ERROR_NAME_SIZE_EXCEEDED", 1.00f},
			{"%i FNames using in %ikB + %ikB", 1.00f},
			{"%d ansi FNames", 0.95f},
			{"%d wide FNames", 0.95f},
			// Common, but still discriminating for the reason above.
			{"*INVALID*", 0.80f},
			{"None", 0.45f},
			{"_%d", 0.35f},
		};

		/// Resolution anchors: these must land on the exact address.
		/**
		 * @brief Reserved names, used to reach the pool from its callers.
		 *
		 * These are the FName spellings from UnrealNames.inl. They identify
		 * FNamePool::FNamePool, which is where a pool build's table actually comes
		 * from: the constructor receives it in X0, so the evidence is on the caller
		 * side and CallerArgSelf is the only rule that reads it.
		 *
		 * Terminated matching is required, not preferred: substring matching would
		 * pick up occurrences of the name inside unrelated mangled C++ symbols, and
		 * requiring a leading NUL as well is too strict to match anything, since
		 * literals are packed back to back.
		 *
		 * The "U"-prefixed twins are deliberately absent: they are UClass names and
		 * lead to the class registrar rather than to FName's own code.
		 */
		const Anchor kResolutionAnchors[] = {
			{"ByteProperty", 0.70f, EPickRule::CallerArgSelf, "FNamePool::FNamePool", EAnchorMatch::Terminated},
			{"IntProperty", 0.70f, EPickRule::CallerArgSelf, "FNamePool::FNamePool", EAnchorMatch::Terminated},
			{"ObjectProperty", 0.70f, EPickRule::CallerArgSelf, "FNamePool::FNamePool", EAnchorMatch::Terminated},
			{"BoolProperty", 0.70f, EPickRule::CallerArgSelf, "FNamePool::FNamePool", EAnchorMatch::Terminated},
			{"FloatProperty", 0.70f, EPickRule::CallerArgSelf, "FNamePool::FNamePool", EAnchorMatch::Terminated},
			{"ERROR_NAME_SIZE_EXCEEDED", 0.88f, EPickRule::SingleVerified, "FName::FName"},
			{"_%d", 0.77f, EPickRule::CalleeArgument, "FName number suffix"},
			{"%i FNames using in %ikB + %ikB", 0.60f, EPickRule::CalleeArgument, "FName memory stats"},
		};

	} // namespace

	std::span<const AnchorString> NameStrategy::ProximityAnchors() const
	{
		return kProximityAnchors;
	}

	std::span<const Anchor> NameStrategy::ResolutionAnchors() const
	{
		return kResolutionAnchors;
	}

	StructureEvidence NameStrategy::Verify(const StructureVerifier &Verifier, uint64_t Address) const
	{
		StructureEvidence E;

		if (!Verifier.IsPlausibleGlobalLocation(Address))
		{
			E.Verdict = EVerdict::NotWritableData;
			E.Why = EVerdictToString(E.Verdict);
			return E;
		}

		const GlobalAccessHarvester &Harvester = Verifier.Harvester();

		const AccessInfo *Self = Harvester.Find(Address);
		if (!Self)
		{
			E.Verdict = EVerdict::Unreferenced;
			E.Why = EVerdictToString(E.Verdict);
			return E;
		}

		// Not a blanket "must be referenced at displacement 0" requirement: some
		// builds address many globals from one hoisted base, so a genuine table
		// base can still be reached only at a nonzero offset. Two stages instead,
		// because neither test alone is sufficient:
		//
		//   referenced at +0     -> accept. The code names this address directly,
		//                           which an interior field of a pool is never seen
		//                           to do.
		//   never at +0          -> it may be a field, so ask whether it sits inside
		//                           a bigger, address-taken global.
		uint64_t Enclosing = 0;
		if (Self->Offsets.count(0) == 0 && IsInteriorToPool(Verifier, Address, *Self, &Enclosing))
		{
			// The enclosing global is named, not just alluded to: this rule rejects
			// more true answers than any other, so a reader needs to see what it
			// matched in order to judge whether it was right.
			const AccessInfo *Encl = Harvester.Find(Enclosing);

			// How many recorded globals lie between the two. A real pool base has
			// its own fields in that span; an unrelated hot global that merely sits
			// below has sparse space under it.
			const std::vector<uint64_t> &All = Verifier.SortedGlobals();
			const size_t Between = static_cast<size_t>(
				std::lower_bound(All.begin(), All.end(), Address) - std::upper_bound(All.begin(), All.end(), Enclosing));

			// That span decides rather than merely being reported: a base with
			// nothing recorded between itself and this address has no fields there
			// for this address to be interior to, so what it has is a neighbour.
			// The distinction matters most where the architecture makes one address
			// serve a whole neighbourhood of unrelated globals, which
			// position-independent code does whenever it anchors a run of them to a
			// single materialized address and reaches each one by displacement.
			if (Between != 0)
			{
				char Buf[224];
				std::snprintf(Buf, sizeof(Buf), "never named at +0 and sits 0x%llX inside 0x%llX (count %u vs %u, %zu between), "
												"which is address-taken; this is a field, not the base",
							  (unsigned long long)(Address - Enclosing),
							  (unsigned long long)Enclosing,
							  Encl ? Encl->Count : 0u,
							  Self->Count,
							  Between);
				E.Why = Buf;
				return E;
			}
		}

		// TNameEntryArray is a pointer to a chunk table; FNamePool is an inline
		// structure whose block array is read the same way. Both are reached as a
		// pointer-width read, with a 32-bit count nearby.
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
				E.bIndexed = true;
			}
			if (Info->NarrowAccesses > 0)
			{
				++E.NarrowFieldCount;
				E.Fields.push_back(Off);
			}
			// A chunk constant belongs to the object array, so seeing one here says
			// this is the wrong structure.
			if (Info->NearbyConstants.count(kNumElementsPerChunk_4_20) ||
				Info->NearbyConstants.count(kNumElementsPerChunk_4_21))
				E.bChunkConstant = true;
		}

		// The block array lives past the header, so the header window cannot see it.
		// Scan outward for a pointer-width field with a 32-bit field beside it - the
		// block pointer plus its current-block / byte-cursor counters.
		for (int64_t Off = kStructHeaderWindow + 4; Off <= kBlockArrayWindow; Off += 4)
		{
			const AccessInfo *Info = Harvester.Find(Address + static_cast<uint64_t>(Off));
			if (!Info || Info->WideAccesses == 0)
				continue;

			bool bCounterBeside = false;
			for (int64_t Near = Off - 0x10; Near <= Off + 0x10 && !bCounterBeside; Near += 4)
			{
				if (Near == Off || Near < 0)
					continue;
				const AccessInfo *N = Harvester.Find(Address + static_cast<uint64_t>(Near));
				bCounterBeside = N && N->NarrowAccesses > 0;
			}
			if (!bCounterBeside)
				continue;

			E.bHasSecondaryField = true;
			E.SecondaryFieldOffset = Off;
			break;
		}

		// Deliberately *not* rejected on the chunk constant. FNameEntryAllocator's
		// block size is 65536, same as the object array's chunk size, so the
		// constant is shared by both structures and is not evidence either way.
		// Telling the two apart is the job of the shape checks below and the
		// base-symbol rule above, both of which are specific to their structure.
		//
		// The two shipped forms are not read the same way, and both are accepted:
		//
		//   TNameEntryArray  GNames is a *pointer* to the chunk table, so the header
		//                    is read at pointer width.
		//   FNamePool        NamePoolData is the pool itself, an inline struct. Code
		//                    takes its address and passes it around; the header is
		//                    never loaded as a pointer at all.
		//
		// Either shape is accepted, and which one matched is recorded as evidence.
		const bool bArrayShape = E.bPointerAtZero;
		const bool bPoolShape =
			(Self->Kinds & static_cast<uint8_t>(EAccessKind::AddressTaken)) != 0 && Self->Count >= 8;

		if (!bArrayShape && !bPoolShape)
		{
			E.Verdict = EVerdict::Rejected;
			E.Why = "neither a loaded pointer (TNameEntryArray) nor an address-taken base (FNamePool)";
			return E;
		}

		// Which of the two the shape reads as. Both can hold at once - a pool whose
		// header happens to be read at pointer width - so the classification says
		// which is better supported rather than picking arbitrarily.
		//
		// A block array is the pool's most distinctive feature and almost never
		// appears on a legacy array, so it carries the strongest reading.
		if (bPoolShape && E.bHasSecondaryField)
			E.Layout = {"FNamePool", 0.95f};
		else if (bPoolShape && !bArrayShape)
			E.Layout = {"FNamePool", 0.80f};
		else if (bArrayShape && !bPoolShape)
			E.Layout = {"TNameEntryArray", 0.80f};
		else if (bPoolShape)
			E.Layout = {"FNamePool", 0.55f};
		else
			E.Layout = {"TNameEntryArray", 0.55f};

		char Buf[224];
		char Block[64] = "";
		if (E.bHasSecondaryField)
			std::snprintf(Block, sizeof(Block), " + block array at +0x%llX", (unsigned long long)E.SecondaryFieldOffset);
		std::snprintf(Buf, sizeof(Buf), "%s shape + %d 32-bit field(s)%s", E.Layout.Kind.c_str(), E.NarrowFieldCount, Block);
		E.Verdict = EVerdict::Accepted;
		E.Why = Buf;
		E.Passed = true;
		return E;
	}

	std::vector<Candidate> NameStrategy::Score(const AnalysisContext &Ctx) const
	{
		std::vector<Candidate> Out;
		if (!Ctx.Harvester || !Ctx.Memory)
			return Out;

		// Filter on what the structure *is*, not on a count. A raw threshold on
		// offset/access counts discards candidates before any evidence is weighed,
		// and does so silently for name tables with unusual access patterns.
		//
		// Structure verification is the better filter: a base-symbol reference at
		// +0, plus either the TNameEntryArray loaded-pointer shape or the
		// FNamePool address-taken shape. A reordered pool is scored down at worst,
		// never dropped outright.
		const StructureVerifier Verifier(*Ctx.Module, *Ctx.Harvester);

		for (const auto &[Address, Info] : Ctx.Harvester->GetAccesses())
		{
			const StructureEvidence Struct = Verify(Verifier, Address);
			if (!Struct.Passed)
				continue;

			const int Offsets = static_cast<int>(Info.Offsets.size());
			const float Count = static_cast<float>(Info.Count);

			Candidate C(Address, 0.0f, Name());
			C.FindMethod = EFindMethod::Statistical;
			if (!ApplyCommonSignals(C, Ctx, Info))
				continue;

			char Buf[96];

			float Anchor = 0.0f;
			if (Ctx.AnchorSitesByAnchor && !Ctx.AnchorSitesByAnchor->empty())
			{
				const float P = StringAnchors::AnchorProximity(Info.Sites, *Ctx.AnchorSitesByAnchor);
				if (P > 0.0f)
				{
					Anchor = Weights.AnchorProximity * P;
					std::snprintf(Buf, sizeof(Buf), "%.0f%% of sites near FName strings", P * 100.0f);
					C.Add("anchor_proximity", Anchor, Buf);
				}
			}

			C.Add("structure_verified", 0.0f, Struct.Why);

			// The block array is *reported*, not rewarded here: rewarding it in
			// scoring would systematically outrank legacy TNameEntryArray builds,
			// which have no block array to show, across the fused candidate list.
			//
			// It still earns its place: StructureVerifier uses it to break
			// EPickRule::SingleVerified ties, where it decides between two candidates
			// that both verify rather than competing across a whole binary.
			if (Struct.bHasSecondaryField)
			{
				char BBuf[80];
				std::snprintf(BBuf, sizeof(BBuf), "block array at +0x%llX", (unsigned long long)Struct.SecondaryFieldOffset);
				C.Add("block_array", 0.0f, BBuf);
			}

			// Soft, not decisive. The table is reached at a single offset in almost
			// every build, so this is real evidence, but exceptions exist - which is
			// exactly why it must not be a gate.
			if (Offsets <= Weights.MaxOffsets)
				C.Add("single_offset", Weights.SingleOffset, Offsets == 1 ? "reached at one offset" : "reached at few offsets");

			// Distance from the access-count band the real table occupies.
			const float LogCount = std::log2(Count + 1.0f);
			const float BandPenalty = Weights.CountBand * std::fabs(LogCount - Weights.CountPeakLog2);
			std::snprintf(Buf, sizeof(Buf), "%u accesses", Info.Count);
			C.Add("count_band", -BandPenalty, Buf);

			if (Info.NearbyConstants.count(kNumElementsPerChunk_4_20) ||
				Info.NearbyConstants.count(kNumElementsPerChunk_4_21))
			{
				// A chunk constant belongs to the object array, so seeing one here is
				// evidence against this being the name table.
				C.Add("chunk_constant_penalty", -Weights.ChunkPenalty, "belongs to the object array");
			}

			if (Info.WideAccesses > 0)
				C.Add("pointer_width_access", Weights.WideBonus);

			// The pool is an inline structure whose base gets taken; the legacy
			// array is a pointer that gets loaded. Either shape earns the same
			// small tie-break.
			if (Info.HasKind(EAccessKind::AddressTaken))
				C.Add("pool_shape", 0.05f, "address-taken base");
			else if (Info.HasKind(EAccessKind::Load))
				C.Add("array_shape", 0.05f, "loaded pointer");

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
