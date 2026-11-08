#pragma once

#include <string>
#include <filesystem>
#include <utility>
#include <vector>

namespace CustomModels {
    std::string ReadFileFromZip(std::string const& path, std::string const& file);

    bool ExtractAllLegacyAssets(std::string const& path, std::string const& destination);

    bool WriteZipFile(std::string const& path, std::vector<std::pair<std::string, std::string>> const& files);
}
