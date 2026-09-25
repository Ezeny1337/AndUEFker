#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../../Memory/IMemory.h"
#include "../Analysis/AnchorTable.h"
#include "../Analysis/GlobalAccessHarvester.h"
#include "../Analysis/StringAnchors.h"
#include "../Analysis/StructureVerifier.h"
#include "../Core/Candidate.h"
#include "../Core/Layout.h"
#include "../Core/Scoring.h"

namespace anduefker::analyzer
{

	/**
	 * @brief Evidence that an address is used the way a target structure is used.
	 *
	 * The return type of IStrategy::Verify(), read generically by code that
	 * doesn't know which strategy produced it (MakeLayoutDescription,
	 * AnchoredResolution's tie-break, GUObjectArrayStrategy building on
	 * ObjObjectsStrategy's evidence). Fields are strategy-neutral by
	 * construction - "a pointer-width field at this offset, read this often",
	 * not "ObjObjects's table". A concept only one strategy needs belongs in
	 * Why, not as a new field here.
	 */
	struct StructureEvidence
	{
		/// Which structure the access shape reads as, and how strongly.
		///
		/// The typed answer to "what is this likely to be". Unknown when the shape
		/// does not separate the variants; never inferred from an engine version.
		LayoutClassification Layout;

		/// Machine-readable outcome. Why carries the measured specifics.
		EVerdict Verdict = EVerdict::Rejected;

		bool bPointerAtZero = false; ///< Pointer-width load at +0.
		int NarrowFieldCount = 0;	 ///< 32-bit accesses at small offsets.

		/// Distinct header offsets read at pointer width. Separates the two name
		/// structures where nothing else does: every legacy TNameEntryArray has
		/// two, while an FNamePool has none or one - its counters are 32-bit, so
		/// its header is mostly narrow.
		int WideFieldCount = 0;
		std::vector<int64_t> Fields; ///< Which offsets those were.
		bool bChunkConstant = false; ///< NumElementsPerChunk seen in code touching it.
		bool bIndexed = false;		 ///< A pointer-width field sits somewhere in the header.

		/// The most heavily-read pointer-width field a strategy found, and its
		/// access count - not assumed to be at any particular offset, since a
		/// reordered fork can move it. A strategy that has no single field this
		/// distinguished leaves this at its default; nothing requires setting it.
		int64_t PrimaryFieldOffset = -1;
		uint32_t PrimaryFieldAccesses = 0;

		/// A second pointer-width field, distinct from the primary one, that a
		/// strategy found and considers extra corroborating detail beyond the
		/// minimum shape its Verdict required - opaque to generic consumers
		/// beyond its presence and offset. AnchoredResolution's tie-break reads
		/// this generically: candidates with corroborating detail outrank ones
		/// with only the minimum, regardless of which strategy is running.
		int64_t SecondaryFieldOffset = -1;
		bool bHasSecondaryField = false;

		bool Passed = false; ///< Whether the access shape matches the target structure.

		/// Specifics behind Verdict, e.g. "table at +16 read 3649 times". Free-form
		/// by design: it reports numbers read off this binary. Use Verdict or
		/// Layout to branch on, never this.
		std::string Why;
	};

	/**
	 * @brief Everything a strategy may look at.
	 *
	 * Assembled once and shared, so adding a strategy costs one class and no extra
	 * scanning of the image.
	 */
	struct AnalysisContext
	{
		const IMemory *Memory = nullptr; ///< Backend being analysed.

		/// Cached once by the analyzer; GetUnrealModule() returns by value, so no
		/// lookup path may call it.
		const ModuleInfo *Module = nullptr;

		const GlobalAccessHarvester *Harvester = nullptr; ///< Every global code computes.
		const StringAnchors *Anchors = nullptr;			  ///< Located anchor strings.

		/// Code that references an anchor string, ascending. Sorted once by the
		/// analyzer rather than per candidate: scoring asks for anchor proximity
		/// once per plausible global, and there are ~200k of those.
		const std::vector<uint64_t> *AnchorSites = nullptr;

		/// The same sites split by the anchor that produced them, with its weight.
		/// Proximity to a rare anchor is far better evidence than proximity to a
		/// common one, and pooling them discarded that distinction.
		const std::vector<StringAnchors::WeightedSites> *AnchorSitesByAnchor = nullptr;

