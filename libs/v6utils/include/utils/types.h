#pragma once

#include <cstdint>
#include <bit>
#include <string_view>

namespace dev
{
	using GlobalAddr = uint32_t;
	using Addr = uint16_t;
	using ColorI = uint32_t;
	using Id = int;
	using Idx = int;

	enum class UIItemMouseAction { NONE = 0, HOVERED, LEFT, RIGHT, MIDDLE };
	enum class Condition : uint8_t { ANY = 0, EQU, LESS, GREATER, LESS_EQU, GREATER_EQU, NOT_EQU, INVALID, COUNT };
	static constexpr int CONDITION_BIT_WIDTH = std::bit_width<uint8_t>(static_cast<uint8_t>(Condition::COUNT) - 1);
	inline constexpr const char* ConditionNames[] = {
		"ANY", "EQU", "LESS", "GREATER", "LESS_EQU", "GREATER_EQU", "NOT_EQU"
	};

	inline auto ParseConditionName(const std::string_view name) -> Condition
	{
		for (uint8_t index = 0; index < static_cast<uint8_t>(Condition::INVALID); index++) {
			if (ConditionNames[index] == name) return static_cast<Condition>(index);
		}
		return Condition::INVALID;
	}
}