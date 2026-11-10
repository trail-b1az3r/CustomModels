#include "zip.hpp"

#include "beatsaber-hook/shared/utils/utils.h"
#include "main.hpp"
#include "metacore/shared/strings.hpp"
#include "zip/shared/zip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

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

bool CustomModels::ExtractFileFromZip(std::string const& path, std::string const& file, std::string const& output) {
    if (!fileexists(path) || std::filesystem::is_directory(path))
        return false;

    auto zip = zip_open(path.data(), ZIP_RDONLY, nullptr);
    if (!zip)
        return false;

    bool ok = false;
    auto index = zip_name_locate(zip, file.data(), 0);
    struct zip_stat stat;
    if (index >= 0 && zip_stat_index(zip, index, 0, &stat) == 0 && stat.size > 0) {
        if (auto source = zip_fopen_index(zip, index, 0)) {
            // Stream in fixed chunks: a converted PC bundle is stored
            // uncompressed and can be hundreds of megabytes, so it must never
            // be held in memory as a whole just to be written back out.
            std::string const temporary = output + ".part";
            if (FILE* out = fopen(temporary.c_str(), "wb")) {
                std::vector<char> buffer(1 << 20);
                zip_uint64_t total = 0;
                ok = true;
                while (true) {
                    auto read = zip_fread(source, buffer.data(), buffer.size());
                    if (read < 0) {
                        ok = false;
                        break;
                    }
                    if (read == 0)
                        break;
                    if (fwrite(buffer.data(), 1, read, out) != static_cast<size_t>(read)) {
                        ok = false;
                        break;
                    }
                    total += read;
                }
                ok = fclose(out) == 0 && ok && total == stat.size;
                std::error_code ec;
                if (ok) {
                    std::filesystem::rename(temporary, output, ec);
                    ok = !ec;
                }
                if (!ok)
                    std::filesystem::remove(temporary, ec);
            }
            zip_fclose(source);
        }
    }

    zip_discard(zip);
    return ok;
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
        auto extension = entryPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

bool CustomModels::WriteZipFile(
    std::string const& path,
    std::vector<std::pair<std::string, std::string>> const& files,
    std::vector<std::pair<std::string, std::string>> const& diskFiles
) {
    auto archive = zip_open(path.data(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    if (!archive)
        return false;

    auto fail = [&]() {
        zip_discard(archive);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    };

    for (auto const& [name, data] : files) {
        auto source = zip_source_buffer_create(data.data(), data.size(), 0, nullptr);
        if (!source)
            return fail();
        if (zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_GUESS) < 0) {
            zip_source_free(source);
            return fail();
        }
    }

    // Large entries (model bundles) are streamed from disk by libzip when the
    // archive is closed instead of being read into memory first, and use the
    // fastest deflate level: on a headset, conversion time is dominated by
    // compressing hundreds of megabytes, and level 1 is several times faster
    // than the default for a slightly larger file.
    for (auto const& [name, source_path] : diskFiles) {
        auto source = zip_source_file_create(source_path.c_str(), 0, -1, nullptr);
        if (!source)
            return fail();
        auto index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_GUESS);
        if (index < 0) {
            zip_source_free(source);
            return fail();
        }
        zip_set_file_compression(archive, index, ZIP_CM_DEFLATE, 1);
    }

    if (zip_close(archive) != 0)
        return fail();
    return true;
}
