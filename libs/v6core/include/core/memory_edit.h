#pragma once

#include <string>
#include <format>
#include <stdexcept>

#include "utils/types.h"
#include "utils/str_utils.h"
#include "utils/json_utils.h"

namespace dev
{
	struct MemoryEdit
	{
		std::string comment;
		GlobalAddr globalAddr = 0;
		uint8_t enteredValue = 0;
		uint8_t originalValue = 0;
		bool readonly = true; // if true, memory is not modified
		bool active = true;

		auto AddrToStr() const -> std::string { return std::format("0x{:06x}: 0x{:02x} {}, {}", globalAddr, enteredValue, readonly ? "read-only" : "", active ? "active" : "not active"); }
		void Erase()
		{
			comment.clear();
			globalAddr = 0;
			enteredValue = 0;
			originalValue = 0;
			readonly = true;
			active = true;
		}

		auto ToStorageJson() const -> nlohmann::json
		{
			return {
				{"comment", comment},
				{"globalAddr", std::format("0x{:06X}", globalAddr)},
				{"value", std::format("0x{:02X}", enteredValue)},
				{"originalValue", std::format("0x{:02X}", originalValue)},
				{"readonly", readonly},
				{"active", active}
			};
		}

		auto ToSnapshotJson(const uint8_t _currentValue) const -> nlohmann::json
		{
			return {
				{"globalAddr", globalAddr},
				{"enteredValue", enteredValue},
				{"originalValue", originalValue},
				{"currentValue", _currentValue},
				{"readonly", readonly},
				{"active", active},
				{"comment", comment}
			};
		}

		MemoryEdit() = default;

		static auto FromStorageJson(const nlohmann::json& _json, const uint8_t _fallbackOriginalValue)
			-> MemoryEdit
		{
			return MemoryEdit(
				dev::StrHexToInt(_json["globalAddr"].get<std::string>()),
				dev::StrHexToInt(_json["value"].get<std::string>()),
				_json.contains("originalValue")
					? dev::StrHexToInt(_json["originalValue"].get<std::string>())
					: _fallbackOriginalValue,
				_json["comment"].get<std::string>(),
				_json["readonly"].get<bool>(),
				_json["active"].get<bool>());
		}

		MemoryEdit(GlobalAddr _globalAddr, uint8_t _enteredValue, uint8_t _originalValue,
			const std::string& _comment = "", bool _readonly = true, bool _active = true)
			:
			comment(_comment),
			globalAddr(_globalAddr),
			enteredValue(_enteredValue),
			originalValue(_originalValue),
			readonly(_readonly),
			active(_active) {}
	};

	class MemoryEditNotFound : public std::runtime_error
	{
	public:
		explicit MemoryEditNotFound(const GlobalAddr _globalAddr)
			: std::runtime_error(std::format("memory edit at global address {} does not exist", _globalAddr)),
			m_globalAddr(_globalAddr) {}

		auto GetGlobalAddr() const -> GlobalAddr { return m_globalAddr; }

	private:
		GlobalAddr m_globalAddr;
	};
}