#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class IMemory;

namespace anduefker::analyzer
{

	/**
	 * @brief A sliding, buffered view over instruction bytes.
	 *
	 * Replaces handing out a raw pointer into a mapped file. A live process cannot
	 * produce such a pointer, so every decode read goes through IMemory::ReadBytes
	 * and is buffered here in large chunks - the per-instruction cost stays a
	 * bounds check and a pointer add, while the transport stays portable.
	 *
	 * A failed read returns nullptr, which is also how a scan discovers the end of
	 * readable content. That matters for a file image, where a segment's
	 * non-file-backed tail (.bss) deliberately fails rather than reading as zeros,
	 * and it means callers need no notion of "file size" to know where to stop -
	 * useful because .bss is not reliably the last thing in a module.
	 */
	class CodeWindow
	{
	public:
		/// 1 MB default: large enough that refills are rare against a hundreds-of-MB
		/// text segment, small enough to stay cache- and syscall-friendly at runtime.
		static constexpr size_t kDefaultChunk = 1u << 20;

		/// Floor for a derived chunk. Below this the refills cost more than the
		/// bytes they save, and a single instruction never needs anywhere near it.
		static constexpr size_t kMinChunk = 4096;

		/**
		 * @brief Chunk size for a walk known to cover at most Span bytes.
		 *
		 * Reading more than the walk can ever visit is pure waste, and on a
		 * syscall-backed backend it is waste that is paid for twice - once
		 * transferring the bytes and once holding the buffer. The default is right
		 * for the whole-image sweeps and badly wrong for the bounded ones: a
		 * FunctionBoundary scan covers at most 64 KB in both directions, so sizing
		 * the chunk to the actual span avoids refilling a multi-megabyte buffer
		 * many times over for a scan that only ever needed a fraction of it.
		 *
		 * @param Span Bytes the walk can visit; clamped into [kMinChunk, kDefaultChunk].
		 */
		static constexpr size_t ChunkForSpan(uint64_t Span)
		{
			if (Span >= kDefaultChunk)
				return kDefaultChunk;
			if (Span <= kMinChunk)
				return kMinChunk;
			return static_cast<size_t>(Span);
		}

		/**
		 * @param Memory    Backend to read through. Borrowed; must outlive the window.
		 * @param ChunkSize Bytes to buffer per refill. A bounded walk should pass
		 *        ChunkForSpan; the default suits a sweep with no known end.
		 */
		CodeWindow(const IMemory *Memory, size_t ChunkSize = kDefaultChunk);

		/**
		 * @brief Pointer to at least NeedBytes readable bytes at Addr.
		 *
		 * @return A pointer valid only until the next call, or nullptr when the bytes
		 *         cannot be read - which callers treat as "stop scanning here".
		 */
		const uint8_t *At(uint64_t Addr, size_t NeedBytes);

	private:
		/// Reloads the buffer around Addr, halving the request on failure so a scan
		/// stops exactly at unreadable content rather than a chunk early.
		bool Refill(uint64_t Addr, size_t NeedBytes);

		const IMemory *Memory_;

		std::vector<uint8_t> Buffer_;
		size_t ChunkSize_ = kDefaultChunk;

		/// Half-open [BufferStart_, BufferStart_ + Valid_) currently held.
		uint64_t BufferStart_ = 0;
		size_t Valid_ = 0;
	};

} // namespace anduefker::analyzer
