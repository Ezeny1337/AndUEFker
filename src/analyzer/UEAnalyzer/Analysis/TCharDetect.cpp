#include "TCharDetect.h"

#include <cstdint>
#include <vector>

#include "LiteralScanner.h"
#include "StringAnchors.h"

namespace anduefker::analyzer
{
	namespace
	{

		/**
		 * @brief Literals used only to measure TCHAR width.
		 *
		 * Chosen for ubiquity, not for locating anything: every one is emitted by
		 * engine code that ships in any UE title, so this answers even on a binary
		 * whose target anchors are all absent - which does happen, and would
		 * otherwise report "undetermined" for want of looking rather than for want
		 * of evidence.
		 *
		 * Short strings are avoided: a two-character literal widened to UTF-32 is
		 * eight bytes of mostly zeros, which matches padding all over the image.
		 */
		const char *const kTCharProbes[] = {
			"None",
			"IntProperty",
			"ByteProperty",
			"Engine",
			"/Script/CoreUObject",
			"/Script/Engine",
			"/Game/",
			"PersistentLevel",
			"DefaultEngine.ini",
			"SkeletalMesh",
			"Transient",
			"bHidden",
		};

	} // namespace

	const char *ETCharKindToString(ETCharKind Kind)
	{
		switch (Kind)
		{
		case ETCharKind::Char16:
			return "char16_t";
		case ETCharKind::Char32:
			return "char32_t";
		default:
			return "unknown";
		}
	}

	ETCharKind DetectTCharKind(const IMemory *Memory, size_t *OutUtf16, size_t *OutUtf32)
	{
		if (!Memory)
			return ETCharKind::Unknown;

		// GetUnrealModule is not const on IMemory, and this only reads - the cast
		// is confined to fetching the module description.
		const ModuleInfo Module = const_cast<IMemory *>(Memory)->GetUnrealModule();

		size_t Utf16 = 0, Utf32 = 0;
		constexpr size_t kMaxPerSegment = 8;

		// Enough hits in one encoding, none in the other, is as settled as this
		// question gets in practice: a real build shows a landslide in its actual
		// encoding and nothing in the other, so a decisive lead lets the scan stop
		// after one or two probes instead of all of them.
		constexpr size_t kDecisiveHits = 16;

		for (const char *Probe : kTCharProbes)
		{
			// Both encodings of a probe are matched in one walk of the module rather
			// than one walk each. The early exit above is why the probes are not all
			// batched together: it usually settles the question on the first probe,
			// and batching would trade two passes for one at the cost of always
			// scanning for all twelve.
			//
			// Executable segments are included deliberately: a typical Android .so has
			// two PT_LOADs, so .rodata - and every string literal with it - lives
			// inside the r-x segment.
			LiteralScanner Scanner;
			const std::vector<uint8_t> W16 = LiteralScanner::Widen<uint16_t>(Probe);
			const std::vector<uint8_t> W32 = LiteralScanner::Widen<uint32_t>(Probe);
			Scanner.AddRaw(W16.data(), W16.size(), 0, 2, kMaxPerSegment);
			Scanner.AddRaw(W32.data(), W32.size(), 0, 4, kMaxPerSegment);

			for (const LiteralScanner::Hit &H : Scanner.ScanPerSegment(Memory, Module))
			{
				if (Scanner.GetNeedles()[H.NeedleIndex].Encoding == 2)
					++Utf16;
				else
					++Utf32;
			}

			if ((Utf16 >= kDecisiveHits && Utf32 == 0) || (Utf32 >= kDecisiveHits && Utf16 == 0))
				break;
		}

		if (OutUtf16)
			*OutUtf16 = Utf16;
		if (OutUtf32)
			*OutUtf32 = Utf32;

		// A UTF-32 literal contains its own UTF-16 form: "None" as UTF-32 is
		// 4E 00 00 00 6F 00 00 00 ..., whose first bytes read as UTF-16 "N\0o\0" is
		// not a match, but shorter overlaps do occur. The shared majority rule
		// handles that the same way it handles stray hits from padding: a clear
		// majority is required, otherwise the answer is "undetermined".
		switch (StringAnchors::DecideTCharWidth(Utf16, Utf32))
		{
		case 2:
			return ETCharKind::Char16;
		case 4:
			return ETCharKind::Char32;
		default:
			return ETCharKind::Unknown;
		}
	}

} // namespace anduefker::analyzer
