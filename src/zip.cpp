#include "zip.hpp"

#include "beatsaber-hook/shared/utils/utils.h"
#include "main.hpp"
#include "metacore/shared/strings.hpp"
#include "zip/shared/zip.h"

static std::string ReadFileInternal(zip* zip, int index) {
    struct zip_stat stat;
    std::string ret;

    if (zip_stat_index(zip, index, 0, &stat) != 0)
        return ret;
    auto file = zip_fopen_index(zip, index, 0);
    if (!file)
        return ret;
    ret.resize(stat.size);
    auto const read = zip_fread(file, ret.data(), stat.size);
    zip_fclose(file);
    if (read < 0 || static_cast<zip_uint64_t>(read) != stat.size)
        return {};

    return ret;
}

std::string CustomModels::ReadFileFromZip(std::string const& path, std::string const& file) {
    if (!fileexists(path) || std::filesystem::is_directory(path))
        return "";

    auto zip = zip_open(path.data(), ZIP_RDONLY, nullptr);
    if (!zip)
        return "";

    std::string ret = "";

    auto index = zip_name_locate(zip, file.data(), 0);
    if (index >= 0)
        ret = ReadFileInternal(zip, index);

    zip_discard(zip);
    return ret;
}

bool CustomModels::ExtractAllLegacyAssets(std::string const& path, std::string const& destination) {
    if (!fileexists(path) || std::filesystem::is_directory(path))
        return false;

    auto zip = zip_open(path.data(), ZIP_RDONLY, nullptr);
    if (!zip)
        return false;

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        zip_discard(zip);
        return false;
    }

    auto const files = zip_get_num_entries(zip, 0);
    int extracted = 0;
    for (zip_int64_t file = 0; file < files; file++) {
        auto const* rawName = zip_get_name(zip, file, ZIP_FL_ENC_GUESS);
        if (!rawName)
            continue;
        std::filesystem::path const entryPath(rawName);
        auto const extension = entryPath.extension().string();
        if (extension != ".qsaber" && extension != ".qbloq" && extension != ".qwall")
            continue;

        // Never trust paths stored inside an uploaded archive.
        auto const name = entryPath.filename().string();
        if (name.empty() || name == "." || name == "..")
            continue;
        auto const outputName = MetaCore::Strings::UniqueFileName(name, destination);
        auto const output = std::filesystem::path(destination) / outputName;
        auto const contents = ReadFileInternal(zip, static_cast<int>(file));
        if (contents.empty())
            continue;
        writefile(output.string(), contents);
        if (std::filesystem::exists(output, ec) && !ec)
            extracted++;
        ec.clear();
    }

    zip_discard(zip);
    return extracted > 0;
}

bool CustomModels::WriteZipFile(std::string const& path, std::vector<std::pair<std::string, std::string>> const& files) {
    auto archive = zip_open(path.data(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    if (!archive)
        return false;

    for (auto const& [name, data] : files) {
        auto source = zip_source_buffer_create(data.data(), data.size(), 0, nullptr);
        if (!source || zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_GUESS) < 0) {
            if (source)
                zip_source_free(source);
            zip_discard(archive);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return false;
        }
    }

    if (zip_close(archive) != 0) {
        zip_discard(archive);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}
