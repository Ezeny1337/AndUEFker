#pragma once

#include <cstdint>

#include "../../Memory/IMemory.h"

class IArchDecoder;

namespace anduefker::analyzer
{

	/**
	 * @brief Half-open address range of one recovered function.
	 */
	struct FunctionBounds
	{
		uint64_t Start = 0; ///< Function entry.
		uint64_t End = 0;	///< Exclusive.

		bool IsValid() const { return Start != 0 && End > Start; }
		uint64_t Size() const { return IsValid() ? End - Start : 0; }
		bool Contains(uint64_t A) const { return IsValid() && A >= Start && A < End; }
	};

	/**
	 * @brief Finds the function enclosing an arbitrary instruction address.
	 *
	 * These binaries are stripped, so there is no function table to consult. The
	 * boundaries are recovered from the instruction stream: scanning backwards, the
	 * first return means the previous function ended there, and the first frame
	 * setup means this function began there - whichever is nearer wins. Scanning
	 * forwards, the first return ends the function.
	 *
	 * This is deliberately conservative. Over-reaching is the dangerous failure:
	 * it would pull in a neighbouring function's globals and produce a confident
	 * wrong answer, so an unbounded scan returns nothing instead.
	 */
	class FunctionBoundary
	{
	public:
		/**
		 * @param Memory Backend to read code from. Borrowed.
		 * @param Module Module being analysed. Borrowed, and must outlive this - a
		 *        boundary is built per anchor site, and fetching the module by value
		 *        each time copied its whole segment list on every one.
		 * @param Arch   Decoder for the module's architecture.
		 */
		FunctionBoundary(const IMemory *Memory, const ModuleInfo &Module, const IArchDecoder &Arch)
			: Memory_(Memory),
			  Module_(Module),
			  Arch_(Arch)
		{
		}

		/**
		 * @brief Recovers the bounds of the function containing Address.
		 *
		 * Both ends must be found, so the limit has to clear the *whole* function
		 * from wherever inside it the caller is looking - not half of one. A large
		 * outlined constructor can span tens of kilobytes with anchor sites well
		 * short of its closing return; too small a limit misses the boundary
		 * entirely, drops the sites before clustering, and makes the function
		 * invisible to the anchor that names it.
		 *
		 * @param Address Any instruction inside the function.
		 * @param MaxScan How far to scan in each direction before giving up.
		 * @return Invalid bounds when no boundary is found inside MaxScan bytes.
		 */
		FunctionBounds Find(uint64_t Address, uint32_t MaxScan = 0x8000) const;

	private:
		const IMemory *Memory_;
		const ModuleInfo &Module_;
		const IArchDecoder &Arch_;
	};

} // namespace anduefker::analyzer
