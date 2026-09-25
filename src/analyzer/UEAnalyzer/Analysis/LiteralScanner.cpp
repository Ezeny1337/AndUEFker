#include "LiteralScanner.h"

#include <algorithm>
#include <cstring>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{
	size_t LiteralScanner::AddLiteral(const char *Text, size_t MaxHitsPerEncoding)
	{
		const size_t Owner = NextOwner_++;
		if (!Text || !*Text)
			return Owner;

		const size_t Length = std::strlen(Text);
		AddRaw(Text, Length, Owner, 1, MaxHitsPerEncoding);

		const std::vector<uint8_t> W16 = Widen<uint16_t>(Text);
		AddRaw(W16.data(), W16.size(), Owner, 2, MaxHitsPerEncoding);

		const std::vector<uint8_t> W32 = Widen<uint32_t>(Text);
		AddRaw(W32.data(), W32.size(), Owner, 4, MaxHitsPerEncoding);

		return Owner;
	}

	void LiteralScanner::AddRaw(const void *Data, size_t Size, size_t OwnerIndex, int Encoding, size_t MaxHits)
	{
		if (!Data || Size == 0)
			return;

		Needle N;
		N.Bytes.assign(static_cast<const uint8_t *>(Data), static_cast<const uint8_t *>(Data) + Size);
		N.OwnerIndex = OwnerIndex;
		N.Encoding = Encoding;
		N.MaxHits = MaxHits;

		LongestNeedle_ = std::max(LongestNeedle_, Size);

		// Bucketed on the way in, so scanning never has to group them.
		const uint8_t First = N.Bytes[0];
		const uint32_t Index = static_cast<uint32_t>(Needles_.size());
		auto At = std::find_if(Buckets_.begin(), Buckets_.end(), [First](const FirstByteBucket &B)
							   { return B.First == First; });
		if (At == Buckets_.end())
		{
			Buckets_.push_back(FirstByteBucket{First, {}});
			At = Buckets_.end() - 1;
		}
		At->Needles.push_back(Index);

		Needles_.push_back(std::move(N));
		NextOwner_ = std::max(NextOwner_, OwnerIndex + 1);
	}

	void LiteralScanner::MatchChunk(const uint8_t *Data, size_t Size, size_t LimitOffset, uint64_t BaseAddress, std::vector<size_t> &FoundPerNeedle, std::vector<Hit> &Out) const
	{
		if (LimitOffset == 0)
			return;

		// One memchr sweep per distinct first byte, not one per needle. All three
		// encodings of a literal begin with the same byte - "None", "N\0o\0n\0e\0"
		// and "N\0\0\0..." all start 'N' - so a set of 7 anchors in 3 encodings is
		// 21 needles but only 7 sweeps, and each byte of the image is read by memchr
		// a third as often.
		for (const FirstByteBucket &Bucket : Buckets_)
		{
			// The shortest needle here bounds how late a match can still start; the
			// per-needle test below rejects the longer ones that no longer fit.
			size_t Shortest = Size;
			for (uint32_t n : Bucket.Needles)
				Shortest = std::min(Shortest, Needles_[n].Bytes.size());
			if (Shortest > Size)
				continue;

			// A match starting at or past LimitOffset lies inside the overlap the next
			// chunk will re-read, and would otherwise be recorded twice - inflating the
			// encoding counts and burning the hit budget on duplicates.
			const size_t Last = std::min(Size - Shortest, LimitOffset - 1);

			for (size_t Off = 0; Off <= Last;)
			{
				const void *Found = std::memchr(Data + Off, Bucket.First, Last - Off + 1);
				if (!Found)
					break;
				const size_t At = static_cast<size_t>(static_cast<const uint8_t *>(Found) - Data);

				for (uint32_t n : Bucket.Needles)
				{
					const Needle &N = Needles_[n];
					if (At + N.Bytes.size() > Size)
						continue;
					if (N.MaxHits && FoundPerNeedle[n] >= N.MaxHits)
						continue;
					if (std::memcmp(Data + At, N.Bytes.data(), N.Bytes.size()) == 0)
					{
						Out.push_back(Hit{BaseAddress + At, n});
						++FoundPerNeedle[n];
					}
				}
				Off = At + 1;
			}
		}
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::ScanRange(const IMemory *Memory, uintptr_t Start, size_t Range) const
	{
		std::vector<Hit> Out;
		std::vector<size_t> FoundPerNeedle(Needles_.size(), 0);
		ScanRangeInto(Memory, Start, Range, FoundPerNeedle, Out);
		return Out;
	}

	void LiteralScanner::ScanRangeInto(const IMemory *Memory, uintptr_t Start, size_t Range, std::vector<size_t> &FoundPerNeedle, std::vector<Hit> &Out) const
	{
		if (!Memory || Needles_.empty() || Range == 0)
			return;

		std::vector<uint8_t> Buffer;

		// The backend decides what is actually readable, so a hole in the middle of
		// the request costs one lookup rather than a failed read per chunk.
		for (const MemRegionInfo &Sub : Memory->BuildSegmentsRanges(Start, Range))
		{
			const uintptr_t SubEnd = Sub.GetEnd();
			// Overlap so a literal straddling a chunk boundary is still matched.
			const size_t Overlap = LongestNeedle_ > 1 ? LongestNeedle_ - 1 : 0;

			for (uintptr_t Cursor = Sub.GetStart(); Cursor < SubEnd;)
			{
				const size_t Want = std::min<size_t>(kChunkBytes, static_cast<size_t>(SubEnd - Cursor));
				Buffer.resize(Want);
				if (!Memory->ReadBytes(Cursor, Buffer.data(), Want))
					break; // readable content ended early; the rest of this range is not ours

				const bool bLast = Want <= Overlap || (Cursor + Want) >= SubEnd;
				const size_t Advance = bLast ? Want : Want - Overlap;
				MatchChunk(Buffer.data(), Want, bLast ? Want : Advance, static_cast<uint64_t>(Cursor), FoundPerNeedle, Out);

				if (bLast)
					break;
				Cursor += Advance;
			}
		}
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::Scan(const IMemory *Memory, const ModuleInfo &Module) const
	{
		std::vector<Hit> Out;
		if (!Memory || Needles_.empty())
			return Out;

		// One budget for the whole module, not one per segment: the caller asks for N
		// occurrences of a literal, and where they fall is not its concern.
		std::vector<size_t> FoundPerNeedle(Needles_.size(), 0);

		// Per segment rather than one module-wide range: a module's span can be
		// enormous while its segments are not, and BuildSegmentsRanges would have to
		// probe every hole in between.
		for (const MemRegionInfo &Seg : Module.GetSegments())
		{
			if (Seg.GetSize() == 0)
				continue;
			ScanRangeInto(Memory, Seg.GetStart(), Seg.GetSize(), FoundPerNeedle, Out);
		}
		return Out;
	}

	std::vector<LiteralScanner::Hit> LiteralScanner::ScanPerSegment(const IMemory *Memory, const ModuleInfo &Module) const
	{
		std::vector<Hit> Out;
		if (!Memory || Needles_.empty())
			return Out;

		for (const MemRegionInfo &Seg : Module.GetSegments())
		{
			if (Seg.GetSize() == 0)
				continue;
			// Budget reset per segment - that is the whole difference from Scan.
			std::vector<size_t> FoundPerNeedle(Needles_.size(), 0);
			ScanRangeInto(Memory, Seg.GetStart(), Seg.GetSize(), FoundPerNeedle, Out);
		}
		return Out;
	}

} // namespace anduefker::analyzer