		/**
		 * @brief Access-count rank of each plausible global, 1 = most referenced.
		 *
		 * Absolute reference counts are not comparable across binaries (a 300 MB
		 * game and a 50 MB one differ by an order of magnitude), but rank is. The
		 * object array consistently sits within the most-referenced few hundred
		 * globals out of roughly 200k, which makes rank one of the most
		 * discriminating static signals available.
		 */
		/// Borrowed, never copied: one entry per plausible global, and a query that
		/// copied the whole map paid for ~200k nodes before scoring anything.
		const std::unordered_map<uint64_t, uint32_t> *AccessRank = nullptr;
		uint32_t RankedCount = 0; ///< How many globals were ranked, for context.

		/// Rank of one address, or 0 when it was not ranked.
		uint32_t RankOf(uint64_t Address) const
		{
			if (!AccessRank)
				return 0u;
			auto It = AccessRank->find(Address);
			return It == AccessRank->end() ? 0u : It->second;
		}

		ScoringWeights Weights; ///< The scoring model in force for this run.

		/**
		 * @brief How many scored candidates a strategy keeps; 0 keeps every one.
		 *
		 * A strategy trims its list so a query does not carry tens of thousands of
		 * no-hope candidates. The trim has to follow the request, though: held at a
		 * fixed 64 it silently overrode any larger AnalyzerOptions::MaxCandidates, so
		 * asking for 200 returned 64. Diagnostics set 0 because the trim also hides
		 * the difference between "a filter rejected this address" and "it scored and
		 * placed 300th", which need opposite fixes.
		 */
		size_t MaxScored = kMaxScoredCandidates;
	};

	/**
	 * @brief One locatable global, described entirely by one class.
	 *
	 * A strategy is the *whole* definition of a target: what it is called, which
	 * strings mark its neighbourhood, which strings identify a function that
	 * resolves it, what its structure looks like, and how a candidate scores.
	 * Nothing else in the codebase branches on which target is being hunted.
	 *
	 * Adding a target means writing a strategy and registering it in one place.
	 * Nothing else needs an edit: weights belong to the strategy, verification is
	 * dispatched through Verify, and diagnostics iterate whatever is registered.
	 *
	 * Strategies never see whether they are running on a file or a live process:
	 * they read through IMemory and consume the shared analysis, which is what
	 * lets the same code serve both.
	 */
	class IStrategy
	{
	public:
		virtual ~IStrategy() = default;

		/// Short name, recorded on every candidate and used in reports.
		virtual const char *Name() const = 0;

		/**
		 * @brief Strings whose neighbourhood suggests this target.
		 *
		 * Used for the `anchor_proximity` ranking signal. A different job from
		 * ResolutionAnchors below: these only have to mark the right
		 * neighbourhood, and the two lists must stay separate - pruning this one
		 * by resolution precision cost the name table its ranking signal outright.
		 */
		virtual std::span<const AnchorString> ProximityAnchors() const = 0;

		/**
		 * @brief Strings that identify a function which must touch this target.
		 *
		 * These carry a pick rule and are scored on whether they resolve to the
		 * exact address; only anchors that resolve reliably survive.
		 */
		virtual std::span<const Anchor> ResolutionAnchors() const = 0;

		/// Whether an address is used the way this target's structure is used.
		virtual StructureEvidence Verify(const StructureVerifier &Verifier,
										 uint64_t Address) const = 0;

		/**
		 * @brief Whether a resolved answer may be presented as verified.
		 *
		 * A strategy whose resolution is measured to be right most of the time, but
		 * not all of the time, returns false: its candidates are still ranked and
		 * offered as leads, and none of them is ever claimed as the answer. That is
		 * the standing rule here - a wrong confident answer is worse than no answer,
		 * because a user cannot tell the two apart.
		 */
		virtual bool ClaimsVerification() const { return true; }

		/// Scores the shared analysis; candidates best first.
		virtual std::vector<Candidate> Score(const AnalysisContext &Ctx) const = 0;
	};

	using StrategyPtr = std::unique_ptr<IStrategy>;

} // namespace anduefker::analyzer
