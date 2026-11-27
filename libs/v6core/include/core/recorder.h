#pragma once

#include <algorithm>
#include <deque>
#include <string_view>
#include <vector>
#include "utils/json_utils.h"

namespace dev
{
	template<class State>
	class RecorderTimeline
	{
	public:
		static constexpr size_t MAX_FRAMES = 3000;
		static constexpr size_t MAX_INSTRUCTIONS = 1000;
		struct Point { uint64_t id = 0, journal = 0; State state{}; };
		struct Change { uint32_t address; uint8_t before, after; };

		void Reset(const State& state)
		{
			frames.clear(); instructions.clear(); changes.clear(); journalBase = 0;
			cursor = tip = {++sequence, 0, state};
			++revision;
		}
		bool Browsing() const { return cursor.id != tip.id; }
		void Branch()
		{
			if (!Browsing()) return;
			while (!frames.empty() && frames.back().id > cursor.id) frames.pop_back();
			while (!instructions.empty() && instructions.back().after.id > cursor.id) instructions.pop_back();
			while (JournalEnd() > cursor.journal) changes.pop_back();
			tip = cursor;
			++revision;
		}
		void Write(uint32_t address, uint8_t before, uint8_t after)
		{
			if (before != after) changes.push_back({address, before, after});
		}
		const State& Current() const { return cursor.state; }
		void Commit(const State& state, bool instruction, bool frame)
		{
			const Point before = cursor;
			cursor = tip = {++sequence, JournalEnd(), state};
			if (instruction) {
				instructions.push_back({before, tip});
			}
			if (frame) {
				frames.push_back(tip);
				if (frames.size() > MAX_FRAMES) frames.pop_front();
			}
			constexpr size_t frameJournalBudget = 32 * 1024 * 1024 / sizeof(Change);
			while (!frames.empty() && JournalEnd() - frames.front().journal > frameJournalBudget) frames.pop_front();
			uint64_t first = tip.journal;
			if (!frames.empty()) first = std::min(first, frames.front().journal);
			if (!instructions.empty()) first = std::min(first, instructions.front().before.journal);
			while (journalBase < first) { changes.pop_front(); ++journalBase; }
			++revision;
		}

		template<class Apply>
		bool Move(std::string_view action, Apply apply, uint64_t targetId = 0)
		{
			const Point* target = nullptr;
			if (action == "seek") {
				if (targetId == cursor.id) return false;
				if (targetId == tip.id) target = &tip;
				const auto frame = std::lower_bound(frames.begin(), frames.end(), targetId,
					[](const Point& p, uint64_t id) { return p.id < id; });
				if (frame != frames.end() && frame->id == targetId) target = &*frame;
				const auto before = InstructionBoundary([&](const Instruction& entry) { return entry.before.id < targetId; });
				if (before < instructions.size() && instructions[before].before.id == targetId) target = &instructions[before].before;
				const auto after = InstructionBoundary([&](const Instruction& entry) { return entry.after.id < targetId; });
				if (after < instructions.size() && instructions[after].after.id == targetId) target = &instructions[after].after;
				if (!target) throw std::invalid_argument("Recorder snapshot is not retained");
			} else if (action == "backFrame") {
				const auto at = FrameLower();
				if (at != frames.begin()) target = &*std::prev(at);
			} else if (action == "forwardFrame") {
				const auto at = FrameUpper();
				if (at != frames.end()) target = &*at;
			} else if (action == "backInstruction" || action == "backInstruction100") {
				const auto at = InstructionBack();
				const size_t count = action == "backInstruction100" ? 100 : 1;
				if (at != 0) target = &instructions[at - std::min(at, count)].before;
			} else if ((action == "forwardInstruction" || action == "forwardInstruction100") && InInstructionRange()) {
				const auto at = InstructionForward();
				const size_t count = action == "forwardInstruction100" ? 100 : 1;
				if (at != instructions.size()) target = &instructions[at + std::min(instructions.size() - at, count) - 1].after;
			}
			if (!target) return false;
			while (cursor.journal > target->journal) {
				const auto& change = changes[--cursor.journal - journalBase];
				apply(change.address, change.before);
			}
			while (cursor.journal < target->journal) {
				const auto& change = changes[cursor.journal++ - journalBase];
				apply(change.address, change.after);
			}
			cursor = *target;
			++revision;
			return true;
		}

