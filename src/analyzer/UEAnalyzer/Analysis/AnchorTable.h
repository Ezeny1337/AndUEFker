#pragma once

#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <cstdint>

#include "../Core/Candidate.h"

namespace anduefker::analyzer
{

	/**
	 * @brief How to choose the global once the anchored function is known.
	 */
	enum class EPickRule : uint8_t
	{
		/// The target is a member of a container whose address the code takes. The
		/// member offset is derived from the function's own accesses, never assumed -
		/// FUObjectArray's layout shifts between engine versions and forks.
		ContainerMember,

		/// The target *is* that container - the object whose address the code takes
		/// and whose member the rule above would have returned.
		///
		/// The member is still derived, and still has to be found, because a global
		/// that holds no array is not the object array's container. The derivation is
		/// the proof; the container is the answer. This is what obfuscated builds
		/// need: the container can be far more heavily referenced than the array
		/// inside it, so the container is findable in binaries where its member is
		/// not.
		Container,

		/// Exactly one global in the anchored function survives structure
		/// verification. Deliberately not "the most accessed one" - frequency alone
		/// is not a reliable signal for which global is the target. If two or more
		/// survive, or none, the anchor yields nothing rather than choosing.
		SingleVerified,

		/// The target is the single writable global the function touches. Strongest
		/// when it applies, and it often does: a median anchored function touches
		/// only two.
		SingleWritable,

		/// The target is arg0 of the calls *into* the anchored function - the object
		/// itself, at offset 0, not a member of one.
		///
		/// The mirror of CalleeArgument, and the shape a pool build actually has:
		/// FNamePool::FNamePool receives the pool in X0, so the evidence is on the
		/// caller side. This is the only rule that has anything to work with for a
		/// constructor that touches, stores and passes nothing of its own.
		CallerArgSelf,

		/// The target is passed as the first argument to something the anchored
		/// function calls.
		///
		/// This is how the name table is actually reached. FName::StaticInit does not
		/// touch the pool at all - it builds each FName on the stack and hands it to a
		/// helper, so the disassembly reads
		///
		///     ADRL X1, dword_...        ; the UTF-32 name data
		///     SUB  X0, X29, #-var_48    ; a *stack local*, not the pool
		///     BL   FName::Init
		///
		/// while FNamePool::FNamePool(this) receives the pool itself in X0. Looking
		/// for a global inside the anchored function therefore finds the wrong
		/// thing - a legacy build's stack-built FName stores nothing useful nearby.
		/// Following the call argument finds the real one, and stack locals are
		/// rejected for free by the writable-global test.
		CalleeArgument,
	};

	/**
	 * @brief A string that identifies a function which must touch the target.
	 *
	 * This is what replaces byte patterns. A pattern encodes one compiler's output
	 * for one build; a string encodes what the engine *says*, which survives
	 * recompilation, inlining and optimisation-level changes.
	 *
	 * Trust values reflect how broadly and reliably each anchor resolves, so
	 * ordering by trust means trying the most broadly present evidence first.
	 *
	 * Owned by the strategy for the target it identifies - see
	 * IStrategy::ResolutionAnchors - so there is no central table to keep in step
	 * with a list of target kinds.
	 */
	/**
	 * @brief How an anchor's literal must match in memory.
	 */
	enum class EAnchorMatch : uint8_t
	{
		/// Anywhere in the bytes. Right for format strings and command names.
		Substring,

		/**
		 * @brief The literal plus its own NUL, with nothing required before it.
		 *
		 * For short vocabulary that also occurs inside longer strings and mangled
		 * symbols, where substring matching finds far more false hits than real
		 * literals. Requiring a *leading* NUL as well finds nothing at all:
		 * literals are packed back to back and alignment padding means the byte
		 * before one is often not a NUL.
		 */
		Terminated,
	};

	struct Anchor
	{
		const char *Text;							  ///< Literal as the engine emits it.
		float Trust;								  ///< How broadly and reliably this anchor resolves, in [0,1].
		EPickRule Rule;								  ///< How to choose the global once the function is known.
		const char *Function;						  ///< The UE function it identifies, for explainability.
		EAnchorMatch Match = EAnchorMatch::Substring; ///< How the literal must match.
	};

	/**
	 * @brief A string whose *neighbourhood* suggests a target.
	 *
	 * Deliberately a different type from Anchor above, because the two are scored
	 * on different things. An Anchor has to resolve to the exact address; an
	 * AnchorString only has to mark the right part of the image. A string can be
	 * a poor resolver and a good proximity marker - the FName statistics format
	 * strings are exactly that - so pruning the proximity list by resolution
	 * precision would cost the name table its ranking signal outright.
	 *
	 * Each strategy owns both lists for its own target; there is no shared table.
	 */
	struct AnchorString
	{
		const char *Text; ///< Literal as the engine emits it.
		float Weight;	  ///< Measured trust, in [0,1].
	};

} // namespace anduefker::analyzer
