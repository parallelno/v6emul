#include "core/trace_log.h"

#include <chrono>
#include <cctype>
#include <format>

#include "utils/str_utils.h"

dev::TraceLog::TraceLog(const DebugData& _debugData)
	:
	m_debugData(_debugData)
{}

// Hardware thread
void dev::TraceLog::Update(
	const CpuI8080::State& _cpuState,
	const Memory::State& _memState,
	const Display::State& _displayState)
{
	InvalidateFilter();
	UpdateLogBuffer(_cpuState, _memState);

	SaveLog(_cpuState, _memState, _displayState);
}

void dev::TraceLog::UpdateLogBuffer(
	const CpuI8080::State& _cpuState, const Memory::State& _memState)
{
	uint8_t opcode = _memState.debug.instr.opcode;

	// skip repeataive HLT
	if (opcode == CpuI8080::OPCODE_HLT &&
		m_log[m_logIdx].instr.opcode == CpuI8080::OPCODE_HLT) {
		return;
	}

	m_logIdx = (m_logIdx + TRACE_LOG_SIZE - 1) % TRACE_LOG_SIZE;
	m_historyCount = std::min(m_historyCount + 1, TRACE_LOG_SIZE);
	++m_historyGeneration;

	m_log[m_logIdx].globalAddr = _memState.debug.instrGlobalAddr;
	m_log[m_logIdx].instr.opcode = _memState.debug.instr.opcode;
	m_log[m_logIdx].instr.dataW = opcode == CpuI8080::OPCODE_PCHL ?
		_cpuState.regs.pc.word : _memState.debug.instr.dataW;
}

auto dev::TraceLog::FindCaller(const nlohmann::json& request, uint16_t livePc) const -> nlohmann::json
{
	const auto start = request.at("rangeStart").get<uint32_t>();
	const auto end = request.at("rangeEnd").get<uint32_t>();
	if (start >= end || end > 0x10000) throw TraceLogQueryError("invalid history range");
	size_t cursor = 0;
	uint16_t destination = livePc;
	if (request.contains("callerCursor")) {
		if (request.at("historyGeneration").get<uint64_t>() != m_historyGeneration)
			throw TraceLogQueryError("stale history generation");
		cursor = request.at("callerCursor").get<size_t>();
		if (cursor >= m_historyCount) throw TraceLogQueryError("invalid history cursor");
		destination = static_cast<uint16_t>(m_log[(m_logIdx + cursor) % TRACE_LOG_SIZE].globalAddr);
		++cursor;
	}
	for (; cursor < m_historyCount; ++cursor) {
		const auto pc = static_cast<uint16_t>(m_log[(m_logIdx + cursor) % TRACE_LOG_SIZE].globalAddr);
		if (pc < start || pc >= end) return {{"status", "found"},
			{"historyGeneration", m_historyGeneration}, {"callerPc", pc},
			{"entrancePc", destination}, {"callerCursor", cursor}};
		destination = pc;
	}
	return {{"status", "historyEnd"}, {"historyGeneration", m_historyGeneration}};
}

auto dev::TraceLog::CallerSuccessor(const nlohmann::json& request) const -> nlohmann::json
{
	if (request.at("historyGeneration").get<uint64_t>() != m_historyGeneration)
		throw TraceLogQueryError("stale history generation");
	const auto cursor = request.at("callerCursor").get<size_t>();
	if (cursor >= m_historyCount) throw TraceLogQueryError("invalid history cursor");
	const auto& item = m_log[(m_logIdx + cursor) % TRACE_LOG_SIZE];
	return {{"address", static_cast<uint16_t>(item.globalAddr + CpuI8080::GetInstrLen(item.instr.opcode))}};
}

void dev::TraceLog::InvalidateFilter()
{
	m_activeFilterId = 0;
	m_filteredEntries.clear();
	++m_filterGeneration;
}

auto dev::TraceLog::GlobMatches(const std::string& pattern, const std::string& value) -> bool
{
	size_t patternIndex = 0;
	size_t valueIndex = 0;
	size_t starIndex = std::string::npos;
	size_t starValueIndex = 0;
	while (valueIndex < value.size()) {
		if (patternIndex < pattern.size() && pattern[patternIndex] != '*' &&
			std::tolower(static_cast<unsigned char>(pattern[patternIndex])) ==
			std::tolower(static_cast<unsigned char>(value[valueIndex]))) {
			++patternIndex;
			++valueIndex;
		} else if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
			starIndex = patternIndex++;
			starValueIndex = valueIndex;
		} else if (starIndex != std::string::npos) {
			patternIndex = starIndex + 1;
			valueIndex = ++starValueIndex;
		} else {
			return false;
		}
	}
	while (patternIndex < pattern.size() && pattern[patternIndex] == '*') ++patternIndex;
	return patternIndex == pattern.size();
}

auto dev::TraceLog::MakeQueryEntry(const Item& item, const DebugData& debugData) -> QueryEntry
{
	DisasmLine line;
	line.InitInstr(static_cast<Addr>(item.globalAddr), item.instr, debugData, false);
	std::string instruction;
	for (size_t index = 0; index < line.cmdP->token_types.size(); ++index) {
		if (line.cmdP->token_types[index] == CMD_TT_IMM) {
			instruction += line.cmdP->imm_type == CMD_IT_B0 || line.cmdP->imm_type == CMD_IT_B1 ?
				dev::Uint8ToStrC0x(line.imm) : dev::Uint16ToStrC0x(line.imm);
		} else {
			instruction += line.cmdP->tokens[index];
		}
	}
	const auto length = CpuI8080::GetInstrLen(item.instr.opcode);
	std::vector<uint8_t> bytes{item.instr.opcode};
	if (length > 1) bytes.push_back(item.instr.dataL);
	if (length > 2) bytes.push_back(item.instr.dataH);
	return {static_cast<uint16_t>(item.globalAddr), std::move(bytes), std::move(instruction)};
}

