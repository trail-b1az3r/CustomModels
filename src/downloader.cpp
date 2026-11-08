#include "downloader.hpp"

#include "UnityEngine/Networking/DownloadHandler.hpp"
#include "UnityEngine/Networking/DownloadHandlerBuffer.hpp"
#include "UnityEngine/Networking/UnityWebRequest.hpp"
#include "UnityEngine/Networking/UnityWebRequestAsyncOperation.hpp"
#include "System/Action_1.hpp"
#include "UnityEngine/AsyncOperation.hpp"
#include "assets.hpp"
#include "converter.hpp"
#include "main.hpp"
#include "custom-types/shared/delegate.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    using namespace CustomModels;

    using RequestCallback = System::Action_1<UnityEngine::AsyncOperation*>*;
#define BEATMODS_REQUEST_COMPLETED(...) \
    custom_types::MakeDelegate<RequestCallback>(classof(RequestCallback), static_cast<std::function<void(UnityEngine::AsyncOperation*)>>(__VA_ARGS__))

    constexpr char kApiBase[] = "https://beatmods.com/api/v3/assets";

    std::string EncodeQuery(std::string const& value) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string encoded;
        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                encoded.push_back(static_cast<char>(c));
            else {
                encoded.push_back('%');
                encoded.push_back(hex[c >> 4]);
                encoded.push_back(hex[c & 15]);
            }
        }
        return encoded;
    }

    std::string ReadString(rapidjson::Value const& value, char const* key) {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString())
            return {};
        return value[key].GetString();
    }

    int ReadInt(rapidjson::Value const& value, char const* key) {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsInt())
            return 0;
        return value[key].GetInt();
    }

    uint64_t ReadUInt64(rapidjson::Value const& value, char const* key) {
        if (!value.IsObject() || !value.HasMember(key) || !value[key].IsNumber())
            return 0;
        if (value[key].IsUint64())
            return value[key].GetUint64();
        if (value[key].IsInt64() && value[key].GetInt64() > 0)
            return static_cast<uint64_t>(value[key].GetInt64());
        return 0;
    }

    bool IsAllowedType(std::string const& type, int category) {
        if (category == 0)
            return type == "saber_whacker" || type == "saber_saber";
        if (category == 1)
            return type == "note_cyoob" || type == "note_bloq";
        return type == "wall_box" || type == "wall_pixie";
    }

    std::string ExtensionForType(std::string const& type) {
        if (type == "saber_whacker") return ".whacker";
        if (type == "saber_saber") return ".saber";
        if (type == "note_cyoob") return ".cyoob";
        if (type == "note_bloq") return ".bloq";
        if (type == "wall_box") return ".box";
        if (type == "wall_pixie") return ".pixie";
        return {};
    }

    bool HasTrailTag(rapidjson::Value const& asset) {
        if (!asset.IsObject() || !asset.HasMember("tags") || !asset["tags"].IsArray())
            return false;
        for (auto const& tag : asset["tags"].GetArray())
            if (tag.IsString() && std::string(tag.GetString()) == "CustomTrails")
                return true;
        return false;
    }

    std::string SafeStem(std::string name) {
        for (char& c : name) {
            unsigned char u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')'))
                c = '_';
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
            name.pop_back();
        if (name.empty() || name == "." || name == "..")
            name = "BeatModsAsset";
        return name;
    }

    struct BeatModsAssetItem final : ListItem {
        int id = 0;
        int category = 0;
        uint64_t fileSize = 0;
        std::string type;
        std::string name;
        std::string author;
        std::string downloadUrl;

        UnityEngine::Sprite* Cover() override {
            if (category == 0) return PNG_SPRITE(icons::saber);
            if (category == 1) return PNG_SPRITE(icons::note);
            return PNG_SPRITE(icons::wall);
        }
        std::string Name() override { return name; }
        std::string Author() override { return author; }
        bool Selected() override { return false; }
        bool Deletable() override { return false; }
        void Select(std::function<void()> onLoaded) override;
    };

    void DownloadAsset(BeatModsAssetItem* item, std::function<void()> onLoaded) {
        if (!item || item->downloadUrl.rfind("https://", 0) != 0) {
            logger.error("BeatMods returned an invalid download URL");
            return;
        }
        auto hostStart = std::string("https://").size();
        auto hostEnd = item->downloadUrl.find('/', hostStart);
        auto host = item->downloadUrl.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
        if (host != "beatmods.com" && (host.size() <= 13 || host.substr(host.size() - 13) != ".beatmods.com")) {
            logger.error("Refusing unexpected BeatMods download host: {}", host);
            return;
        }

        auto request = UnityEngine::Networking::UnityWebRequest::Get(item->downloadUrl);
        request->SendWebRequest()->add_completed(BEATMODS_REQUEST_COMPLETED([request, item, onLoaded](UnityEngine::AsyncOperation*) {
            if (request->get_responseCode() == 0 || request->get_responseCode() >= 400) {
                logger.error("BeatMods download failed for {}: {}", item->name, request->get_error());
                return;
            }

            auto data = request->get_downloadHandler()->GetData();
            if (data.size() == 0 || (item->fileSize && data.size() != item->fileSize)) {
                logger.error("BeatMods download size mismatch for {} (expected {}, received {})",
                             item->name, item->fileSize, data.size());
                return;
            }

            std::string bytes;
            bytes.resize(data.size());
            for (int i = 0; i < data.size(); ++i)
                bytes[i] = static_cast<char>(data[i]);

            auto ext = ExtensionForType(item->type);
            if (ext.empty())
                return;

            auto root = std::filesystem::path(getDataDir(MOD_ID));
            auto destination = root / "Legacy";
            if (ext == ".whacker" || ext == ".cyoob" || ext == ".box")
                destination = root / (item->category == 0 ? "Sabers" : item->category == 1 ? "Notes" : "Walls");

            std::error_code ec;
            std::filesystem::create_directories(destination, ec);
            if (ec) {
                logger.error("Could not create BeatMods download folder: {}", ec.message());
                return;
            }

            auto filename = std::to_string(item->id) + "_" + SafeStem(item->name) + ext;
            auto output = destination / filename;
            writefile(output.string(), bytes);
            if (!std::filesystem::exists(output, ec) || ec || std::filesystem::file_size(output, ec) != bytes.size() || ec) {
                logger.error("Could not verify downloaded BeatMods asset {}", item->name);
                std::filesystem::remove(output, ec);
                return;
            }

            if (ext == ".saber" || ext == ".bloq" || ext == ".pixie") {
                auto converted = Converter::ConvertDirectoryModels(root / "Legacy");
                logger.info("BeatMods asset {} downloaded; conversion results: {} converted, {} failed",
                            item->name, converted.converted, converted.failed);
            } else {
                logger.info("BeatMods asset {} downloaded to {}", item->name, output.string());
            }

            LoadManifests();
            if (onLoaded)
                onLoaded();
        }));
    }

    void BeatModsAssetItem::Select(std::function<void()> onLoaded) {
        DownloadAsset(this, std::move(onLoaded));
    }
}

