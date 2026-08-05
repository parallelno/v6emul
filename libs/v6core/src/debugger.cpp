#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <vector>

#include "core/debugger.h"
#include "utils/str_utils.h"
#include "utils/utils.h"

dev::Debugger::Debugger(Hardware& _hardware, const int _recordFrames)
	:
	m_hardware(_hardware),
	m_lastReadsAddrs(), m_lastWritesAddrs(),
	m_memLastRW(),
	m_lastReadsAddrsOld(), m_lastWritesAddrsOld(),
	m_debugData(_hardware),
	m_disasm(_hardware, m_debugData),
	m_traceLog(m_debugData),
	m_lastRWAddrsOut(),
	m_recorder(_recordFrames)
{

	Hardware::DebugFunc debugFunc = std::bind
		(&Debugger::Debug, this,
		std::placeholders::_1,
		std::placeholders::_2,
		std::placeholders::_3,
		std::placeholders::_4);

	Hardware::DebugReqHandlingFunc debugReqHandlingFunc = std::bind(
		&Debugger::DebugReqHandling, this,
		std::placeholders::_1,
		std::placeholders::_2,
		std::placeholders::_3,
		std::placeholders::_4,
		std::placeholders::_5,
		std::placeholders::_6);

	m_hardware.AttachDebugFuncs(debugFunc, debugReqHandlingFunc);
}

// UI thread
dev::Debugger::~Debugger()
{
	m_hardware.Request(Hardware::Req::DEBUG_ATTACH, { {"data", false} });
}

// Hardware thread.
// Has to be called after Hardware Reset, loading the rom file, fdd immage,
// attach/dettach debugger, and other operations that change the Hardware
// states because this func stores the last state of Hardware
void dev::Debugger::Reset(bool _resetRecorder,
	CpuI8080::State* _cpuStateP, Memory::State* _memStateP,
	IO::State* _ioStateP, Display::State* _displayStateP)
{
	m_disasm.Reset();
	m_debugData.Reset();

	m_lastWritesAddrs.fill(uint32_t(LAST_RW_NO_DATA));
	m_lastReadsAddrs.fill(uint32_t(LAST_RW_NO_DATA));
	m_lastWritesIdx = 0;
	m_lastReadsIdx = 0;
	m_memLastRW.fill(0);

	m_traceLog.Reset();
	if (_resetRecorder) m_recorder.Reset(
		_cpuStateP, _memStateP, _ioStateP, _displayStateP);
}

//////////////////////////////////////////////////////////////
//
// Debug call from the Hardware thread
//
//////////////////////////////////////////////////////////////

