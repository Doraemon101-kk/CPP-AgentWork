#pragma once
#include <nlohmann/json.hpp>
#include <functional>
using ModelDelta = std::function<void(const std::string&)>;
nlohmann::json model_chat(const nlohmann::json& request, unsigned short port, ModelDelta on_text = {});