void CustomModels::ClearBeatModsAssets(std::vector<ListItem*>& items) {
    for (auto* item : items)
        delete item;
    items.clear();
}

void CustomModels::SearchBeatModsAssets(std::string query, int category, bool trails,
                                        std::function<void(std::vector<ListItem*>, std::string)> callback) {
    std::string url = std::string(kApiBase) + "?status=public&page=1&limit=100";
    if (!query.empty())
        url += "&name=" + EncodeQuery(query);

    auto request = UnityEngine::Networking::UnityWebRequest::Get(url);
    request->SendWebRequest()->add_completed(BEATMODS_REQUEST_COMPLETED([request, category, trails, callback](UnityEngine::AsyncOperation*) {
        if (request->isNetworkError() || request->isHttpError()) {
            callback({}, "BeatMods API request failed: " + std::string(request->get_error()));
            return;
        }

        std::string body = request->get_downloadHandler()->GetText();
        rapidjson::Document document;
        document.Parse(body.c_str());
        if (document.HasParseError() || !document.IsObject() || !document.HasMember("assets") || !document["assets"].IsArray()) {
            callback({}, "BeatMods returned an invalid asset response");
            return;
        }

        std::vector<ListItem*> results;
        for (auto const& asset : document["assets"].GetArray()) {
            auto type = ReadString(asset, "type");
            if (!IsAllowedType(type, category) || (trails && !HasTrailTag(asset)))
                continue;
            auto name = ReadString(asset, "name");
            auto downloadUrl = ReadString(asset, "downloadUrl");
            if (name.empty() || downloadUrl.empty())
                continue;

            auto item = new BeatModsAssetItem();
            item->id = ReadInt(asset, "id");
            item->category = category;
            item->fileSize = ReadUInt64(asset, "fileSize");
            item->type = std::move(type);
            item->name = std::move(name);
            item->downloadUrl = std::move(downloadUrl);
            if (asset.HasMember("uploader") && asset["uploader"].IsObject()) {
                item->author = ReadString(asset["uploader"], "displayName");
                if (item->author.empty())
                    item->author = ReadString(asset["uploader"], "username");
            }
            results.emplace_back(item);
        }
        callback(std::move(results), {});
    }));
}