// Hardware thread
bool dev::Debugger::Debug(CpuI8080::State* _cpuStateP, Memory::State* _memStateP,
	IO::State* _ioStateP, Display::State* _displayStateP)
{
	// instruction check
	m_debugData.MemRunsUpdate(_memStateP->debug.instrGlobalAddr);

	// reads check
	{
		std::lock_guard<std::mutex> mlock(m_lastRWMutex);

		for (int i = 0; i < _memStateP->debug.readLen; i++)
		{
			GlobalAddr globalAddr = _memStateP->debug.readGlobalAddr[i];
			uint8_t val = _memStateP->debug.read[i];

			m_debugData.MemReadsUpdate(globalAddr);

			m_debugData.GetWatchpoints().Check(Watchpoint::Access::R, globalAddr, val);

			m_lastReadsAddrs[m_lastReadsIdx++] = globalAddr;
			m_lastReadsIdx %= LAST_RW_MAX;
		}
	}

	// writes check
	{
		std::lock_guard<std::mutex> mlock(m_lastRWMutex);

		for (int i = 0; i < _memStateP->debug.writeLen; i++)
		{
			GlobalAddr globalAddr = _memStateP->debug.writeGlobalAddr[i];
			uint8_t val = _memStateP->debug.write[i];

			// check if the memory is read-only
			auto memEdit = m_debugData.GetMemoryEdit(globalAddr);
			if (memEdit && memEdit->active && memEdit->readonly) {
				_memStateP->debug.write[i] = _memStateP->debug.beforeWrite[i];
				_memStateP->ramP->at(globalAddr) = _memStateP->debug.beforeWrite[i];
				continue;
			};

			m_debugData.MemWritesUpdate(globalAddr);

			m_debugData.GetWatchpoints().Check(
				Watchpoint::Access::W, globalAddr, val, _memStateP->debug.beforeWrite[i]);

			m_lastWritesAddrs[m_lastWritesIdx++] = globalAddr;
			m_lastWritesIdx %= LAST_RW_MAX;
		}
	}

	// code perf
	// TODO: check if the m_debugData window is open
	m_debugData.CheckCodePerfs(_cpuStateP->regs.pc.word, _cpuStateP->cc);

	auto break_ = false;

	// check scripts
	// TODO: check if the m_debugData window is open
	break_ |= m_debugData.GetScripts().Check(
		_cpuStateP, _memStateP, _ioStateP, _displayStateP);

	// check watchpoint status
	const auto watchpointHit = m_debugData.GetWatchpoints().GetHit();
	const bool watchpointBreak = m_debugData.GetWatchpoints().CheckBreak();
	break_ |= watchpointBreak;

	// check breakpoints
	const bool breakpointBreak = m_debugData.GetBreakpoints().Check(*_cpuStateP, *_memStateP);
	break_ |= breakpointBreak;

	if (watchpointBreak && watchpointHit) {
		const auto access = watchpointHit->access == Watchpoint::Access::R ? "read" : "write";
		nlohmann::json trigger = {
			{"watchpointIds", watchpointHit->ids},
			{"access", access},
			{"accessedGlobalAddress", watchpointHit->globalAddr},
			{"globalInstructionAddress", _memStateP->debug.instrGlobalAddr},
			{"description", std::format("Watchpoint matched a {}", access)}
		};
		if (watchpointHit->access == Watchpoint::Access::R) {
			trigger["observedValue"] = watchpointHit->value;
		} else {
			trigger["newValue"] = watchpointHit->value;
			if (watchpointHit->oldValue) trigger["oldValue"] = *watchpointHit->oldValue;
		}
		m_hardware.RecordStop("watchpoint", trigger);
	} else if (breakpointBreak) {
		const auto address = _cpuStateP->regs.pc.word;
		m_hardware.RecordStop("breakpoint", {
			{"breakpointIds", {address}},
			{"breakpointAddress", address},
			{"description", std::format("Breakpoint at 0x{:04X}", address)}
		});
	} else if (break_) {
		m_hardware.RecordStop("unknown", {{"description", "Debugger requested a stop"}});
	}

	// tracelog
	m_traceLog.Update(*_cpuStateP, *_memStateP, *_displayStateP);

	// recorder
	m_recorder.Update(_cpuStateP, _memStateP, _ioStateP, _displayStateP);

	return break_;
}

// Hardware thread
auto dev::Debugger::DebugReqHandling(Hardware::Req _req, nlohmann::json _reqDataJ,
	CpuI8080::State* _cpuStateP, Memory::State* _memStateP,
	IO::State* _ioStateP, Display::State* _displayStateP)
