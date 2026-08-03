#include <format>

#include "core/debug_data.h"

dev::DebugData::DebugData(Hardware& _hardware)
	:
	m_hardware(_hardware),
	m_scripts(std::bind(&DebugData::GetLabelAddr, this, std::placeholders::_1)),
	m_memRuns(), m_memReads(), m_memWrites()
{}

void dev::DebugData::Reset()
{
	m_memRuns.fill(0);
	m_memReads.fill(0);
	m_memWrites.fill(0);
}

bool IsConstLabel(const char* _s)
{
	// Iterate through each character in the string
	while (*_s != '\0') {
		// Check if the character is uppercase or underscore
		if (!(std::isupper(*_s) || *_s == '_' || (*_s >= '0' && *_s <= '9'))) {
			return false; // Not all characters are capital letters or underscores
		}
		++_s; // Move to the next character
	}
	return true; // All characters are capital letters or underscores
}

// returns a copies of labels for the given addr.
// local labels are put at the end of the list.
// if no labels are found, returns std::nullopt.
auto dev::DebugData::GetLabels(const Addr _addr) const
-> std::optional<LabelList>
{
	auto labelsI = m_labels.find(_addr);
	if (labelsI == m_labels.end()){
		return std::nullopt;
	}
	LabelList labels;
	for (auto& label : labelsI->second)
	{
		// put local labels at the end of the label list
		if (!label.empty() && label[0] == '@') {
			labels.push_back(label);
		}
		else{
			labels.insert(labels.begin(), label);
		}
	}

	return labels;
}

auto dev::DebugData::HasLabels(const Addr _addr) const
-> bool
{
	auto labelsI = m_labels.find(_addr);
	return labelsI != m_labels.end();
}

auto dev::DebugData::GetLabelAddr(const std::string& _label)
-> int
{
	for (const auto& [addr, labels] : m_labels)
	{
		for (const auto& label : labels)
		{
			if (label == _label) {
				return addr;
			}
		}
	}
	return -1;
}

void dev::DebugData::GetFilteredLabels(FilteredElements& _out, const std::string& _filter) const
{
	_out.clear();
	for (const auto& [globalAddr, labels] : m_labels)
	{
		for(const auto& label : labels)
		{
			if (label.find(_filter) == std::string::npos) continue;

			_out.push_back({ label, globalAddr, std::format("0x{:06x}", globalAddr) });
		}
	}

	// alphabetical	sort
	std::sort(_out.begin(), _out.end(), [](const auto& lhs, const auto& rhs) {
		return std::get<0>(lhs) < std::get<0>(rhs);
	});
}

void dev::DebugData::SetLabels(const Addr _addr, const LabelList& _labels)
{
	if (_labels.empty()) {
		auto labelsI = m_labels.find(_addr);
		if (labelsI != m_labels.end()) m_labels.erase(labelsI);
	}
	else {
		m_labels[_addr] = _labels;
	}

	m_labelsUpdates++;
}

void dev::DebugData::AddLabel(const Addr _addr, const std::string& _label)
{
	auto labelsI = m_labels.find(_addr);
	if (labelsI == m_labels.end()) {
		m_labels[_addr] = LabelList{ _label };
	}
	else {
		labelsI->second.push_back(_label);
	}
	m_labelsUpdates++;
}

void dev::DebugData::DelLabel(const Addr _addr, const std::string& _label)
{
	auto labelsI = m_labels.find(_addr);
	if (labelsI != m_labels.end())
	{
		auto labelI = std::find(labelsI->second.begin(), labelsI->second.end(), _label);
		if (labelI != labelsI->second.end())
		{
			labelsI->second.erase(labelI);
			if (labelsI->second.empty()) {
				m_labels.erase(labelsI);
			}
			m_labelsUpdates++;
		}
	}
}

void dev::DebugData::DelLabels(const Addr _addr)
{
	auto labelsI = m_labels.find(_addr);
	if (labelsI == m_labels.end()) return;

	m_labels.erase(labelsI);
	m_labelsUpdates++;
}

void dev::DebugData::DelAllLabels()
{
	m_labels.clear();
	m_labelsUpdates++;
}

void dev::DebugData::RenameLabel(const Addr _addr, const std::string& _oldLabel, const std::string& _newLabel)
{
	auto labelsI = m_labels.find(_addr);
	if (labelsI != m_labels.end())
	{
		auto labelI = std::find(labelsI->second.begin(), labelsI->second.end(), _oldLabel);
		if (labelI != labelsI->second.end())
		{
			*labelI = _newLabel;
			m_labelsUpdates++;
		}
	}
}

auto dev::DebugData::GetConsts(const Addr _addr) const -> const LabelList*
{
	auto constsI = m_consts.find(_addr);
	return constsI != m_consts.end() ? &constsI->second : nullptr;
}

void dev::DebugData::GetFilteredConsts(FilteredElements& _out, const std::string& _filter) const
{
	_out.clear();
	for (const auto& [globalAddr, consts] : m_consts)
	{
		for(const auto& const_ : consts)
		{
			if (const_.find(_filter) == std::string::npos) continue;

			_out.push_back({ const_, globalAddr, std::format("0x{:06x}", globalAddr) });
		}
	}

	// alphabetical	sort
	std::sort(_out.begin(), _out.end(), [](const auto& lhs, const auto& rhs) {
		return std::get<0>(lhs) < std::get<0>(rhs);
	});
}

