#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../Memory/IMemory.h"

#include "Analysis/GlobalAccessHarvester.h"
#include "Analysis/TCharDetect.h"
#include "Core/Candidate.h"
#include "Core/Scoring.h"
#include "Strategies/IStrategy.h"

class IArchDecoder;

namespace anduefker::analyzer
{

	/**
	 * @brief Caller-supplied final veto on a candidate.
	 *
	 * Runs after every built-in check, so it can only ever reject - it cannot
	 * promote an address the library rejected. Returning false drops the candidate.
	 *
	 * The library deliberately ships no built-in structure walker to fill this role:
	 * what counts as a valid FUObjectArray or FNamePool differs across engine forks,
	 * so the check belongs to the caller, who knows the target.
	 */
	using CandidateValidator = std::function<bool(uintptr_t)>;

	/**
	 * @brief How many threads analysis may use.
	 *
	 * Single is the default and is the reference result: every other mode is
	 * verified by diffing its output against it. Two is capped deliberately - the
	 * only phase that parallelises safely is literal scanning, so a third thread
	 * would have nothing to do.
	 */
	enum class EThreadMode
	{
		Single, ///< Everything on the calling thread.
		Two,	///< Instruction harvest and literal scanning run side by side.
	};

	/// Everything tunable about an analysis run.
	struct AnalyzerOptions
	{
		ScoringWeights Weights;	   ///< Scoring model.
		size_t MaxCandidates = 10; ///< Cap on returned candidates, per target.

		/// Restricts resolution to one mechanism; empty tries all of them.
		///
		/// A diagnostic knob: it answers "does the anchored tier alone find this?".
		/// Leave it empty for normal use, which runs the verified tiers first and
		/// falls back to leads.
		std::optional<EFindMethod> Method;

		bool bVerbose = false; ///< Extra tracing on stderr.

		/// Single by default: a phone should not pay for a thread it did not ask
		/// for, and Single is the result every other mode is checked against.
		EThreadMode ThreadMode = EThreadMode::Single;

		/// Which targets to analyse, by strategy name. Empty means all of them.
		///
		/// Each target costs its own pass over the module's literals to find its
		/// anchors, so a caller after one global need not pay for the others. The
		/// shared instruction harvest and call graph happen once either way.
		///
		/// A target left out answers empty from Find(), as an unknown name does.
		std::vector<std::string> Targets;

		/// Whether TargetName was requested. True for everything when Targets is empty.
		bool IsTargetWanted(const char *TargetName) const
		{
			if (Targets.empty())
				return true;
			return std::find(Targets.begin(), Targets.end(), TargetName) != Targets.end();
		}
	};

	/// Names of the built-in targets, for use with UEAnalyzer::Find.
	///
	/// A registered strategy's Name() is the only identity a target has; these
	/// spare callers from spelling the built-ins by hand. A strategy added later
	/// needs no entry here - Find takes any registered name.
	namespace Targets
	{
		inline constexpr const char *ObjObjects = "ObjObjects";
		inline constexpr const char *Names = "Names";
		inline constexpr const char *GUObjectArray = "GUObjectArray";

		/**
		 * @brief Every strategy this library ships.
		 *
		 * The one place a new target is registered. Adding a strategy means writing
		 * its .h/.cpp and adding one line here - nothing in UEAnalyzer or any caller
		 * needs to change, since Analyze() builds its target list from this and
		 * every diagnostic iterates whatever is registered.
		 */
		std::vector<StrategyPtr> CreateAll();
	} // namespace Targets

	/// One field of a located structure, at an offset read off the binary.
	struct LayoutField
	{
		int64_t Offset = -1; ///< Bytes from the address described. -1 when absent.
		uint32_t Reads = 0;	 ///< How often code touches it.
		uint8_t Width = 0;	 ///< Access width in bytes, as code uses it.
	};

	/// The structure at an address, as this binary actually uses it.
	struct LayoutDescription
	{
		/// What the shape reads as, and how strongly. Unknown when undetermined.
		LayoutClassification Layout;

		LayoutField Table;				 ///< Hottest pointer-width field: the element table.
		std::vector<LayoutField> Counts; ///< 32-bit fields beside it: the element counts.
		bool bValid = false;			 ///< False when the address has no readable shape.
	};

