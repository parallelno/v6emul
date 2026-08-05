#include <string>

#include "core/breakpoints.h"
#include "utils/str_utils.h"
#include "utils/utils.h"

void dev::Breakpoints::Clear()
{
	if (m_bps.empty()) return;
	m_bps.clear();
	m_updates++;
}

void dev::Breakpoints::SetStatus(const Addr _addr, const Breakpoint::Status _status)
{
	auto bpI = m_bps.find(_addr);
	if (bpI == m_bps.end() || bpI->second.data.structured.status == _status) return;
	bpI->second.data.structured.status = _status;
	m_updates++;
}

void dev::Breakpoints::Add(Breakpoint&& _bp )
{
	m_updates++;
	auto bpI = m_bps.find(_bp.data.structured.addr);
	if (bpI != m_bps.end())
	{
		bpI->second.Update(std::move(_bp));
		return;
	}

	m_bps.emplace(static_cast<Addr>(_bp.data.structured.addr), std::move(_bp));
}

void dev::Breakpoints::Add(const nlohmann::json& _bpJ)
{
	m_updates++;

	Breakpoint::Data bpData {_bpJ};
	Breakpoint bp{ std::move(bpData), _bpJ["comment"] };

	auto bpI = m_bps.find(bp.data.structured.addr);
	if (bpI != m_bps.end())
	{
		bpI->second.Update(std::move(bp));
		return;
	}

	m_bps.emplace(static_cast<Addr>(bp.data.structured.addr), std::move(bp));
}

void dev::Breakpoints::Del(const Addr _addr)
{
	auto bpI = m_bps.find(_addr);
	if (bpI != m_bps.end())
	{
		m_bps.erase(bpI);
		m_updates++;
	}
}

auto dev::Breakpoints::GetStatus(const Addr _addr)
-> const Breakpoint::Status
{
	auto bpI = m_bps.find(_addr);
	return bpI == m_bps.end() ? Breakpoint::Status::DELETED : bpI->second.data.structured.status;
}

bool dev::Breakpoints::Check(const CpuI8080::State& _cpuState, const Memory::State& _memState)
{
	auto bpI = m_bps.find(_cpuState.regs.pc.word);
	if (bpI == m_bps.end()) return false;

	if (!bpI->second.CheckStatus(_cpuState, _memState)) return false;

	auto& data = bpI->second.data.structured;
	const bool counterChanged = data.counter > 0;
	if (counterChanged) data.counter--;
	const bool shouldBreak = data.counter == 0;
	if (shouldBreak && data.autoDel) {
		m_bps.erase(bpI);
		m_updates++;
	} else if (counterChanged) {
		m_updates++;
	}
	return shouldBreak;
}

auto dev::Breakpoints::GetAll()
-> const BpMap&
{
	return m_bps;
}

auto dev::Breakpoints::GetUpdates()
-> const uint32_t
{
	return m_updates;
}