-> nlohmann::json
{
	nlohmann::json out;

	switch (_req)
	{
	case Hardware::Req::DEBUG_RESET:
		Reset(_reqDataJ["resetRecorder"], _cpuStateP, _memStateP, _ioStateP, _displayStateP);
		break;
	//////////////////
	//
	// Recorder
	//
	/////////////////

	case Hardware::Req::DEBUG_RECORDER_RESET:
		m_recorder.Reset(_cpuStateP, _memStateP, _ioStateP, _displayStateP);
		break;

	case Hardware::Req::DEBUG_RECORDER_PLAY_FORWARD:
		m_recorder.PlayForward(_reqDataJ["frames"], _cpuStateP, _memStateP,
								_ioStateP, _displayStateP);
		break;

	case Hardware::Req::DEBUG_RECORDER_PLAY_REVERSE:
		m_recorder.PlayReverse(_reqDataJ["frames"], _cpuStateP, _memStateP,
								_ioStateP, _displayStateP);
		break;

	case Hardware::Req::DEBUG_RECORDER_GET_STATE_RECORDED:
		out = nlohmann::json{ {"states", m_recorder.GetStateRecorded() } };
		break;

	case Hardware::Req::DEBUG_RECORDER_GET_STATE_CURRENT:
		out = nlohmann::json{ {"states", m_recorder.GetStateCurrent() } };
		break;

	case Hardware::Req::DEBUG_RECORDER_SERIALIZE: {

		out = nlohmann::json{ {"data", nlohmann::json::binary(m_recorder.Serialize()) } };
		break;
	}
	case Hardware::Req::DEBUG_RECORDER_DESERIALIZE: {

		nlohmann::json::binary_t binaryData =
			_reqDataJ["data"].get<nlohmann::json::binary_t>();

		std::vector<uint8_t> data(binaryData.begin(), binaryData.end());

		m_recorder.Deserialize(data, _cpuStateP, _memStateP, _ioStateP, _displayStateP);
		break;
	}
	//////////////////
	//
	// Breakpoints
	//
	/////////////////

	case Hardware::Req::DEBUG_BREAKPOINT_DEL_ALL:
		m_debugData.GetBreakpoints().Clear();
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_DEL:
		m_debugData.GetBreakpoints().Del(_reqDataJ["addr"]);
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_ADD: {
		Breakpoint::Data bpData{
			_reqDataJ["addr"],
			Breakpoint::MemPages{_reqDataJ["memPages"]},
			Breakpoint::GetStatus(_reqDataJ["status"]),
			_reqDataJ["autoDelete"],
			Breakpoint::GetOperand(_reqDataJ["operand"]),
			ParseConditionName(_reqDataJ["condition"]),
			_reqDataJ["value"],
			_reqDataJ.value("counter", uint64_t{1}) };
		m_debugData.GetBreakpoints().Add({ std::move(bpData), _reqDataJ["comment"] });
		break;
	}
	case Hardware::Req::DEBUG_BREAKPOINT_SET_STATUS:
		m_debugData.GetBreakpoints().SetStatus(
			 _reqDataJ["addr"], Breakpoint::GetStatus(_reqDataJ["status"]));
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_ACTIVE:
		m_debugData.GetBreakpoints().SetStatus(
			_reqDataJ["addr"], Breakpoint::Status::ACTIVE);
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_DISABLE:
		m_debugData.GetBreakpoints().SetStatus(
			_reqDataJ["addr"], Breakpoint::Status::DISABLED);
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_GET_STATUS:
		out = nlohmann::json{{
				"status",
				Breakpoint::GetStatusS(m_debugData.GetBreakpoints().GetStatus(_reqDataJ["addr"]))
			}};
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_GET_UPDATES:
		out = nlohmann::json{ {
			"updates",
			static_cast<uint64_t>(m_debugData.GetBreakpoints().GetUpdates())
		}};
		break;

	case Hardware::Req::DEBUG_BREAKPOINT_GET_ALL:
		{
			out = nlohmann::json::array();
			std::vector<const Breakpoint*> breakpoints;
			for (const auto& [addr, bp] : m_debugData.GetBreakpoints().GetAll()) breakpoints.push_back(&bp);
			std::sort(breakpoints.begin(), breakpoints.end(), [](const auto* lhs, const auto* rhs) {
				return lhs->data.structured.addr < rhs->data.structured.addr;
			});
			for (const auto* breakpoint : breakpoints) out.push_back(breakpoint->ToProtocolJson());
		}
		break;

	//////////////////
	//
	// Watchpoints
	//
	/////////////////

	case Hardware::Req::DEBUG_WATCHPOINT_DEL_ALL:
		m_debugData.GetWatchpoints().Clear();
		break;

	case Hardware::Req::DEBUG_WATCHPOINT_DEL:
		m_debugData.GetWatchpoints().Del(_reqDataJ["id"]);
		break;

	case Hardware::Req::DEBUG_WATCHPOINT_ADD: {
		Watchpoint::Data wpData{
			-1,
			Watchpoint::GetAccess(_reqDataJ["access"]),
			_reqDataJ["globalAddr"],
				ParseConditionName(_reqDataJ["condition"]),
			_reqDataJ["value"],
			Watchpoint::GetType(_reqDataJ["type"]),
			_reqDataJ["len"],
			_reqDataJ["active"]
		};
		m_debugData.GetWatchpoints().AddNew({std::move(wpData), _reqDataJ["comment"]});
		break;
	}
	case Hardware::Req::DEBUG_WATCHPOINT_EDIT: {
		Watchpoint::Data wpData{
			_reqDataJ["id"],
			Watchpoint::GetAccess(_reqDataJ["access"]),
			_reqDataJ["globalAddr"],
			ParseConditionName(_reqDataJ["condition"]),
			_reqDataJ["value"],
			Watchpoint::GetType(_reqDataJ["type"]),
			_reqDataJ["len"],
			_reqDataJ["active"]
		};
		const auto id = wpData.id;
		if (!m_debugData.GetWatchpoints().Edit({std::move(wpData), _reqDataJ["comment"]})) {
			throw WatchpointNotFound{id};
		}
		break;
	}
	case Hardware::Req::DEBUG_WATCHPOINT_GET_UPDATES:
		out = nlohmann::json{ {"updates", static_cast<uint64_t>(m_debugData.GetWatchpoints().GetUpdates()) } };
		break;

	case Hardware::Req::DEBUG_WATCHPOINT_GET_ALL:
		out = nlohmann::json::array();
		{
			std::vector<const Watchpoint*> watchpoints;
			for (const auto& [id, wp] : m_debugData.GetWatchpoints().GetAll()) watchpoints.push_back(&wp);
			std::sort(watchpoints.begin(), watchpoints.end(), [](const auto* lhs, const auto* rhs) {
				return lhs->data.id < rhs->data.id;
			});
			for (const auto* wp : watchpoints) {
				out.push_back(wp->ToJson());
			}
		}
		break;

	//////////////////
	//
	// Memory Edits
	//
	/////////////////

	case Hardware::Req::DEBUG_MEMORY_EDIT_DEL_ALL:
		m_debugData.DelAllMemoryEdits();
		break;

	case Hardware::Req::DEBUG_MEMORY_EDIT_DEL:
		m_debugData.DelMemoryEdit(_reqDataJ["globalAddr"]);
		break;

	case Hardware::Req::DEBUG_MEMORY_EDIT_ADD:
	{
		const auto globalAddr = _reqDataJ["globalAddr"].get<GlobalAddr>();
		const auto existing = m_debugData.GetMemoryEdit(globalAddr);
		const auto originalValue = existing
			? existing->originalValue
			: _memStateP->ramP->at(globalAddr);
		const auto enteredValue = _reqDataJ["enteredValue"].get<uint8_t>();
		const auto active = _reqDataJ["active"].get<bool>();
		const bool applyValue = active &&
			(!existing || !existing->active || existing->enteredValue != enteredValue);
		MemoryEdit edit{
			globalAddr,
			enteredValue,
			originalValue,
			_reqDataJ["comment"].get<std::string>(),
			_reqDataJ["readonly"].get<bool>(),
			active
		};
		m_debugData.SetMemoryEdit(edit);
		if (applyValue) _memStateP->ramP->at(globalAddr) = edit.enteredValue;
		break;
	}

	case Hardware::Req::DEBUG_MEMORY_EDIT_GET:
	{
		const auto globalAddr = _reqDataJ["globalAddr"].get<GlobalAddr>();
		auto memEdit = m_debugData.GetMemoryEdit(globalAddr);
		if (memEdit)
		{
			out = memEdit->ToSnapshotJson(_memStateP->ramP->at(globalAddr));
		}
		else out = nullptr;
		break;
	}

	case Hardware::Req::DEBUG_MEMORY_EDIT_EXISTS:
		out = { {"exists", m_debugData.GetMemoryEdit(_reqDataJ["globalAddr"]) != nullptr } };
		break;

	case Hardware::Req::DEBUG_MEMORY_EDIT_GET_ALL:
	{
		std::vector<const MemoryEdit*> edits;
		for (const auto& [globalAddr, edit] : m_debugData.GetMemoryEdits()) edits.push_back(&edit);
		std::sort(edits.begin(), edits.end(), [](const auto* lhs, const auto* rhs) {
			return lhs->globalAddr < rhs->globalAddr;
		});
		out["edits"] = nlohmann::json::array();
		for (const auto* edit : edits) {
			out["edits"].push_back(edit->ToSnapshotJson(_memStateP->ramP->at(edit->globalAddr)));
		}
		break;
	}

	case Hardware::Req::DEBUG_MEMORY_EDIT_RESTORE:
	{
		const auto globalAddr = _reqDataJ["globalAddr"].get<GlobalAddr>();
		const auto edit = m_debugData.GetMemoryEdit(globalAddr);
		if (!edit) throw MemoryEditNotFound(globalAddr);
		const auto restoredValue = edit->originalValue;
		_memStateP->ramP->at(globalAddr) = restoredValue;
		m_debugData.DelMemoryEdit(globalAddr);
		out = { {"globalAddr", globalAddr}, {"restoredValue", restoredValue}, {"deleted", true} };
		break;
	}

	case Hardware::Req::INTERNAL_REAPPLY_MEMORY_EDITS:
		for (const auto& [globalAddr, edit] : m_debugData.GetMemoryEdits()) {
			if (edit.active) _memStateP->ramP->at(globalAddr) = edit.enteredValue;
		}
		break;

	//////////////////
	//
	// Code Perfs
	//
	/////////////////
	case Hardware::Req::INTERNAL_CANCEL_CODE_PERF_SAMPLES:
		m_debugData.CancelCodePerfSamples();
		break;

	case Hardware::Req::DEBUG_CODE_PERF_DEL_ALL:
		m_debugData.DelAllCodePerfs();
		break;

	case Hardware::Req::DEBUG_CODE_PERF_DEL:
		m_debugData.DelCodePerf(_reqDataJ["id"]);
		break;

	case Hardware::Req::DEBUG_CODE_PERF_ADD:
	{
		const auto id = m_debugData.AddCodePerf(CodePerf{_reqDataJ});
		out = m_debugData.GetCodePerf(id)->ToSnapshotJson(id);
		break;
	}

	case Hardware::Req::DEBUG_CODE_PERF_EDIT:
	{
		const auto id = _reqDataJ["id"].get<Id>();
		out = m_debugData.EditCodePerf(id, CodePerf{_reqDataJ}).ToSnapshotJson(id);
		break;
	}

	case Hardware::Req::DEBUG_CODE_PERF_GET:
	{
		const auto id = _reqDataJ["id"].get<Id>();
		auto codePerf = m_debugData.GetCodePerf(id);
		out = codePerf ? codePerf->ToSnapshotJson(id) : nlohmann::json(nullptr);
		break;
	}

	case Hardware::Req::DEBUG_CODE_PERF_GET_ALL:
	{
		std::vector<Id> ids;
		ids.reserve(m_debugData.GetCodePerfs().size());
		for (const auto& [id, codePerf] : m_debugData.GetCodePerfs()) ids.push_back(id);
		std::sort(ids.begin(), ids.end());

		out = nlohmann::json::array();
		for (const auto id : ids)
			out.push_back(m_debugData.GetCodePerfs().at(id).ToSnapshotJson(id));
		break;
	}

	case Hardware::Req::DEBUG_CODE_PERF_EXISTS:
		out = { {"exists", m_debugData.GetCodePerf(_reqDataJ["id"]) != nullptr } };
		break;

	//////////////////
	//
	// Scripts
	//
	/////////////////

	case Hardware::Req::DEBUG_SCRIPT_DEL_ALL:
		m_debugData.GetScripts().Clear();
		break;

	case Hardware::Req::DEBUG_SCRIPT_DEL:
		m_debugData.GetScripts().Del(_reqDataJ["id"]);
		break;

	case Hardware::Req::DEBUG_SCRIPT_ADD: {
		m_debugData.GetScripts().Add(_reqDataJ);
		break;
	}
	case Hardware::Req::DEBUG_SCRIPT_GET_UPDATES:
		out = nlohmann::json{ {"updates", static_cast<uint64_t>(m_debugData.GetScripts().GetUpdates()) } };
		break;

	case Hardware::Req::DEBUG_SCRIPT_GET_ALL:
		for (const auto& [id, script] : m_debugData.GetScripts().GetAll())
		{
			out.push_back(script.ToJson());
		}
		break;

	//////////////////
	//
	// Trace Log
	//
	/////////////////

	case Hardware::Req::DEBUG_TRACE_LOG_ENABLE:
		m_traceLog.SetSaveLog(true, _reqDataJ["path"]);
		break;

	case Hardware::Req::DEBUG_TRACE_LOG_DISABLE:
		m_traceLog.SetSaveLog(false);
		break;

	default:
		break;
	}

	return out;
}