void dev::DebugData::SetConsts(const Addr _addr, const LabelList& _consts)
{
	if (_consts.empty()) {
		// del the consts at the addr if the list of consts is empty
		auto constsI = m_consts.find(_addr);
		if (constsI != m_consts.end()) m_consts.erase(constsI);
	}
	else {
		m_consts[_addr] = _consts;
	}

	m_constsUpdates++;
}

void dev::DebugData::AddConst(const Addr _addr, const std::string& _const)
{
	auto constsI = m_consts.find(_addr);
	if (constsI == m_consts.end()) {
		m_consts[_addr] = LabelList{ _const };
	}
	else {
		constsI->second.push_back(_const);
	}
	m_constsUpdates++;
}

void dev::DebugData::DelConst(const Addr _addr, const std::string& _const)
{
	auto constsI = m_consts.find(_addr);
	if (constsI != m_consts.end()) {
		auto constI = std::find(constsI->second.begin(), constsI->second.end(), _const);
		if (constI != constsI->second.end())
		{
			constsI->second.erase(constI);
			if (constsI->second.empty()) {
				m_consts.erase(constsI);
			}
			m_constsUpdates++;
		}
	}
}

void dev::DebugData::DelConsts(const Addr _addr)
{
	auto constsI = m_consts.find(_addr);
	if (constsI == m_consts.end()) return;

	m_consts.erase(constsI);
	m_constsUpdates++;
}

void dev::DebugData::DelAllConsts()
{
	m_consts.clear();
	m_constsUpdates++;
}

void dev::DebugData::RenameConst(const Addr _addr, const std::string& _oldConst, const std::string& _newConst)
{
	auto constsI = m_consts.find(_addr);
	if (constsI != m_consts.end()) {
		auto constI = std::find(constsI->second.begin(), constsI->second.end(), _oldConst);
		if (constI != constsI->second.end()) {
			*constI = _newConst;
			m_constsUpdates++;
		}
	}
}


auto dev::DebugData::GetComment(const Addr _addr) const
-> const std::string*
{
	auto commentI = m_comments.find(_addr);
	return commentI != m_comments.end() ? &commentI->second : nullptr;
}

void dev::DebugData::GetFilteredComments(FilteredElements& _out, const std::string& _filter) const
{
	_out.clear();
	for (const auto& [globalAddr, comment] : m_comments)
	{
		if (comment.find(_filter) == std::string::npos) continue;

		_out.push_back({ comment, globalAddr, std::format("0x{:06x}", globalAddr) });
	}

	// sort by addr
	std::sort(_out.begin(), _out.end(), [](const auto& lhs, const auto& rhs) {
		return std::get<1>(lhs) < std::get<1>(rhs);
	});
}

void dev::DebugData::SetComment(const Addr _addr, const std::string& _comment)
{
	m_comments[_addr] = _comment;
	m_commentsUpdates++;
}

void dev::DebugData::DelComment(const Addr _addr)
{
	auto commentI = m_comments.find(_addr);
	if (commentI == m_comments.end()) return;
	m_comments.erase(commentI);
	m_commentsUpdates++;
}

void dev::DebugData::DelAllComments()
{
	m_comments.clear();
	m_commentsUpdates++;
}

auto dev::DebugData::GetMemoryEdit(const GlobalAddr _globalAddr) const
-> const MemoryEdit*
{
	auto editsI = m_memoryEdits.find(_globalAddr);
	return editsI != m_memoryEdits.end() ? &editsI->second : nullptr;
}

void dev::DebugData::SetMemoryEdit(const MemoryEdit& _edit)
{
	m_memoryEdits[_edit.globalAddr] = _edit;
	m_editsUpdates++;
}

void dev::DebugData::DelMemoryEdit(const GlobalAddr _globalAddr)
{
	auto editI = m_memoryEdits.find(_globalAddr);
	if (editI == m_memoryEdits.end()) return;
	m_memoryEdits.erase(editI);
	m_editsUpdates++;
}

void dev::DebugData::DelAllMemoryEdits()
{
	m_memoryEdits.clear();
	m_editsUpdates++;
}

void dev::DebugData::GetFilteredMemoryEdits(FilteredElements& _out, const std::string& _filter) const
{
	_out.clear();
	for (const auto& [globalAddr, edit] : m_memoryEdits)
	{
		if (edit.comment.find(_filter) == std::string::npos) continue;
		_out.push_back({ edit.comment, globalAddr, edit.AddrToStr() });
	}
}

////////////////////

auto dev::DebugData::GetCodePerf(const Id _id) const
-> const CodePerf*
{
	auto codePerfI = m_codePerfs.find(_id);
	return codePerfI != m_codePerfs.end() ? &codePerfI->second : nullptr;
}

auto dev::DebugData::SetCodePerf(const CodePerf& _codePerf) -> Id
{
	const auto id = m_nextCodePerfId++;
	m_codePerfs.emplace(id, _codePerf);
	m_codePerfsUpdates++;
	return id;
}

void dev::DebugData::DelCodePerf(const Id _id)
{
	auto codePerfI = m_codePerfs.find(_id);
	if (codePerfI == m_codePerfs.end()) return;
	m_codePerfs.erase(codePerfI);
	m_codePerfsUpdates++;
}

void dev::DebugData::DelAllCodePerfs()
{
	m_codePerfs.clear();
	m_codePerfsUpdates++;
}


auto dev::DebugData::CheckCodePerfs(const Addr _addrStart, const uint64_t _cc) -> bool
{
	for (auto& [id, codePerf] : m_codePerfs)
	{
		codePerf.CheckPerf(_addrStart, _cc);
	}
	return false;
}
////////////////////
