#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../Memory/IMemory.h"

#include "AnchorTable.h"

namespace anduefker::analyzer
{

	class GlobalAccessHarvester;

	/**
	 * @brief Locates anchor strings and the code that references them.
	 *
	 * String literals can appear in three different encodings depending on the
	 * build - ASCII/UTF-8 always, plus UTF-32 on UE <= 4.19 (4-byte wchar_t on
	 * Unix-like targets) and UTF-16 on newer builds. All three are searched,
	 * because assuming one silently loses every anchor in builds that use
	 * another.
	 */
	class StringAnchors
	{
	public:
		/**
		 * @brief Finds the given anchors and records the addresses they live at.
		 *
		 * The caller supplies the list - each strategy owns its own - so this has no
		 * notion of which target is being hunted. The module is passed in rather than
		 * looked up, because every caller already has it and the lookup is not free.
		 */
		void Run(const IMemory *Memory, const ModuleInfo &Module, std::span<const AnchorString> Anchors);

		/// Addresses of located anchor strings.
		const std::vector<uint64_t> &GetAnchorAddresses() const { return AnchorAddrs_; }

		/// Human-readable list of which anchors were found, for explainability.
		const std::vector<std::string> &GetFoundNames() const { return FoundNames_; }

		/// Anchor hits per encoding, measured rather than assumed.
		struct EncodingHits
		{
			size_t Narrow = 0; // ASCII / UTF-8
			size_t Utf16 = 0;
			size_t Utf32 = 0;
		};
		const EncodingHits &GetEncodingHits() const { return Hits_; }

		/**
		 * @brief Measured TCHAR width for this image: 2, 4, or 0 when undetermined.
		 *
		 * Engine version does not determine this - it is a build configuration, and
		 * two titles on the same engine version can differ. So the width is taken
		 * from whichever wide encoding the anchor strings were actually found in.
		 */
		int DetectedTCharWidth() const;

		/// Decision rule behind DetectedTCharWidth, split out so it is testable
		/// without a binary. Narrow hits are not passed in: ASCII literals appear in
		/// every build regardless of TCHAR width, so only wide hits are informative.
		static int DecideTCharWidth(size_t Utf16Hits, size_t Utf32Hits);

		/**
		 * @brief Code addresses that reference any located anchor string.
		 *
		 * Built from the harvester's access map: a site that computes an anchor
		 * string's address is code that mentions the anchor.
		 */
		std::unordered_set<uint64_t> CollectAnchorSites(const GlobalAccessHarvester &Harvester) const;

		/**
		 * @brief One anchor's weight and the code sites that reference it, ascending.
		 *
		 * Kept apart per anchor because the anchors are not equally informative. A
		 * global near ERROR_NAME_SIZE_EXCEEDED - which occurs in FName's own code and
		 * essentially nowhere else - is far better evidence than one near `None`,
		 * which occurs everywhere. Pooling them threw that away.
		 */
		struct WeightedSites
		{
			float Weight = 1.0f;
			std::vector<uint64_t> Sites; ///< Ascending, so proximity can binary-search.
		};

		/// @brief Anchor sites grouped by the anchor that produced them.
		std::vector<WeightedSites> CollectAnchorSitesByAnchor(const GlobalAccessHarvester &Harvester) const;

		bool Empty() const { return AnchorAddrs_.empty(); }

		/**
		 * @brief Proximity of a candidate's access sites to anchor code.
		 *
		 * Being *used by the same function* as "VERIFYGC" or "None" is far more
		 * specific than any byte pattern, which is why this is a scoring signal rather
		 * than a filter.
		 *
		 * @param Sites       Code addresses that access the candidate.
		 * @param SortedAnchorSites Code addresses that reference an anchor string,
		 *        ascending. Sorting is the caller's job because this is called once
		 *        per candidate and the set does not change between calls.
		 * @param Window      How close, in bytes, counts as "near".
		 * @return Fraction of Sites within Window bytes of an anchor site, in [0,1].
		 */
		/**
		 * @brief Weighted proximity: how near a candidate's sites are to *good* anchors.
		 *
		 * Same shape as the unweighted form - a fraction of the candidate's own sites,
		 * which is what keeps a globally-common string from saturating - except each
		 * near site contributes the weight of the best anchor it is near, rather than
		 * a flat 1. Identical to the unweighted form when every weight is 1.
		 */
		static float AnchorProximity(const std::vector<uint64_t> &Sites,
									 const std::vector<WeightedSites> &ByAnchor,
									 uint64_t Window = 0x4000);

		static float AnchorProximity(const std::vector<uint64_t> &Sites,
									 const std::vector<uint64_t> &SortedAnchorSites,
									 uint64_t Window = 0x4000);

	private:
		std::vector<uint64_t> AnchorAddrs_;
		/// Per anchor, in the order the caller supplied them: weight and its own hits.
		std::vector<WeightedSites> PerAnchor_;
		std::vector<std::string> FoundNames_;
		EncodingHits Hits_;
	};

} // namespace anduefker::analyzer
