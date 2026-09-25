#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../../Memory/IMemory.h"
#include "../Core/UEConstants.h"

class IArchDecoder;

namespace anduefker::analyzer
{
	class CallGraph;

	/**
	 * @brief How a global's address was formed at a given site.
	 */
	enum class EAccessKind : uint8_t
	{
		/// The address itself was materialized into a register - "address taken".
		/// On AArch64 that is ADRP+ADD or a bare ADR.
		AddressTaken = 1 << 0,

		/// LDR through a computed base - the global is being read.
		Load = 1 << 1,

		/// STR through a computed base - the global is being written.
		Store = 1 << 2,

		/**
		 * The address was read out of a relocated pointer slot rather than computed.
		 *
		 * This is a distinct way of *naming* a global, not of using one, and it has
		 * to be recorded or the object is invisible as a container. Several UE builds
		 * reach GUObjectArray this way:
		 *
		 *      ADRP X8, #page_of_slot
		 *      LDR  X8, [X8, #slot]      ; X8 = *slot = GUObjectArray  <- IndirectBase
		 *      LDR  X9, [X8, #0x10]      ; ObjObjects
		 *
		 * Recording only the slot would leave the container itself absent from the
		 * access map, so anchored resolution could not derive the member from it.
		 */
		IndirectBase = 1 << 3,
	};

	/**
	 * @brief A small set of scalars kept as a sorted vector.
	 *
	 * Chosen over std::unordered_set for footprint, not speed: an image yields
	 * ~200k AccessInfo entries and each held two node-based sets, every non-empty
	 * one of which paid a bucket array plus a node allocation. These sets hold a
	 * handful of values, are capped, and are read far more often than written, so
	 * a sorted vector is both smaller and faster to query.
	 */
	template <typename T>
	class SmallSet
	{
	public:
		/// @brief Inserts Value if absent. @return true when it was added.
		bool insert(T Value)
		{
			const auto At = std::lower_bound(Values_.begin(), Values_.end(), Value);
			if (At != Values_.end() && *At == Value)
				return false;
			Values_.insert(At, Value);
			return true;
		}

		/// @brief 1 when Value is present, 0 otherwise. Named to match std::set.
		size_t count(T Value) const
		{
			const auto At = std::lower_bound(Values_.begin(), Values_.end(), Value);
			return (At != Values_.end() && *At == Value) ? 1u : 0u;
		}

		size_t size() const { return Values_.size(); } ///< @brief Element count.
		bool empty() const { return Values_.empty(); } ///< @brief True when none.

		/// Ascending iteration, which a hash set could not promise.
		typename std::vector<T>::const_iterator begin() const { return Values_.begin(); }
		typename std::vector<T>::const_iterator end() const { return Values_.end(); }

	private:
		std::vector<T> Values_;
	};

	/**
	 * @brief Everything observed about one candidate global address.
	 */
	struct AccessInfo
	{
		uint32_t Count = 0; ///< Total observations, including those beyond the site cap.
		uint8_t Kinds = 0;	///< Bitmask of EAccessKind.

		/// Instruction addresses that form or use this global. Capped: a hot global
		/// can have tens of thousands of sites and we only need enough to correlate.
		std::vector<uint64_t> Sites;

		/// Immediates seen shortly before/after a site (MOV/MOVZ/MOVK/CMP). Used to
		/// spot NumElementsPerChunk without hardcoding an offset or a byte pattern.
		SmallSet<uint32_t> NearbyConstants;

		/// Displacements this address was reached by, relative to whatever base the
		/// compiler used. A struct member is reached at several distinct offsets from
		/// several bases; a scalar pointer is reached at one. Capped, since a hot
		/// global would otherwise accumulate thousands of entries.
		SmallSet<int64_t> Offsets;

		/// Access widths in bytes, split narrow (<= 4) vs pointer-width (8). Element
		/// and chunk counts are 32-bit while the tables themselves are 64-bit, so the
		/// mix says something about the shape of what lives here.
		uint32_t NarrowAccesses = 0;
		uint32_t WideAccesses = 0;

		/// True when this global was ever reached in the given way.
		bool HasKind(EAccessKind K) const { return (Kinds & static_cast<uint8_t>(K)) != 0; }

		/// How many different EAccessKind forms were observed.
		int DistinctKinds() const
		{
			int N = 0;
			for (uint8_t B = 1; B; B <<= 1)
				if (Kinds & B)
					++N;
			return N;
		}
	};

	/// Every candidate global in an image, keyed by address.
	using AccessMap = std::unordered_map<uint64_t, AccessInfo>;

