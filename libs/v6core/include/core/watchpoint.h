#pragma once

#include <memory>
#include <mutex>
#include <map>
#include <vector>
#include <format>

#include "utils/types.h"
#include "utils/str_utils.h"
#include "utils/utils.h"
#include "core/cpu_i8080.h"
#include "core/memory.h"
#include "utils/json_utils.h"

namespace dev
{
	static const char* wpAccessS[] = { "R", "W", "RW" };
	static const char* wpTypesS[] = { "LEN", "WORD" };

	struct Watchpoint
	{
		// LEN - breaks if the condition succeds for any bytes in m_len range
		// WORD - breaks if the condition succeds for a word
		enum class Type : uint8_t { LEN = 0, WORD, COUNT };
		enum class Access : uint8_t { R = 0, W, RW, COUNT };

		static auto GetAccess(const std::string _accessS)
		-> Access
		{
			for (int i = 0; i < static_cast<int>(Access::COUNT); i++)
			{
				if (std::string(wpAccessS[i]) == _accessS)
				{
					return static_cast<Access>(i);
				}
			}
			return Access::COUNT;
		}

		static auto GetType(const std::string _typeS)
		-> Type
		{
			for (int i = 0; i < static_cast<int>(Type::COUNT); i++)
			{
				if (std::string(wpTypesS[i]) == _typeS)
				{
					return static_cast<Type>(i);
				}
			}
			return Type::COUNT;
		}

		static auto GetStructuredCondition(const std::string& _condition) -> Condition
		{
			return ParseConditionName(_condition);
		}

		struct Data {
			GlobalAddr globalAddr;
			Id id;
			GlobalAddr len;
			uint16_t value;
			Access access;
			Condition cond;
			Type type;
			bool active;
			bool breakL;
			bool breakH;

			Data(
				const Id _id, const Access _access, const GlobalAddr _globalAddr, const Condition _cond,
				const uint16_t _value, const Type _type = Type::LEN, const GlobalAddr _len = 1,
				const bool _active = true,
				const bool _breakH = false, const bool _breakL = false
			) :
				id(_id), access(_access), globalAddr(_globalAddr),
				cond(_cond), value(_value), type(_type), len(_len), active(_active), breakH(_breakH), breakL(_breakL)
			{};
			Data(const nlohmann::json& _wpJ) :
				Data(_wpJ["id"],
					GetAccess(_wpJ["access"].get<std::string>()),
					dev::StrHexToInt(_wpJ["globalAddr"].get<std::string>()),
					GetCondition(_wpJ["cond"].get<std::string>()),
					dev::StrHexToInt(_wpJ["value"].get<std::string>()),
					GetType(_wpJ["type"].get<std::string>()),
					dev::StrHexToInt(_wpJ["len"].get<std::string>()),
					_wpJ["active"])
			{};
		};

		Watchpoint(Data&& _data, const std::string& _comment = "");

		void Update(Watchpoint&& _wp);

		auto Check(const Access _access, const GlobalAddr _globalAddr, const uint8_t _value) -> const bool;
		auto GetAccessI() const -> int;
		auto GetComment() const -> const std::string& { return comment;  };
		auto GetConditionS() const -> const char*;
		auto GetAccessS() const -> const char*;
		auto GetTypeS() const -> const char*;
		void Reset();
		void Print() const;
		auto ToJson() const -> nlohmann::json
		{
			return {
				{"id", data.id},
				{"globalAddr", data.globalAddr},
				{"len", data.len},
				{"value", data.value},
				{"access", GetAccessS()},
				{"condition", ConditionNames[static_cast<uint8_t>(data.cond)]},
				{"type", GetTypeS()},
				{"active", data.active},
				{"comment", comment}
			};
		};

		Data data;
		std::string comment;
	};
}