	/// How code reaches an address.
	struct AccessSummary
	{
		uint32_t Total = 0;
		uint32_t PointerWide = 0;
		uint32_t Narrow = 0;
		uint32_t Rank = 0; ///< 1 = most referenced; 0 = not ranked.
		uint32_t RankedTotal = 0;
		bool bAddressTaken = false;
		bool bIndirectBase = false;
		bool bLoaded = false;
		bool bStored = false;
		std::vector<int64_t> Offsets; ///< Displacements the address is reached at.
	};

	/// One target's reading of an address.
	struct TargetAssessment
	{
		const char *Target = ""; ///< Strategy name this assessment belongs to.
		StructureEvidence Evidence;
		LayoutDescription Layout; ///< Meaningful only when Evidence.Passed.

		size_t Rank = 0; ///< 1-based position among candidates; 0 = absent.
		size_t CandidateCount = 0;
		bool bAnswer = false; ///< True when this target returned Address.

		/// One line covering whichever question applies: why the address is not a
		/// candidate, or where it ranks now that it is. Always set, so a caller
		/// never has to branch on Evidence/Rank just to describe the verdict.
		std::string Reason;
	};

	/// Everything known about one address, across every registered target.
	struct AddressExplanation
	{
		uint64_t Address = 0;
		bool bInModule = false;
		std::string Segment; ///< Region name; empty when outside the module.
		bool bWritable = false;
		bool bExecutable = false;

		bool bReferenced = false; ///< False when no instruction computes the address.
		AccessSummary Access;

		/// One entry per registered target, in registration order.
		std::vector<TargetAssessment> Targets;
	};

	/// Renders an explanation as a human-readable report.
	///
	/// Kept out of the analyzer so presentation can change without touching
	/// detection, and so callers can render their own.
	std::string FormatExplanation(const AddressExplanation &Explanation);

	/**
	 * @brief Locates UE globals.
	 *
	 * The analysis pass is heavy - millions of decoded instructions - so it runs
	 * once in Analyze() and is shared by both lookups.
	 */
	class UEAnalyzer
	{
	public:
		UEAnalyzer() = default;
		~UEAnalyzer() = default;
		UEAnalyzer(UEAnalyzer &&) noexcept = default;
		UEAnalyzer &operator=(UEAnalyzer &&) noexcept = default;

		/**
		 * @brief Analyses a memory backend and returns the analyzer for it.
		 *
		 * The only way to obtain one: an analyzer that has not analysed anything is
		 * not a useful object, so it cannot be constructed. On failure this still
		 * returns an instance - IsValid() is false and GetError() says why - rather
		 * than making every caller handle a second failure channel.
		 *
		 * The heavy pass (millions of decoded instructions) happens here once and is
		 * shared by every query afterwards.
		 *
		 * @param Memory  Backend to analyse. Borrowed, and must outlive the analyzer.
		 *                Null is reported through GetError(), never dereferenced.
		 * @param Arch    Decoder for the image's architecture, borrowed the same way.
		 *                Stated by the caller rather than derived: a live process may
		 *                have had its ELF header stripped, so there is nothing to read
		 *                it off. For a mapped file, FileImageMemory::GetArch() has it.
		 * @param Options Scoring and method options; the defaults are the fitted model.
		 * @param Harvest Instruction-scan tuning; the defaults are the fitted values.
		 */
		static UEAnalyzer Analyze(IMemory *Memory, const IArchDecoder *Arch, AnalyzerOptions Options = {}, HarvestOptions Harvest = {});

		/// True when analysis succeeded and the query methods will answer.
		bool IsValid() const;

		/**
		 * @brief Locates the target a strategy answers to, by its own name.
		 *
		 * A caller that walks several targets - a batch harness, a diagnostic
		 * tool - should loop TargetNames() through this rather than a switch, so
		 * adding a target needs no edit at the call site.
		 *
		 * @param TargetName Exactly the strategy's Name(); an unknown one answers empty.
		 * @param Validator Optional final veto; empty means no veto.
		 */
		LocateResult Find(const char *TargetName, const CandidateValidator &Validator = {}) const;

		/// Names of every registered target, in registration order.
		///
		/// Lets a caller loop over "everything this analyzer can find" - a batch
		/// harness, a diagnostic tool - instead of hardcoding the built-in Targets
		/// constants, so a strategy added later needs no edit at the call site.
		std::vector<std::string> TargetNames() const;

