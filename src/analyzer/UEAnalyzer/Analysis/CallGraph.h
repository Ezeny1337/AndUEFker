#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../../Memory/IMemory.h"

class IArchDecoder;

namespace anduefker::analyzer
{

	/**
	 * @brief Call sites, indexed by callee.
	 *
	 * Needed because the most precise anchors identify *member* functions. A
	 * function like FUObjectArray::AllocateUObjectIndex reaches the object array
	 * through its `this` parameter and never materialises the global's address, so
	 * the global only appears at the call site. Without the callers, the strongest
	 * anchor in the table resolves to nothing.
	 */
	class CallGraph
	{
	public:
		/// One pass over the executable segments recording every direct call/tail-call.
		/// Kept for standalone callers; UEAnalyzer instead has the harvester record
		/// edges during its own sweep, so the image is decoded once rather than twice.
		void Build(IMemory *Memory, const IArchDecoder &Arch);

		/// @brief Records one call edge. Callers must add sites ascending per callee.
		void AddEdge(uint64_t Callee, uint64_t Site)
		{
			Callers_[Callee].push_back(Site);
			++Edges_;
		}

		/// @brief Drops every recorded edge.
		void Clear()
		{
			Callers_.clear();
			Edges_ = 0;
		}

		/// Addresses of instructions that call Callee, empty when none.
		const std::vector<uint64_t> *CallersOf(uint64_t Callee) const;

		/// True when some instruction calls this exact address, i.e. it is a real
		/// function entry. More reliable than any prologue heuristic.
		bool IsEntry(uint64_t Address) const { return Callers_.count(Address) != 0; }

		/**
		 * @brief Nearest known entry at or below Address, within Window bytes.
		 *
		 * Prologue scanning can land a few instructions off the true entry, which is
		 * enough to lose every caller. Snapping to an address something actually
		 * calls fixes that.
		 */
		bool SnapToEntry(uint64_t Address, uint64_t Window, uint64_t &OutEntry) const;

		size_t EdgeCount() const { return Edges_; }

		/**
		 * @brief Value of an argument register at a call, if it is a known address.
		 *
		 * Replays the caller from its own entry to the call site with the same
		 * register propagation used everywhere else, then reads the argument register.
		 * This is how `this` is recovered: the caller does ADRP+ADD into X0 and calls.
		 *
		 * @param Memory       Backend to read code from.
		 * @param Module      Module being analysed, for bounds.
		 * @param Arch        Decoder for the module's architecture.
		 * @param CallerStart Entry of the function containing CallSite.
		 * @param CallSite    Address of the call instruction.
		 * @param ArgReg      Argument register index (0 = X0/R0, the `this` slot).
		 * @param OutValue    Receives the resolved address on success.
		 * @return false when the register does not hold a resolvable address.
		 */
		static bool ResolveArgumentAtCall(const IMemory *Memory, const ModuleInfo &Module, const IArchDecoder &Arch, uint64_t CallerStart, uint64_t CallSite, int ArgReg, uint64_t &OutValue);

	private:
		std::unordered_map<uint64_t, std::vector<uint64_t>> Callers_;
		size_t Edges_ = 0;
	};

} // namespace anduefker::analyzer
