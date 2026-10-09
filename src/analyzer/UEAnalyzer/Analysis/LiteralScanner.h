#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class IMemory;
class MemRegionInfo;
class ModuleInfo;

namespace anduefker::analyzer
{
	/**
	 * @brief Finds many string literals in a single pass over memory.
	 *
	 * Searching every literal the analyzer looks for on its own - each anchor,
	 * each TCHAR probe, each resolution string, in ASCII *and* UTF-16 *and*
	 * UTF-32 - would mean dozens of separate passes over the image per
	 * Analyze(), each walking the module from the start.
	 *
	 * Against a mapped file that many passes are memchr over resident pages and
	 * merely slow. Against a live process each pass drags the module through the
	 * transport again, which is enough reads to make the analyzer unusable at
	 * runtime.
	 *
	 * So the needles are collected first and the bytes are walked once. Memory
	 * stays bounded: one chunk is held at a time, never a snapshot of the module,
	 * and chunks overlap by the longest needle minus one byte so a literal lying
	 * across a boundary is still found.
	 */
	class LiteralScanner
	{
	public:
		/// Bytes read per chunk. Matches CodeWindow's default for the same reasons:
		/// large enough that refills are rare, small enough to stay friendly to the
		/// cache and to a syscall-based transport.
		static constexpr size_t kChunkBytes = 1u << 20;

		/**
		 * @brief One literal to look for, in one encoding.
		 *
		 * Encodings are separate needles rather than a flag, because a caller wants
		 * to know *which* encoding matched - that is how TCHAR width is measured.
		 */
		struct Needle
		{
			std::vector<uint8_t> Bytes; ///< Encoded form to match.
			size_t OwnerIndex = 0;		///< Index of the literal this came from.
			int Encoding = 0;			///< 1 = ASCII, 2 = UTF-16LE, 4 = UTF-32LE.
			size_t MaxHits = 0;			///< Stop recording after this many; 0 = all.
		};

		/// Where one needle was found.
		struct Hit
		{
			uint64_t Address = 0;
			size_t NeedleIndex = 0;
		};

		/// @brief Adds a needle in all three encodings, returning its owner index.
		size_t AddLiteral(const char *Text, size_t MaxHitsPerEncoding = 0);

		/// @brief Adds a single pre-encoded needle.
		void AddRaw(const void *Data, size_t Size, size_t OwnerIndex, int Encoding, size_t MaxHits = 0);

		/// @brief True when nothing has been added.
		bool Empty() const { return Needles_.empty(); }

		/// @brief The needles, in the order they were added.
		const std::vector<Needle> &GetNeedles() const { return Needles_; }

		/**
		 * @brief Walks the module's readable ranges once, matching every needle.
		 *
		 * @return Hits in address order per range, needles interleaved.
		 */
		std::vector<Hit> Scan(const IMemory *Memory, const ModuleInfo &Module) const;

		/**
		 * @brief As Scan, but each segment gets its own hit budget.
		 *
		 * Two policies exist because callers mean different things by a cap. An
		 * anchor search wants N occurrences of a literal and does not care where
		 * they fall, so Scan spends one budget across the module. A tally used to
		 * weigh evidence caps per segment instead, so one literal-dense segment
		 * cannot dominate the count.
		 */
		std::vector<Hit> ScanPerSegment(const IMemory *Memory, const ModuleInfo &Module) const;

		/// @brief As Scan, restricted to one range.
		std::vector<Hit> ScanRange(const IMemory *Memory, uintptr_t Start, size_t Range) const;

		/// @brief Widens an ASCII literal to a wider character type, little-endian.
		template <typename TChar>
		static std::vector<uint8_t> Widen(const char *Text)
		{
			std::vector<uint8_t> Out;
			for (const char *P = Text; *P; ++P)
			{
				const TChar C = static_cast<TChar>(*P);
				const uint8_t *B = reinterpret_cast<const uint8_t *>(&C);
				Out.insert(Out.end(), B, B + sizeof(TChar));
			}
			return Out;
		}

	private:
		std::vector<Needle> Needles_;
		size_t NextOwner_ = 0;
	};

	/// Owns scan tasks until Run completes. Completion callbacks borrow their outputs;
	/// those outputs must outlive Run. Each task retains its own needle hit budgets.
	class LiteralScanBatch
	{
	public:
		struct Statistics
		{
			size_t RegisteredNeedles = 0;
			size_t UniqueNeedles = 0;
			uint64_t Comparisons = 0;
			uint64_t ReadOperations = 0;
			uint64_t ReadBytes = 0;
		};
		using Completion = std::function<void(const LiteralScanner &, const std::vector<LiteralScanner::Hit> &)>;
		void Add(LiteralScanner Scanner, bool PerSegment, Completion Complete);
		void Run(const IMemory *Memory, const ModuleInfo &Module);
		const Statistics &Stats() const { return Stats_; }

	private:
		struct Task
		{
			LiteralScanner Scanner;
			bool PerSegment = false;
			Completion Complete;
			std::vector<size_t> Counts;
			std::vector<LiteralScanner::Hit> Hits;
		};
		std::vector<Task> Tasks_;
		Statistics Stats_;
	};

} // namespace anduefker::analyzer