	/**
	 * @brief Tuning for one harvest pass.
	 */
	struct HarvestOptions
	{
		/// Maximum distinct offsets recorded per global.
		uint32_t MaxOffsetsPerGlobal = 64;
		/// Maximum recorded sites per global.
		uint32_t MaxSitesPerGlobal = 512;
		/// How far after a base-forming instruction its completing add or access
		/// may appear.
		uint32_t PairWindow = 16;
		/// How many instructions of constant context to keep around a site.
		uint32_t ConstantWindow = 12;
		/// Drop computed addresses that land in executable memory.
		///
		/// Off by default, and deliberately so: a typical Android .so has only two
		/// PT_LOADs, which puts .rodata - and therefore every string literal - inside
		/// the r-x segment. Filtering on executability here discards all string
		/// references, which are what the anchor correlation depends on. Candidate
		/// filtering happens later, in IsPlausibleGlobalLocation.
		bool RequireNonExecutableTarget = false;
		/// Follow `LDR Xt,[Xn,#off]` through relocated pointer slots. Without this
		/// the most common real-world access form cannot be resolved statically.
		bool FollowIndirect = true;
	};

	/**
	 * @brief Enumerates every global address that executable code computes.
	 *
	 * This replaces "find one byte pattern". Instead of searching for a known shape,
	 * it decodes all executable bytes once and records what each base-rooted address
	 * computation actually points at, tracking register values forward so that the
	 * hoisted-base idiom resolves. For example on AArch64:
	 *
	 *      ADRP X23, #page          <- base established, possibly hoisted far away
	 *      ...                      <- dozens of unrelated instructions
	 *      LDR  X20, [X23, #disp]   <- the real access
	 *
	 * The pass itself knows none of that: it works on NormalizedInsn from an
	 * IArchDecoder, so another architecture only needs a new analyzer.
	 *
	 * Register tracking is invalidated on redefinition and at control-flow joins, so
	 * a stale base cannot manufacture a bogus global.
	 */
	class GlobalAccessHarvester
	{
	public:
		GlobalAccessHarvester();

		/// Defined out of line: the unique_ptr member holds a forward-declared type,
		/// so the destructor must be emitted where IArchDecoder is complete.
		~GlobalAccessHarvester();

		GlobalAccessHarvester(GlobalAccessHarvester &&) noexcept;
		GlobalAccessHarvester &operator=(GlobalAccessHarvester &&) noexcept;

		/**
		 * @brief Scans every executable segment of the image.
		 *
		 * @param OutCalls Optional call graph to fill while scanning. The sweep
		 *        already decodes every instruction and already recognises a call, so
		 *        letting it record the edges saves a second identical pass over the
		 *        whole image - which is what CallGraph::Build was.
		 * @return false when the image's architecture has no decoder.
		 */
		bool Run(IMemory *Memory, const IArchDecoder *Arch, const HarvestOptions &Options = {}, CallGraph *OutCalls = nullptr);

		/// Every global the scan found, keyed by address.
		const AccessMap &GetAccesses() const { return Accesses_; }

		/// Total instructions decoded, for diagnostics.
		uint64_t GetDecodedCount() const { return Decoded_; }

		/// Sites that reference a given address, empty when unknown.
		const AccessInfo *Find(uint64_t Address) const;

		/**
		 * @brief Harvests one function in isolation.
		 *
		 * Same register propagation and indirect-slot resolution as the whole-image
		 * pass, but bounded to [Start, End) and starting from a clean register state,
		 * so the result is exactly "the globals this function touches". That set is
		 * small - a median of 2 for anchored functions - which is what makes picking
		 * the right one tractable.
		 *
		 * Requires a prior Run() to have selected a decoder; it does not disturb the
		 * whole-image map.
		 *
		 * @param Memory Backend to read code from.
		 * @param Start Function entry.
		 * @param End   One past the last instruction to decode.
		 * @return The globals this function alone touches.
		 */
		AccessMap ScanFunction(const IMemory *Memory, uint64_t Start, uint64_t End, const HarvestOptions &Options = {}) const;

	private:
		void ScanRange(const IMemory *Memory, uint64_t Start, uint64_t End, const HarvestOptions &Options, CallGraph *OutCalls);

		/**
		 * @brief Records one observation of a global.
		 *
		 * @param bRecordOffset False for observations that do not read a field, so
		 *        they cannot add a spurious displacement-0 entry. Whether a global is
		 *        ever named at +0 is load-bearing: it is what separates a struct base
		 *        from a field buried inside one, for both the name table and the
		 *        derived object-array member.
		 */
		static void RecordTo(AccessMap &Map, uint64_t Global, EAccessKind Kind, uint64_t Site, const HarvestOptions &Options, int64_t Offset = 0, uint8_t AccessWidth = 0, bool bRecordOffset = true);

		/// Shared walker behind both the whole-image and per-function passes.
		void Walk(const IMemory *Memory, uint64_t Start, uint64_t End, const HarvestOptions &Options, AccessMap &Out, uint64_t *DecodedOut, CallGraph *OutCalls = nullptr) const;

		AccessMap Accesses_;
		/// Fetched once in Run(): GetUnrealModule() returns by value, so it must
		/// never be called from the per-instruction path.
		ModuleInfo Module_;
		uint64_t Decoded_ = 0;
		/// Borrowed, not owned: the caller chooses the decoder and must keep it
		/// alive for the harvester's lifetime.
		const IArchDecoder *Arch_ = nullptr;
	};

} // namespace anduefker::analyzer