auto dev::TraceLog::CreateFilter(const std::string& addressPattern,
	const std::string& instructionPattern) -> nlohmann::json
{
	InvalidateFilter();
	const auto lastIndex = m_logIdx + TRACE_LOG_SIZE - 1;
	for (auto index = m_logIdx; index <= lastIndex; ++index) {
		const auto& item = m_log[index % TRACE_LOG_SIZE];
		if (item.globalAddr == EMPTY_ITEM) break;
		auto entry = MakeQueryEntry(item, m_debugData);
		const auto address = dev::Uint16ToStrC0x(entry.address);
		if ((addressPattern.empty() || GlobMatches(addressPattern, address)) &&
			(instructionPattern.empty() || GlobMatches(instructionPattern, entry.instruction))) {
			m_filteredEntries.push_back(std::move(entry));
		}
	}
	m_activeFilterId = ++m_filterGeneration;
	return {{"filterId", m_activeFilterId},
		{"totalMatches", m_filteredEntries.size()}};
}

auto dev::TraceLog::GetFilterWindow(const uint64_t filterId, const size_t start,
	const size_t lines) const -> nlohmann::json
{
	if (filterId != m_activeFilterId || m_activeFilterId == 0)
		throw TraceLogQueryError("unknown or expired trace-log filterId");
	if (start > m_filteredEntries.size()) throw TraceLogQueryError("trace-log window start is outside the filtered result");
	nlohmann::json entries = nlohmann::json::array();
	const auto end = std::min(start + lines, m_filteredEntries.size());
	for (auto index = start; index < end; ++index) {
		const auto& entry = m_filteredEntries[index];
		entries.push_back({{"address", entry.address}, {"bytes", entry.bytes},
			{"instruction", entry.instruction}});
	}
	return {{"start", start}, {"entries", std::move(entries)}};
}

// UI thread
auto dev::TraceLog::GetDisasm(const size_t _lines, const uint8_t _filter)
-> const Lines*
{
	size_t idxLast = m_logIdx + TRACE_LOG_SIZE - 1;
	m_disasmLinesLen = 0;
	auto idx = m_logIdx;
	int line = 0;

	for (; idx <= idxLast && line < _lines; idx++)
	{
		auto& item = m_log[idx % TRACE_LOG_SIZE];
		auto globalAddr = item.globalAddr;

		if (globalAddr == EMPTY_ITEM) { break; }

		if (CpuI8080::GetInstrType(item.instr.opcode) <= _filter)
		{
			m_disasmLines[m_disasmLinesLen].InitInstr(globalAddr, item.instr, m_debugData, false);
			m_disasmLinesLen++;
			line++;
		}
	}

	return &m_disasmLines;
}



void dev::TraceLog::Reset()
{
	m_historyCount = 0;
	++m_historyGeneration;
	m_disasmLinesLen = m_logIdx = 0;
	m_log[m_logIdx].globalAddr = EMPTY_ITEM;
	InvalidateFilter();

	// create a new log file
	if (m_saveLogInited)
	{
		SetSaveLog(false);
		SetSaveLog(true);
	}
}


void dev::TraceLog::SetSaveLog(bool _saveLog, const std::string& _path)
{
	if (_saveLog)
	{
		if (!m_saveLogInited)
		{
			m_saveLogPath = !_path.empty() ? _path :
				!m_saveLogPath.empty() ? m_saveLogPath :
				GetDefaultLogPath();

			// create the log file
			m_logFile.open(m_saveLogPath, std::ios::out | std::ios::trunc);

			// handle error
			if (!m_logFile)
			{
				dev::Log(
					"Trace Log: Failed to open log file: {}", m_saveLogPath);
				m_saveLog = false;
			}
			else{
				m_saveLogInited = true;
				m_saveLog = true;
			}
		}
	}
	else{
		if (m_logFile.is_open()){
			m_logFile.close();
		}
		m_saveLogInited = false;
		m_saveLog = false;
	}
}

std::array<char, dev::DisasmLine::LINE_BUFF_LEN> _traceLogBuffer = {};

void dev::TraceLog::SaveLog(
	const CpuI8080::State& _cpuState,
	const Memory::State& _memState,
	const Display::State& _displayState)
{
	if (m_saveLog && m_logFile.is_open())
	{
		m_logFile << DisasmLine::PrintToBuffer(_traceLogBuffer,
											   _cpuState,
											   _memState,
											   _displayState,
											   false);
	}
}


auto dev::TraceLog::GetDefaultLogPath()
-> std::string
{
	return dev::GetExecutableDir() + GetLogFilename();
}

auto dev::TraceLog::GetLogFilename()
-> std::string
{
	auto saveLogFilename = std::format("{}_{}.txt",
		TRACE_LOG_NAME,
		dev::FormatLocalTime(std::chrono::system_clock::now(), "%Y-%m-%d_%H-%M"));

	return saveLogFilename;
}