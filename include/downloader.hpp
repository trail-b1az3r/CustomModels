#pragma once

#include "selection.hpp"

namespace CustomModels {
    void SearchBeatModsAssets(std::string query, int category, bool trails,
                              std::function<void(std::vector<ListItem*>, std::string)> callback);
    void ClearBeatModsAssets(std::vector<ListItem*>& items);
}
