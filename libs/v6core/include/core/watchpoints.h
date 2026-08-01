#pragma once

#include <stdexcept>
#include <string>
#include <optional>
#include <vector>

#include "utils/types.h"
#include "core/watchpoint.h"

namespace dev
{
	class WatchpointNotFound : public std::runtime_error
	{
	public:
		explicit WatchpointNotFound(const Id id)
			: std::runtime_error("watchpoint id not found"), m_id(id) {}

		auto GetId() const -> Id { return m_id; }

	private:
		Id m_id;
	};

	struct Watchpoints
	{
	public:
		using WpMap = std::unordered_map<dev::Id, Watchpoint>;
		struct Hit {
			std::vector<Id> ids;
			Watchpoint::Access access;
			GlobalAddr globalAddr;
			uint8_t value;
			std::optional<uint8_t> oldValue;
		};

		void Add(Watchpoint&& _bp);
		auto AddNew(Watchpoint&& _wp) -> Id;
		auto Edit(Watchpoint&& _wp) -> bool;
		void Add(const nlohmann::json& _wpJ);
		void Del(const dev::Id _id);
		void Check(const Watchpoint::Access _access, const GlobalAddr _globalAddr,
			const uint8_t _value, const std::optional<uint8_t> _oldValue = std::nullopt);
		auto GetAll() -> const WpMap&;
		auto GetUpdates() -> const uint32_t;
		auto GetHit() const -> const std::optional<Hit>& { return m_hit; }
		void Clear();
		bool CheckBreak();

	private:

		WpMap m_wps;
		uint32_t m_updates = 0; // counts number of updates
		bool m_wpBreak = false;
		std::optional<Hit> m_hit;
		Id m_nextId = 0;
	};
}