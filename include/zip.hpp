#pragma once

#include <string>
#include <filesystem>
#include <utility>
#include <vector>

namespace CustomModels {
    std::string ReadFileFromZip(std::string const& path, std::string const& file);

    // Streams one entry of a zip to a file on disk without loading it into
    // memory. Writes to output + ".part" and renames on success.
    bool ExtractFileFromZip(std::string const& path, std::string const& file, std::string const& output);

    bool ExtractAllLegacyAssets(std::string const& path, std::string const& destination);

    // files: entry name -> contents. diskFiles: entry name -> path of a file
    // whose contents are streamed into the archive (it must exist until this
    // returns).
    bool WriteZipFile(
        std::string const& path,
        std::vector<std::pair<std::string, std::string>> const& files,
        std::vector<std::pair<std::string, std::string>> const& diskFiles = {}
    );
}
