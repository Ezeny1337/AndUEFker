#pragma once

#include <cstddef>
#include <cstdint>

#include "../../Memory/IMemory.h"

namespace anduefker::analyzer
{

	/**
	 * @brief What a character is in this build.
	 *
	 * UE's `TCHAR` is either `char16_t` (`PLATFORM_TCHAR_IS_CHAR16`) or a 4-byte
	 * `wchar_t`, and which one it is decides how to read an `FString` - which is a
	 * `TArray<TCHAR>`, so its buffer is 2 bytes per character or 4. Reading it the
	 * wrong way produces garbage rather than an error.
	 *
	 * This is a property of the *build*, not of the engine version: two titles
	 * on the same engine version can disagree, so no version rule is reliable.
	 */
	enum class ETCharKind
	{
		Unknown, ///< Not decisive. Do not infer a width from this.
		Char16,	 ///< TCHAR is char16_t: wide string data is 2 bytes per character.
		Char32,	 ///< TCHAR is a 4-byte wchar_t.
	};

	/// @brief Name of a TCHAR kind, for reporting.
	const char *ETCharKindToString(ETCharKind Kind);

	/**
	 * @brief Measures TCHAR width from the engine's own string literals.
	 *
	 * A `TEXT("...")` literal in read-only data *is* a `TCHAR[]`, so finding the
	 * same literal as a UTF-16 array means `char16_t` and as a UTF-32 array means
	 * `wchar_t`. That is a direct measurement, not an inference from the version.
	 *
	 * ASCII hits are deliberately not counted: they occur in every build whatever
	 * TCHAR is - symbol tables, ANSI FNameEntry data, format strings passed to
	 * narrow printf - so they carry no information about it.
	 *
	 * The literals searched are chosen for being ubiquitous rather than for
	 * locating anything, so this still answers on a binary whose target anchors
	 * are absent. Deliberately separate from the proximity anchors for that
	 * reason.
	 *
	 * @param OutUtf16 Receives the number of UTF-16 literal hits, for reporting.
	 * @param OutUtf32 Receives the number of UTF-32 literal hits, for reporting.
	 * @return The measured kind, or Unknown when neither encoding clearly wins.
	 */
	ETCharKind DetectTCharKind(const IMemory *Memory, size_t *OutUtf16 = nullptr, size_t *OutUtf32 = nullptr);

} // namespace anduefker::analyzer
