#include "StringAnchors.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

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
		StringAnchors *Output = this;
		RunBatch(Memory, Module, std::span<StringAnchors *const>(&Output, 1), {Anchors});
	}

	void StringAnchors::RunBatch(const IMemory *Memory, const ModuleInfo &Module,
								 std::span<StringAnchors *const> Outputs, const std::vector<std::span<const AnchorString>> &Groups,
								 LiteralScanBatch *Batch)
	{
		if (Outputs.size() != Groups.size() || std::any_of(Outputs.begin(), Outputs.end(),
														   [](const StringAnchors *Output)
														   { return Output == nullptr; }))
			throw std::invalid_argument("anchor output groups are inconsistent");
		LiteralScanner Scanner;
		std::vector<std::pair<size_t, size_t>> Owners;
		std::vector<std::vector<bool>> Found;
		for (size_t group = 0; group < Groups.size(); ++group)
		{
			auto &Output = *Outputs[group];
			Output.AnchorAddrs_.clear();
			Output.FoundNames_.clear();
			Output.Hits_ = {};
			Output.PerAnchor_.assign(Groups[group].size(), WeightedSites{});
			Found.emplace_back(Groups[group].size(), false);
			for (size_t item = 0; item < Groups[group].size(); ++item)
			{
				Output.PerAnchor_[item].Weight = Groups[group][item].Weight;
				Scanner.AddLiteral(Groups[group][item].Text, 24);
				Owners.emplace_back(group, item);
			}
		}
		auto Complete = [Outputs = std::vector<StringAnchors *>(Outputs.begin(), Outputs.end()), Groups,
						 Owners = std::move(Owners), Found = std::move(Found)]
			(const LiteralScanner &Scanner, const std::vector<LiteralScanner::Hit> &Hits) mutable
		{
			for (const LiteralScanner::Hit &H : Hits)
			{
				const LiteralScanner::Needle &N = Scanner.GetNeedles()[H.NeedleIndex];
				const auto [group, item] = Owners[N.OwnerIndex];
				auto &Output = *Outputs[group];
				Output.AnchorAddrs_.push_back(H.Address);
				Found[group][item] = true;
				Output.PerAnchor_[item].Sites.push_back(H.Address);
				switch (N.Encoding)
				{
				case 1:
					++Output.Hits_.Narrow;
					break;
				case 2:
					++Output.Hits_.Utf16;
					break;
				default:
					++Output.Hits_.Utf32;
					break;
				}
			}
			for (size_t group = 0; group < Groups.size(); ++group)
			{
				auto &Output = *Outputs[group];
				for (size_t item = 0; item < Groups[group].size(); ++item)
					if (Found[group][item])
						Output.FoundNames_.emplace_back(Groups[group][item].Text);
				std::sort(Output.AnchorAddrs_.begin(), Output.AnchorAddrs_.end());
				Output.AnchorAddrs_.erase(std::unique(Output.AnchorAddrs_.begin(), Output.AnchorAddrs_.end()), Output.AnchorAddrs_.end());
			}
		};
		if (Batch)
			Batch->Add(std::move(Scanner), false, std::move(Complete));
		else
			Complete(Scanner, Scanner.Scan(Memory, Module));
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
