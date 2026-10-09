#include "LiteralScanner.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{
	size_t LiteralScanner::AddLiteral(const char *Text, size_t MaxHitsPerEncoding)
	{
		if (NextOwner_ == SIZE_MAX) throw std::length_error("literal owner index overflow");
		const size_t Owner = NextOwner_++;
		if (!Text || !*Text) return Owner;
		const size_t Length = std::strlen(Text);
		if (Length > kChunkBytes / 4) throw std::length_error("literal exceeds scan chunk size");
		AddRaw(Text, Length, Owner, 1, MaxHitsPerEncoding);
		const auto W16 = Widen<uint16_t>(Text);
		const auto W32 = Widen<uint32_t>(Text);
		AddRaw(W16.data(), W16.size(), Owner, 2, MaxHitsPerEncoding);
		AddRaw(W32.data(), W32.size(), Owner, 4, MaxHitsPerEncoding);
		return Owner;
	}

	void LiteralScanner::AddRaw(const void *Data, size_t Size, size_t OwnerIndex, int Encoding, size_t MaxHits)
	{
		if (!Data || !Size) return;
		if (Size > kChunkBytes || OwnerIndex == SIZE_MAX) throw std::length_error("literal registration exceeds scan limits");
		Needle N;
		N.Bytes.assign(static_cast<const uint8_t *>(Data), static_cast<const uint8_t *>(Data) + Size);
		N.OwnerIndex = OwnerIndex;
		N.Encoding = Encoding;
		N.MaxHits = MaxHits;
		Needles_.push_back(std::move(N));
		NextOwner_ = std::max(NextOwner_, OwnerIndex + 1);
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::Scan(const IMemory *Memory, const ModuleInfo &Module) const
	{
		std::vector<Hit> Out;
		LiteralScanBatch Batch;
		Batch.Add(*this, false, [&](const auto &, const auto &Hits) { Out = Hits; });
		Batch.Run(Memory, Module);
		return Out;
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::ScanPerSegment(const IMemory *Memory, const ModuleInfo &Module) const
	{
		std::vector<Hit> Out;
		LiteralScanBatch Batch;
		Batch.Add(*this, true, [&](const auto &, const auto &Hits) { Out = Hits; });
		Batch.Run(Memory, Module);
		return Out;
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::ScanRange(const IMemory *Memory, uintptr_t Start, size_t Range) const
	{
		if (!Range || Range > UINTPTR_MAX - Start) return {};
		return Scan(Memory, ModuleInfo({}, Start, Start + Range, Start,
			{MemRegionInfo({}, Start, Start + Range, 0, true)}));
	}

	void LiteralScanBatch::Add(LiteralScanner Scanner, bool PerSegment, Completion Complete)
	{
		Task T;
		T.Counts.resize(Scanner.GetNeedles().size());
		T.Scanner = std::move(Scanner);
		T.PerSegment = PerSegment;
		T.Complete = std::move(Complete);
		Tasks_.push_back(std::move(T));
	}

	void LiteralScanBatch::Run(const IMemory *Memory, const ModuleInfo &Module)
	{
		// Patterns borrow immutable task bytes. Detaching tasks keeps registration and
		// completion callbacks from invalidating those bytes or their consumer indices.
		std::vector<Task> Tasks;
		Tasks.swap(Tasks_);
		Statistics Stats;
		struct Consumer { size_t Task, Needle; };
		struct Pattern { const std::vector<uint8_t> *Bytes; std::vector<Consumer> Consumers; size_t ActiveConsumers = 0; };
		std::vector<Pattern> Patterns;
		std::unordered_map<std::string_view, size_t> Registered;
		std::array<std::vector<size_t>, 256> Buckets;
		for (size_t TaskIndex = 0; TaskIndex < Tasks.size(); ++TaskIndex)
		{
			const auto &Needles = Tasks[TaskIndex].Scanner.GetNeedles();
			for (size_t NeedleIndex = 0; NeedleIndex < Needles.size(); ++NeedleIndex)
			{
				const auto &Bytes = Needles[NeedleIndex].Bytes;
				++Stats.RegisteredNeedles;
				const auto [At, Added] = Registered.emplace(std::string_view(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()), Patterns.size());
				if (Added)
				{
					Buckets[Bytes.front()].push_back(Patterns.size());
					Patterns.push_back({&Bytes, {}});
				}
				Patterns[At->second].Consumers.push_back({TaskIndex, NeedleIndex});
				++Patterns[At->second].ActiveConsumers;
			}
		}
		Stats.UniqueNeedles = Patterns.size();
		const auto Needs = [](const Pattern &P) { return P.ActiveConsumers != 0; };
		std::vector<uint8_t> Buffer;
		if (Memory && !Patterns.empty())
			for (const auto &Seg : Module.GetSegments())
			{
				if (!Seg.GetSize()) continue;
				for (auto &T : Tasks)
					if (T.PerSegment) std::fill(T.Counts.begin(), T.Counts.end(), 0);
				for (auto &P : Patterns)
				{
					P.ActiveConsumers = 0;
					for (const auto &C : P.Consumers)
					{
						const auto &T = Tasks[C.Task];
						const auto Cap = T.Scanner.GetNeedles()[C.Needle].MaxHits;
						P.ActiveConsumers += !Cap || T.Counts[C.Needle] < Cap ? 1u : 0u;
					}
				}
				for (const auto &Sub : Memory->BuildSegmentsRanges(Seg.GetStart(), Seg.GetSize()))
				{
					if (Sub.GetStart() < Seg.GetStart() || Sub.GetEnd() > Seg.GetEnd()) continue;
					for (uintptr_t Cursor = Sub.GetStart(); Cursor < Sub.GetEnd();)
					{
						size_t Longest = 0;
						for (const auto &P : Patterns) if (Needs(P)) Longest = std::max(Longest, P.Bytes->size());
						if (!Longest) break;
						const size_t Overlap = Longest - 1;
						const size_t Want = std::min<size_t>(LiteralScanner::kChunkBytes, Sub.GetEnd() - Cursor);
						Buffer.resize(Want);
						const size_t Received = Memory->ReadBytes(Cursor, Buffer.data(), Want);
						++Stats.ReadOperations;
						if (!Received || Received > Want) break;
						Stats.ReadBytes += Received;
						const bool Last = Received < Want || Received <= Overlap || Received == Sub.GetEnd() - Cursor;
						const size_t Advance = Last ? Received : Received - Overlap;
						for (size_t First = 0; First < Buckets.size(); ++First)
						{
							std::vector<size_t> Active;
							size_t Shortest = Received + 1;
							for (const auto Index : Buckets[First])
								if (Needs(Patterns[Index]))
								{ Active.push_back(Index); Shortest = std::min(Shortest, Patterns[Index].Bytes->size()); }
							if (Shortest > Received) continue;
							const size_t FinalOffset = std::min(Received - Shortest, Advance - 1);
							for (size_t Off = 0; Off <= FinalOffset && !Active.empty();)
							{
								const auto *Found = static_cast<const uint8_t *>(std::memchr(Buffer.data() + Off, static_cast<int>(First), FinalOffset - Off + 1));
								if (!Found) break;
								const size_t At = static_cast<size_t>(Found - Buffer.data());
								bool Retired = false;
								for (const auto Index : Active)
								{
									auto &P = Patterns[Index];
									if (P.Bytes->size() > Received - At) continue;
									++Stats.Comparisons;
									if (std::memcmp(Found, P.Bytes->data(), P.Bytes->size())) continue;
									for (const auto &C : P.Consumers)
									{
										auto &T = Tasks[C.Task];
										const auto Cap = T.Scanner.GetNeedles()[C.Needle].MaxHits;
										if (Cap && T.Counts[C.Needle] >= Cap) continue;
										T.Hits.push_back({Cursor + At, C.Needle});
										++T.Counts[C.Needle];
										if (Cap && T.Counts[C.Needle] == Cap) --P.ActiveConsumers;
									}
									Retired = Retired || !Needs(P);
								}
								if (Retired) std::erase_if(Active, [&](size_t Index) { return !Needs(Patterns[Index]); });
								Off = At + 1;
							}
						}
						if (Last) break;
						Cursor += Advance;
					}
				}
			}
		Stats_ = Stats;
		for (auto &T : Tasks)
		{
			std::sort(T.Hits.begin(), T.Hits.end(), [](const auto &A, const auto &B)
			{ return A.Address != B.Address ? A.Address < B.Address : A.NeedleIndex < B.NeedleIndex; });
			if (T.Complete) T.Complete(T.Scanner, T.Hits);
		}
	}
}