// UI thread
void dev::Debugger::UpdateLastRW()
{
	// remove old stats
	for (int i = 0; i < m_lastReadsAddrsOld.size(); i++)
	{
		auto globalAddrLastRead = m_lastReadsAddrsOld[i];
		if (globalAddrLastRead != LAST_RW_NO_DATA) {
			m_memLastRW[globalAddrLastRead] = 0;
		}
		auto globalAddrLastWrite = m_lastWritesAddrsOld[i];
		if (globalAddrLastWrite != LAST_RW_NO_DATA) {
			m_memLastRW[globalAddrLastWrite] = 0;
		}
	}

	// copy new reads stats
	std::lock_guard<std::mutex> mlock(m_lastRWMutex);
	uint16_t readsIdx = m_lastReadsIdx;
	for (auto globalAddr : m_lastReadsAddrs){
		if (globalAddr != LAST_RW_NO_DATA)
		{
			auto val = m_memLastRW[globalAddr] & 0xFFFF0000; // remove reads, keep writes
			m_memLastRW[globalAddr] =
				val |
				static_cast<uint16_t>(LAST_RW_MAX - readsIdx) % LAST_RW_MAX;
		}
		readsIdx--;
	}

	// copy new writes stats
	uint16_t writesIdx = m_lastWritesIdx;
	for (auto globalAddr : m_lastWritesAddrs){
		if (globalAddr != LAST_RW_NO_DATA)
		{
			auto val = m_memLastRW[globalAddr] & 0x0000FFFF; // remove writes, keep reads
			m_memLastRW[globalAddr] =
				val |
				(static_cast<uint16_t>(LAST_RW_MAX - writesIdx) % LAST_RW_MAX)<<16;
		}
		writesIdx--;
	}

	m_lastReadsAddrsOld = m_lastReadsAddrs;
	m_lastWritesAddrsOld = m_lastWritesAddrs;
}