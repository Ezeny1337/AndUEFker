#include "CodeWindow.h"

#include <algorithm>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{

	CodeWindow::CodeWindow(const IMemory *Memory, size_t ChunkSize)
		: Memory_(Memory),
		  ChunkSize_(ChunkSize ? ChunkSize : kDefaultChunk)
	{
	}

	const uint8_t *CodeWindow::At(uint64_t Addr, size_t NeedBytes)
	{
		if (NeedBytes == 0)
			return nullptr;

		// Fast path: already buffered. This is the common case by a wide margin -
		// a sequential decode walks the buffer and only refills once per chunk.
		if (Valid_ && Addr >= BufferStart_ && Addr + NeedBytes <= BufferStart_ + Valid_)
			return Buffer_.data() + (Addr - BufferStart_);

		if (!Refill(Addr, NeedBytes))
			return nullptr;

		return Buffer_.data() + (Addr - BufferStart_);
	}

	bool CodeWindow::Refill(uint64_t Addr, size_t NeedBytes)
	{
		// Which way the caller is going decides where the window sits. A forward
		// scan wants everything ahead of the request; a backward one wants
		// everything behind it, and a forward-only window missed on every single
		// backward step - one full-chunk read per instruction, for a walk whose
		// whole range is a few kilobytes.
		//
		// Read before Valid_ is cleared below, or the test always sees an empty
		// window and every walk looks like a forward one.
		const bool bBackward = Valid_ != 0 && Addr < BufferStart_;

		Valid_ = 0;

		const size_t Want = std::max(ChunkSize_, NeedBytes);
		if (Buffer_.size() < Want)
			Buffer_.resize(Want);

		const size_t Behind = bBackward ? Want - NeedBytes : 0;
		uint64_t Base = Addr >= Behind ? Addr - Behind : 0;

		// Try a full chunk first, then shrink. The tail of a segment is shorter than
		// a chunk, and near the end of readable content a large read fails while the
		// bytes actually present still read fine - so back off rather than give up,
		// or a scan would stop up to a chunk early.
		for (;;)
		{
			// Everything the request itself needs, counted from the window's start.
			// size_t, not uint64_t: on a 32-bit build they are different types and
			// the buffer is sized in size_t anyway.
			const size_t Least = static_cast<size_t>(Addr - Base) + NeedBytes;

			size_t Size = Want;
			while (Size >= Least)
			{
				if (Memory_->ReadBytes(static_cast<uintptr_t>(Base), Buffer_.data(), Size))
				{
					BufferStart_ = Base;
					Valid_ = Size;
					return true;
				}
				if (Size == Least)
					break;
				Size = std::max(Least, Size / 2);
			}

			// Reading from behind the request can fail where reading from the request
			// itself succeeds - the bytes before it may be unmapped. Fall back to the
			// exact request rather than reporting the address unreadable.
			if (Base == Addr)
				return false;
			Base = Addr;
		}
	}

} // namespace anduefker::analyzer