		/**
		 * @brief One target's full reading of an address: verification, layout,
		 *        ranking, and a one-line reason covering whichever applies.
		 *
		 * Call this for any address regardless of whether the target actually
		 * returned it; TargetAssessment::Reason and ::bAnswer say which case
		 * applies.
		 *
		 * @param TargetName Exactly the strategy's Name(); an unknown one answers
		 *        an assessment whose Reason explains that.
		 */
		TargetAssessment AssessAddress(const char *TargetName, uint64_t Address) const;

		/// Everything known about one address, for every registered target.
		///
		/// Returns data rather than text so callers can inspect or render it; pass
		/// the result to FormatExplanation for the human-readable report.
		AddressExplanation Explain(uint64_t Address) const;

		/**
		 * @brief Whether this build's TCHAR is char16_t or a 4-byte wchar_t.
		 *
		 * Measured from the engine's own string literals, never inferred from a
		 * version. Unknown when the evidence is not decisive. The measurement itself
		 * happens in Analyze(); this only reports what it found.
		 */
		ETCharKind GetTCharKind() const;

		/// Stride for walking an FString buffer: 2, 4, or 0 when undetermined.
		size_t GetTCharSize() const;

		/// 一次分析的只读摘要。不参与打分，也不改变 Find() 的结果。
		struct TargetDigest
		{
			uint64_t DecodedInstructions = 0; ///< 可执行段里解码过的指令数。0 表示没有扫到代码。
			size_t GlobalAccesses = 0;		  ///< harvest 记录到的全局地址数。
			uint32_t RankedGlobals = 0;		  ///< 参与排序的可写、非执行全局数。
			size_t AnchorAddresses = 0;		  ///< 这个目标在镜像里找到的锚定字符串地址数。
			size_t AnchorCodeSites = 0;		  ///< 代码里引用这些字符串的位置数。
			size_t NarrowHits = 0;
			size_t Utf16Hits = 0;
			size_t Utf32Hits = 0;
			std::vector<std::string> FoundAnchors; ///< 实际命中的锚定字符串原文。
			bool bFound = false;				   ///< TargetName 有对应策略时为 true。
		};

		/// @param TargetName 策略自己的 Name()。未知名字仍返回 harvest 规模，但 bFound 为 false。
		TargetDigest DescribeTarget(const char *TargetName) const;

		/// 一条锚定字符串为什么没有变成已验证结果。Skip 为空表示至少有一次解析被采纳。
		struct ResolutionTrace
		{
			std::string Anchor;
			size_t Sites = 0;
			size_t Applied = 0;
			size_t Rejected = 0;
			size_t NoPick = 0;
			std::string Skip;
			std::vector<std::pair<uint64_t, std::string>> RejectedSamples;
		};

		/// 重跑该目标的锚定解析，只为取出每条锚定的站点、放弃原因和被结构检查拒绝的地址。
		std::vector<ResolutionTrace> ExplainResolution(const char *TargetName) const;

		/// The options this analyzer is currently answering with.
		const AnalyzerOptions &GetOptions() const { return Options_; }

		/**
		 * @brief Replaces the options.
		 *
		 * Safe at any point: the options are read on every Find* call rather than
		 * baked into the analysis, so a changed scoring model or method takes effect
		 * on the next query with no re-analysis. The heavy pass depends only on the
		 * memory backend.
		 */
		void SetOptions(AnalyzerOptions Options) { Options_ = std::move(Options); }

		/// Why analysis failed; empty when IsValid().
		const std::string &GetError() const { return Error_; }

	public:
		/// Shared analysis state. Opaque by design: the heavy analysis headers stay
		/// out of this one, and only the implementation names its members.
		struct Analysis;

		/**
		 * @brief Shared body behind both Find methods.
		 *
		 * The target is named, not numbered: the name is the strategy's own Name(),
		 * the same string recorded on every candidate and printed in every report, so
		 * there is one spelling of a target and no ordering to keep in step. A name
		 * no strategy answers to returns an empty result.
		 */
		LocateResult Run(const char *TargetName, const CandidateValidator &Validator) const;

		/// Turns structure evidence into a layout description, reading field widths
		/// and counts back out of the access map.
		LayoutDescription MakeLayoutDescription(const StructureEvidence &Evidence, uint64_t Address) const;

		AnalyzerOptions Options_;
		std::string Error_;
		IMemory *Memory_ = nullptr;
		std::shared_ptr<Analysis> State_;
	};

} // namespace anduefker::analyzer