		uint64_t Revision() const { return revision; }
		bool IsInstructionPosition(uint64_t id) const { return !instructions.empty() && id >= instructions.front().before.id; }
		void Touch() { ++revision; }
		auto Status(bool attached, bool running, uint16_t pc, uint64_t frame) const -> nlohmann::json
		{
			using Json = nlohmann::json;
			Json result = {{"attached", attached}, {"running", running}, {"revision", revision},
				{"frameCheckpoints", attached ? frames.size() : 0}, {"frameStepsBack", 0}, {"frameStepsForward", 0},
				{"instructionStepsBack", 0}, {"instructionStepsForward", 0}, {"position", nullptr}, {"timeline", nullptr}};
			if (!attached) return result;
			const auto lo = FrameLower(), hi = FrameUpper();
			result["frameStepsBack"] = lo - frames.begin();
			result["frameStepsForward"] = frames.end() - hi;
			result["instructionStepsBack"] = InstructionBack();
			result["instructionStepsForward"] = InInstructionRange() ? instructions.size() - InstructionForward() : 0;
			result["position"] = {{"id", cursor.id}, {"pc", pc}, {"frameNumber", frame},
				{"atFrameCheckpoint", lo != hi}, {"atRecordedTip", !Browsing()}};
			uint64_t oldest = cursor.id;
			if (!frames.empty()) oldest = std::min(oldest, frames.front().id);
			if (!instructions.empty()) oldest = std::min(oldest, instructions.front().before.id);
			std::vector<uint64_t> stops;
			stops.reserve(instructions.size() + 1);
			for (size_t i = 0; i < instructions.size(); ++i) {
				const auto& entry = instructions[i];
				if (stops.empty() || stops.back() != entry.before.id) stops.push_back(entry.before.id);
				stops.push_back(entry.after.id);
			}
			result["timeline"] = {{"oldest", oldest}, {"newest", tip.id},
				{"instructionStart", instructions.empty() ? Json(nullptr) : Json(instructions.front().before.id)},
				{"frameTicks", Sample(frames.size(), [&](size_t i) { return frames[i].id; })},
				{"instructionTicks", Sample(stops.size(), [&](size_t i) { return stops[i]; })}};
			if (!running) {
				for (const auto& checkpoint : frames) stops.push_back(checkpoint.id);
				stops.push_back(cursor.id);
				stops.push_back(tip.id);
				std::sort(stops.begin(), stops.end());
				stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
				auto snapshots = Json::array();
				for (const auto id : stops) {
					if (id == tip.id) snapshots.push_back(Json::array({id, 0, "tip"}));
					else if (IsInstructionPosition(id)) {
						const auto executed = InstructionBoundary([&](const Instruction& entry) { return entry.after.id <= id; });
						snapshots.push_back(Json::array({id, instructions.size() - executed, "instruction"}));
					} else {
						const auto next = std::upper_bound(frames.begin(), frames.end(), id,
							[](uint64_t value, const Point& p) { return value < p.id; });
						const bool partialTip = frames.empty() || frames.back().id != tip.id;
						snapshots.push_back(Json::array({id, frames.end() - next + partialTip, "frame"}));
					}
				}
				result["timeline"]["snapshots"] = std::move(snapshots);
			}
			return result;
		}
	private:
		struct Instruction { Point before, after; };
		class InstructionRing {
			std::vector<Instruction> storage;
			size_t head = 0, count = 0;
		public:
			void clear() { head = count = 0; }
			bool empty() const { return count == 0; }
			size_t size() const { return count; }
			const Instruction& operator[](size_t i) const { return storage[(head + i) % MAX_INSTRUCTIONS]; }
			const Instruction& front() const { return (*this)[0]; }
			const Instruction& back() const { return (*this)[count - 1]; }
			void pop_back() { --count; }
			void push_back(const Instruction& value) {
				if (storage.empty()) storage.resize(MAX_INSTRUCTIONS);
				storage[(head + count) % MAX_INSTRUCTIONS] = value;
				if (count < MAX_INSTRUCTIONS) ++count;
				else head = (head + 1) % MAX_INSTRUCTIONS;
			}
		};
		std::deque<Point> frames;
		InstructionRing instructions;
		std::deque<Change> changes;
		Point cursor, tip;
		uint64_t journalBase = 0, sequence = 0, revision = 0;
		uint64_t JournalEnd() const { return journalBase + changes.size(); }
		bool InInstructionRange() const { return !instructions.empty() && cursor.id >= instructions.front().before.id; }
		auto FrameLower() const { return std::lower_bound(frames.begin(), frames.end(), cursor.id,
			[](const Point& point, uint64_t id) { return point.id < id; }); }
		auto FrameUpper() const { return std::upper_bound(frames.begin(), frames.end(), cursor.id,
			[](uint64_t id, const Point& point) { return id < point.id; }); }
		template<class Before>
		size_t InstructionBoundary(Before before) const {
			size_t low = 0, high = instructions.size();
			while (low < high) {
				const size_t middle = low + (high - low) / 2;
				if (before(instructions[middle])) low = middle + 1;
				else high = middle;
			}
			return low;
		}
		size_t InstructionBack() const { return InstructionBoundary([&](const Instruction& entry) { return entry.after.id <= cursor.id; }); }
		size_t InstructionForward() const { return InstructionBoundary([&](const Instruction& entry) { return entry.before.id < cursor.id; }); }
		template<class At>
		static auto Sample(size_t count, At at) -> std::vector<uint64_t>
		{
			std::vector<uint64_t> ticks;
			const size_t stride = count <= 40 ? 1 : 10;
			for (size_t i = 0; i < count; i += stride) ticks.push_back(at(i));
			if (count && ticks.back() != at(count - 1)) ticks.push_back(at(count - 1));
			return ticks;
		}
	};
}