#pragma once

#include <cstdint>

namespace anduefker::analyzer
{

	/// NumElementsPerChunk in UE 4.20: 66560 / 0x10400.
	inline constexpr uint32_t kNumElementsPerChunk_4_20 = 65u * 1024u;

	/**
	 * @brief NumElementsPerChunk from UE 4.21: 65536 / 0x10000.
	 *
	 * FNameEntryAllocator uses the same value for its block size, so the constant
	 * alone does not say which structure is nearby - it has to be seen near the
	 * object array's own access sites to mean anything.
	 */
	inline constexpr uint32_t kNumElementsPerChunk_4_21 = 64u * 1024u;

	/// How far above a candidate address a struct field may sit, for the header
	/// window a structure check walks. FUObjectArray and FNamePool headers are
	/// both well inside this; shared because both strategies scan the same
	/// shape of header.
	inline constexpr int64_t kStructHeaderWindow = 0x20;

} // namespace anduefker::analyzer
