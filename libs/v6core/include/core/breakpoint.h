#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <vector>
#include <format>

#include "utils/types.h"
#include "utils/consts.h"
#include "utils/utils.h"
#include "utils/str_utils.h"
#include "core/cpu_i8080.h"
#include "core/memory.h"
#include "utils/json_utils.h"

namespace dev
{
	static const char* bpOperandsS[] = {
		"A", "F", "B", "C", "D", "E",
		"H", "L", "PSW", "BC", "DE", "HL", "CC", "SP" };

	struct Breakpoint
	{
		struct MemPages {
			uint64_t data;
			explicit MemPages(const uint64_t _data) : data(_data) {};
		};

		static constexpr uint64_t MAPPING_PAGES_ALL = (uint64_t{1} << 33) - 1;
		enum class Status : uint8_t {
			DISABLED = 0,
			ACTIVE,
			DELETED,
			COUNT,
		};
		enum class Operand : uint8_t { A = 0, F, B, C, D, E, H, L, PSW, BC, DE, HL, CC, SP, COUNT };

		static auto GetOperand(std::string operandS)
		-> Operand
		{
			if (operandS == "Flags") return Operand::F;
			for (size_t i = 0; i < sizeof(bpOperandsS) / sizeof(bpOperandsS[0]); i++)
			{
				if (bpOperandsS[i] == operandS)
				{
					return static_cast<Operand>(i);
				}
			}
			return Operand::COUNT;
		}

		static auto GetStatus(const std::string& status) -> Status
		{
			if (status == "ACTIVE") return Status::ACTIVE;
			if (status == "DISABLED") return Status::DISABLED;
			return Status::COUNT;
		}

		static auto GetStatusS(const Status status) -> const char*
		{
			switch (status) {
			case Status::ACTIVE: return "ACTIVE";
			case Status::DISABLED: return "DISABLED";
			case Status::DELETED: return "DELETED";
			default: return "INVALID";
			}
		}

		struct DataStruct {
			MemPages memPages;
			uint64_t value;
			uint64_t counter;
			Addr addr;
			Operand operand;
			Condition cond;
			Status status;
			bool autoDel;

			DataStruct(
				const Addr _addr,
				const MemPages _memPages = MemPages{MAPPING_PAGES_ALL},
				const Status _status = Status::ACTIVE,
				const bool _autoDel = false,
				const Operand _operand = Operand::A,
				const Condition _cond = Condition::ANY,
				const uint64_t _value = 0,
				const uint64_t _counter = 1
			) :
				memPages(_memPages), value(_value), counter(_counter), addr(_addr), operand(_operand), cond(_cond), status(_status), autoDel(_autoDel)
			{};
		};

		struct Data {
			DataStruct structured;

			Data(
				const Addr _addr,
				const MemPages _memPages = MemPages{MAPPING_PAGES_ALL},
				const Status _status = Status::ACTIVE,
				const bool _autoDel = false,
				const Operand _operand = Operand::A,
				const Condition _cond = Condition::ANY,
				const uint64_t _value = 0,
				const uint64_t _counter = 1
			) :
				structured(_addr, _memPages, _status, _autoDel, _operand, _cond, _value, _counter)
			{};
			Data(const nlohmann::json& _bpJ) :
				structured(
					dev::StrHexToInt(_bpJ["addr"].get<std::string>()),
					static_cast<MemPages>(_bpJ["memPages"]),
					static_cast<Status>(_bpJ["status"]),
					_bpJ["autoDel"],
					GetOperand(_bpJ["operand"].get<std::string>()),
					dev::GetCondition(_bpJ["cond"].get<std::string>()),
					dev::StrHexToInt(_bpJ["value"].get<std::string>()),
					_bpJ.value("counter", uint64_t{1})
				)
			{};
		};

		Breakpoint(Data&& _data, const std::string& _comment = "");

		void Update(Breakpoint&& _bp);

		auto GetAddrMappingS() const -> const char*;
		bool IsActive() const { return data.structured.status == Status::ACTIVE; };
		bool CheckStatus(const CpuI8080::State& _cpuState, const Memory::State& _memState) const;
		auto GetOperandS() const -> const char*;
		auto GetConditionS() const -> const std::string;
		void Print() const;
		auto IsActiveS() const -> const char*;
		void UpdateAddrMappingS();
		auto ToJson() const -> nlohmann::json
		{
			return {
				{"addr", std::format("0x{:04X}", data.structured.addr)},
				{"memPages", data.structured.memPages.data},
				{"status", static_cast<uint32_t>(data.structured.status)},
				{"autoDel", data.structured.autoDel},
				{"operand", GetOperandS()},
				{"cond", dev::ConditionsS[static_cast<uint8_t>(data.structured.cond)]},
				{"value", std::format("0x{:02X}", data.structured.value)},
				{"counter", data.structured.counter},
				{"comment", comment}
			};
		};
		auto ToProtocolJson() const -> nlohmann::json
		{
			return {
				{"addr", data.structured.addr},
				{"memPages", data.structured.memPages.data},
				{"status", GetStatusS(data.structured.status)},
				{"autoDelete", data.structured.autoDel},
				{"operand", GetOperandS()},
				{"condition", dev::ConditionNames[static_cast<uint8_t>(data.structured.cond)]},
				{"value", data.structured.value},
				{"counter", data.structured.counter},
				{"comment", comment}
			};
		}

		Data data;
		std::string comment;

		std::string addrMappingS;
	};
}