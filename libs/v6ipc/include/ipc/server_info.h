#pragma once

#include <nlohmann/json.hpp>

namespace dev::ipc
{
	auto IsRawFrameServerCompatible(const nlohmann::json& response) -> bool;
}