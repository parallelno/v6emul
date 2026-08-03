#pragma once

#include <unordered_map>
#include <vector>
#include <limits.h>
#include <format>
#include <stdexcept>

#include "utils/types.h"
#include "utils/str_utils.h"
#include "core/hardware.h"

#include "core/breakpoints.h"
#include "core/watchpoints.h"
#include "core/code_perf.h"
#include "core/memory_edit.h"
#include "core/scripts.h"
#include "utils/json_utils.h"

namespace dev
{
	class CodePerfNotFound : public std::runtime_error
	{
	public:
		explicit CodePerfNotFound(const Id _id)
			: std::runtime_error("code performance record id not found"), m_id(_id) {}

		auto GetId() const -> Id { return m_id; }

	private:
		Id m_id;
	};

	enum class CodePerfAddFailure { CAPACITY, ID_EXHAUSTED };

	class CodePerfAddError : public std::runtime_error
	{
	public:
		explicit CodePerfAddError(const CodePerfAddFailure _failure)
			: std::runtime_error("code performance record cannot be added"), m_failure(_failure) {}

		auto GetFailure() const -> CodePerfAddFailure { return m_failure; }

	private:
		CodePerfAddFailure m_failure;
	};

	class DebugData
	{
	public:
		using LabelList = std::vector<std::string>;
		using Labels = std::unordered_map<GlobalAddr, LabelList>;
		using Comments = std::unordered_map<GlobalAddr, std::string>;

		using UpdateId = int;

		using FilteredElements = std::vector<std::tuple<std::string, GlobalAddr, std::string>>; // name, addr, addrS

		// injects the value into the memory while loading

		using MemoryEdits = std::unordered_map<GlobalAddr, MemoryEdit>;
		using CodePerfs = std::unordered_map<Id, CodePerf>;

		DebugData(Hardware& _hardware);

		auto GetLabels(const Addr _addr) const -> std::optional<LabelList>;
		auto HasLabels(const Addr _addr) const -> bool;
		auto GetLabelAddr(const std::string& _label) -> int;
		void SetLabels(const Addr _addr, const LabelList& _labels);
		void AddLabel(const Addr _addr, const std::string& _label);
		void DelLabel(const Addr _addr, const std::string& _label);
		void DelLabels(const Addr _addr);
		void DelAllLabels();
		void RenameLabel(const Addr _addr, const std::string& _oldLabel, const std::string& _newLabel);
		void GetFilteredLabels(FilteredElements& _out, const std::string& _filter = "") const;

		auto GetConsts(const Addr _addr) const -> const LabelList*;
		void SetConsts(const Addr _addr, const LabelList& _consts);
		void AddConst(const Addr _addr, const std::string& _const);
		void DelConst(const Addr _addr, const std::string& _const);
		void DelConsts(const Addr _addr);
		void DelAllConsts();
		void RenameConst(const Addr _addr, const std::string& _oldConst, const std::string& _newConst);
		void GetFilteredConsts(FilteredElements& _out, const std::string& _filter = "") const;

		auto GetComment(const Addr _addr) const -> const std::string*;
		void SetComment(const Addr _addr, const std::string& _comment);
		void DelComment(const Addr _addr);
		void DelAllComments();
		void GetFilteredComments(FilteredElements& _out, const std::string& _filter = "") const;

		auto GetMemoryEdit(const GlobalAddr _globalAddr) const -> const MemoryEdit*;
		auto GetMemoryEdits() const -> const MemoryEdits& { return m_memoryEdits; }
		void SetMemoryEdit(const MemoryEdit& _edit);
		void DelMemoryEdit(const GlobalAddr _globalAddr);
		void DelAllMemoryEdits();
		void GetFilteredMemoryEdits(FilteredElements& _out, const std::string& _filter = "") const;

		auto GetCodePerf(const Id _id) const -> const CodePerf*;
		auto GetCodePerfs() const -> const CodePerfs& { return m_codePerfs; }
		auto AddCodePerf(const CodePerf& _codePerf) -> Id;
		auto EditCodePerf(const Id _id, const CodePerf& _codePerf) -> const CodePerf&;
		void DelCodePerf(const Id _id);
		void DelAllCodePerfs();
		void CancelCodePerfSamples();
		auto CheckCodePerfs(const Addr _addrStart, const uint64_t _cc) -> bool;

		auto GetCommentsUpdates() const -> UpdateId { return m_commentsUpdates; };
		auto GetLabelsUpdates() const -> UpdateId { return m_labelsUpdates; };
		auto GetConstsUpdates() const -> UpdateId { return m_constsUpdates; };
		auto GetEditsUpdates() const -> UpdateId { return m_editsUpdates; };
		auto GetCodePerfsUpdates() const -> UpdateId { return m_codePerfsUpdates; };

		auto GetBreakpoints() -> Breakpoints& { return m_breakpoints; };
		auto GetWatchpoints() -> Watchpoints& { return m_watchpoints; };
		auto GetScripts() -> Scripts& { return m_scripts; };

		auto GetPath() const -> const std::string& { return m_debugPath; };

		inline void MemRunsUpdate(const GlobalAddr _globalAddr) { m_memRuns[_globalAddr]++; };
		inline void MemReadsUpdate(const GlobalAddr _globalAddr) { m_memReads[_globalAddr]++; };
		inline void MemWritesUpdate(const GlobalAddr _globalAddr) { m_memWrites[_globalAddr]++; };

		inline auto GetMemRuns(const GlobalAddr _globalAddr) const
			-> uint64_t {
				return m_memRuns[_globalAddr];
			};
		inline auto GetMemReads(const GlobalAddr _globalAddr) const
			-> uint64_t {
				return m_memReads[_globalAddr];
			};
		inline auto GetMemWrites(const GlobalAddr _globalAddr) const
			-> uint64_t {
				return m_memWrites[_globalAddr];
			};

		void Reset();

	private:

		Hardware& m_hardware;

		Labels m_labels;	// labels
		Labels m_consts;	// labels used as constants or they point to data
		Comments m_comments;
		MemoryEdits m_memoryEdits; // code/data modifications
		CodePerfs m_codePerfs;
		Breakpoints m_breakpoints;
		Watchpoints m_watchpoints;
		Scripts m_scripts;

		std::string m_debugPath;

		UpdateId m_labelsUpdates = 0;
		UpdateId m_constsUpdates = 0;
		UpdateId m_commentsUpdates = 0;
		UpdateId m_editsUpdates = 0;
		UpdateId m_codePerfsUpdates = 0;
		Id m_nextCodePerfId = 0;
		bool m_codePerfIdsExhausted = false;

		using MemStats = std::array<uint64_t, Memory::MEMORY_GLOBAL_LEN>;
		MemStats m_memRuns;
		MemStats m_memReads;
		MemStats m_memWrites;
	};
}