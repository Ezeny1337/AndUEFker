#include "StringAnchors.h"

#include <algorithm>

#include "../../Memory/IMemory.h"

#include "GlobalAccessHarvester.h"
#include "LiteralScanner.h"

namespace anduefker::analyzer
{

	int StringAnchors::DecideTCharWidth(size_t Utf16Hits, size_t Utf32Hits)
	{
		// A clear majority is required; otherwise the answer is "undetermined"
		// rather than a guess. Both encodings turn up a stray hit or two in most
		// binaries, so a bare comparison would be noise-driven.
		if (Utf16Hits == 0 && Utf32Hits == 0)
			return 0;
		if (Utf16Hits > Utf32Hits * 2)
			return 2;
		if (Utf32Hits > Utf16Hits * 2)
			return 4;
		return 0;
	}

	int StringAnchors::DetectedTCharWidth() const
	{
		return DecideTCharWidth(Hits_.Utf16, Hits_.Utf32);
	}

	void StringAnchors::Run(const IMemory *Memory, const ModuleInfo &Module, std::span<const AnchorString> Anchors)
	{
		AnchorAddrs_.clear();
		FoundNames_.clear();
		PerAnchor_.assign(Anchors.size(), WeightedSites{});
		for (size_t i = 0; i < Anchors.size(); ++i)
			PerAnchor_[i].Weight = Anchors[i].Weight;
		Hits_ = {};

		// Cap per anchor per encoding: a short literal like "None" occurs everywhere,
		// and only enough occurrences to correlate with code are wanted.
		constexpr size_t kMaxHitsPerEncoding = 24;

		// Every anchor, in all three encodings, queued as needles and matched in one
		// walk of the module. Searching them one at a time meant a pass over the
		// whole image per anchor per encoding - tolerable against a mapped file,
		// ruinous against a live process, where each pass is the module pulled
		// through the transport again.
		//
		// All three encodings are always searched: which one a build uses cannot be
		// derived from its engine version, so it is counted here and reported by
		// DetectedTCharWidth().
		LiteralScanner Scanner;
		for (const AnchorString &A : Anchors)
			Scanner.AddLiteral(A.Text, kMaxHitsPerEncoding);

		std::vector<bool> AnchorFound(Anchors.size(), false);
		for (const LiteralScanner::Hit &H : Scanner.Scan(Memory, Module))
		{
			const LiteralScanner::Needle &N = Scanner.GetNeedles()[H.NeedleIndex];
			AnchorAddrs_.push_back(H.Address);
			if (N.OwnerIndex < AnchorFound.size())
			{
				AnchorFound[N.OwnerIndex] = true;
				PerAnchor_[N.OwnerIndex].Sites.push_back(H.Address);
			}

			switch (N.Encoding)
			{
			case 1:
				++Hits_.Narrow;
				break;
			case 2:
				++Hits_.Utf16;
				break;
			default:
				++Hits_.Utf32;
				break;
			}
		}

		for (size_t i = 0; i < Anchors.size(); ++i)
			if (AnchorFound[i])
				FoundNames_.emplace_back(Anchors[i].Text);

		std::sort(AnchorAddrs_.begin(), AnchorAddrs_.end());
		AnchorAddrs_.erase(std::unique(AnchorAddrs_.begin(), AnchorAddrs_.end()), AnchorAddrs_.end());
	}

	std::vector<StringAnchors::WeightedSites> StringAnchors::CollectAnchorSitesByAnchor(
		const GlobalAccessHarvester &Harvester) const
	{
		std::vector<WeightedSites> Out;
		Out.reserve(PerAnchor_.size());

		for (const WeightedSites &A : PerAnchor_)
		{
			WeightedSites Entry;
			Entry.Weight = A.Weight;
			for (uint64_t Addr : A.Sites)
				if (const AccessInfo *Info = Harvester.Find(Addr))
					Entry.Sites.insert(Entry.Sites.end(), Info->Sites.begin(), Info->Sites.end());

			std::sort(Entry.Sites.begin(), Entry.Sites.end());
			Entry.Sites.erase(std::unique(Entry.Sites.begin(), Entry.Sites.end()), Entry.Sites.end());
			if (!Entry.Sites.empty())
				Out.push_back(std::move(Entry));
		}
		return Out;
	}

	float StringAnchors::AnchorProximity(const std::vector<uint64_t> &Sites,
										 const std::vector<WeightedSites> &ByAnchor,
										 uint64_t Window)
	{
		if (Sites.empty() || ByAnchor.empty())
			return 0.0f;

		float Near = 0.0f;
		for (uint64_t S : Sites)
		{
			// The best anchor this site is near, not the count of anchors near it:
			// several weak anchors must not add up to one strong one.
			float BestWeight = 0.0f;
			for (const WeightedSites &A : ByAnchor)
			{
				if (A.Weight <= BestWeight)
					continue; // cannot improve on what we already have

				auto It = std::lower_bound(A.Sites.begin(), A.Sites.end(), S);
				uint64_t Best = UINT64_MAX;
				if (It != A.Sites.end())
					Best = std::min(Best, *It - S);
				if (It != A.Sites.begin())
					Best = std::min(Best, S - *(It - 1));
				if (Best <= Window)
					BestWeight = A.Weight;
			}
			Near += BestWeight;
		}
		return Near / static_cast<float>(Sites.size());
	}

	std::unordered_set<uint64_t> StringAnchors::CollectAnchorSites(
		const GlobalAccessHarvester &Harvester) const
	{
		std::unordered_set<uint64_t> Sites;
		for (uint64_t Addr : AnchorAddrs_)
		{
			if (const AccessInfo *Info = Harvester.Find(Addr))
			{
				for (uint64_t Site : Info->Sites)
					Sites.insert(Site);
			}
		}
		return Sites;
	}

	float StringAnchors::AnchorProximity(const std::vector<uint64_t> &Sites,
										 const std::vector<uint64_t> &Sorted,
										 uint64_t Window)
	{
		if (Sites.empty() || Sorted.empty())
			return 0.0f;

		// A real distance test rather than a coarser approximation, since a bucket
		// index can flatten the signal and let decoys tie with the true target.
		size_t Near = 0;
		for (uint64_t S : Sites)
		{
			auto It = std::lower_bound(Sorted.begin(), Sorted.end(), S);
			uint64_t Best = UINT64_MAX;
			if (It != Sorted.end())
				Best = std::min(Best, *It - S);
			if (It != Sorted.begin())
				Best = std::min(Best, S - *(It - 1));
			if (Best <= Window)
				++Near;
		}
		return static_cast<float>(Near) / static_cast<float>(Sites.size());
	}

} // namespace anduefker::analyzer
