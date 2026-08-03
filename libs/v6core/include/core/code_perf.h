#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "utils/types.h"
#include "utils/json_utils.h"

namespace dev
{
	struct CodePerf
	{
		static constexpr int64_t MAX_TEST_COUNT = 20000;
		static constexpr size_t MAX_NAME_BYTES = 1024;
		static constexpr size_t MAX_RECORDS = 256;

		std::string name;
		Addr addrStart = 0;
		Addr addrEnd = 0x100;
		double averageClockCycles = 0.0;
		int64_t testCount = 0;
		std::optional<uint64_t> sampleStartClock;
		bool active = true;

		void CancelSample()
		{
			sampleStartClock.reset();
		}

		void ResetStatistics()
		{
			averageClockCycles = 0.0;
			testCount = 0;
			CancelSample();
		}

		void Edit(const CodePerf& _codePerf)
		{
			const bool endpointsChanged =
				addrStart != _codePerf.addrStart || addrEnd != _codePerf.addrEnd;

			name = _codePerf.name;
			if (endpointsChanged)
			{
				addrStart = _codePerf.addrStart;
				addrEnd = _codePerf.addrEnd;
				ResetStatistics();
			}
			else if (active != _codePerf.active)
			{
				CancelSample();
			}
			active = _codePerf.active;
		}

		void CheckPerf(const Addr _addr, const uint64_t _cc)
		{
			if (!active || testCount >= MAX_TEST_COUNT) return;

			if (addrStart == _addr)
			{
				sampleStartClock = _cc;
				return;
			}

			if (addrEnd == _addr && sampleStartClock)
			{
				if (_cc < *sampleStartClock)
				{
					CancelSample();
					return;
				}

				testCount++;
				const auto duration = static_cast<double>(_cc - *sampleStartClock);
				const auto weight = 1.0 / static_cast<double>(testCount);
				averageClockCycles += (duration - averageClockCycles) * weight;
				CancelSample();
			}
		}

		CodePerf() = default;

		CodePerf(const nlohmann::json& _json)
			:
			name(_json["name"].get<std::string>()),
			addrStart(_json["addrStart"].get<Addr>()),
			addrEnd(_json["addrEnd"].get<Addr>()),
			active(_json["active"].get<bool>())
		{}

		auto ToSnapshotJson(const Id _id) const -> nlohmann::json
		{
			return {
				{"id", _id},
				{"name", name},
				{"addrStart", addrStart},
				{"addrEnd", addrEnd},
				{"active", active},
				{"averageClockCycles", averageClockCycles},
				{"testCount", testCount}
			};
		}
	};
}