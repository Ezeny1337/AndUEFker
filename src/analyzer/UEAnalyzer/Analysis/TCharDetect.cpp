#include "TCharDetect.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <utility>
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
		constexpr size_t kMaxPerSegment = 8;
		constexpr size_t kDecisiveHits = 16;

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

	void QueueTCharDetection(LiteralScanBatch &Batch, ETCharKind &Kind, size_t &Utf16, size_t &Utf32)
	{
		LiteralScanner Scanner;
		// The matcher shares patterns with all other consumers. Absence of an encoding
		// is established after all ranges; premature prefix decisions could miss a
		// conflicting encoding in a later segment. Preserve the reference probe order.
		for (size_t i = 0; i < std::size(kTCharProbes); ++i)
		{
			const auto W16 = LiteralScanner::Widen<uint16_t>(kTCharProbes[i]);
			const auto W32 = LiteralScanner::Widen<uint32_t>(kTCharProbes[i]);
			Scanner.AddRaw(W16.data(), W16.size(), i, 2, kMaxPerSegment);
			Scanner.AddRaw(W32.data(), W32.size(), i, 4, kMaxPerSegment);
		}
		Batch.Add(std::move(Scanner), true, [&](const LiteralScanner &Scanned, const std::vector<LiteralScanner::Hit> &Hits)
		{
			std::array<std::array<size_t, 2>, std::size(kTCharProbes)> Counts{};
			for (const auto &H : Hits)
			{
				const auto &N = Scanned.GetNeedles()[H.NeedleIndex];
				++Counts[N.OwnerIndex][N.Encoding == 2 ? 0 : 1];
			}
			Utf16 = 0;
			Utf32 = 0;
			for (const auto &C : Counts)
			{
				Utf16 += C[0];
				Utf32 += C[1];
				if ((Utf16 >= kDecisiveHits && Utf32 == 0) || (Utf32 >= kDecisiveHits && Utf16 == 0))
					break;
			}
			const int Width = StringAnchors::DecideTCharWidth(Utf16, Utf32);
			Kind = Width == 2 ? ETCharKind::Char16 : Width == 4 ? ETCharKind::Char32 : ETCharKind::Unknown;
		});
	}

	ETCharKind DetectTCharKind(const IMemory *Memory, size_t *OutUtf16, size_t *OutUtf32)
	{
		if (!Memory)
			return ETCharKind::Unknown;

		// GetUnrealModule is not const on IMemory, and this only reads - the cast
		// is confined to fetching the module description.
		const ModuleInfo Module = const_cast<IMemory *>(Memory)->GetUnrealModule();

		size_t Utf16 = 0, Utf32 = 0;

		LiteralScanBatch Batch;
		ETCharKind Kind = ETCharKind::Unknown;
		QueueTCharDetection(Batch, Kind, Utf16, Utf32);
		Batch.Run(Memory, Module);

		if (OutUtf16)
			*OutUtf16 = Utf16;
		if (OutUtf32)
			*OutUtf32 = Utf32;

		return Kind;
	}

} // namespace anduefker::analyzer
