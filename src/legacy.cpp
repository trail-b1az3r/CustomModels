#include "legacy.hpp"

#include "UnityEngine/AssetBundle.hpp"
#include "UnityEngine/Graphics.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/RenderTexture.hpp"
#include "UnityEngine/TextAsset.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "json.hpp"
#include "main.hpp"
#include "metacore/shared/strings.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

static void ExtractZipFiles(std::filesystem::path destination) {
    std::filesystem::path folder = getDataDir(MOD_ID);
    folder /= "Zips";

    logger.info("extracting legacy zip files");

    std::vector<std::filesystem::path> remove;

    for (auto const& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.is_directory() || entry.path().extension() != ".zip") {
            logger.debug("skipping file {}", entry.path().string());
            continue;
        }
        if (CustomModels::ExtractAllLegacyAssets(entry.path(), destination))
            remove.emplace_back(entry.path());
    }

    for (auto& file : remove)
        std::filesystem::remove(file);
}

static bool LoadManifest(UnityEngine::AssetBundle* bundle, CustomModels::Manifest& manifest, std::string const& file) {
    manifest.androidFileName = "model.bundle";

    auto text = bundle->LoadAsset<UnityEngine::TextAsset*>("descriptor");
    if (!text) {
        logger.warn("legacy config not found");
        manifest.descriptor.author = "- Unknown -";
        manifest.descriptor.objectName = file;
        manifest.descriptor.description = "- Legacy file -";
        return true;
    }
    std::string json = text->text;
    CustomModels::LegacyDescriptor legacy;
    try {
        ReadFromString(json, legacy);
    } catch (std::exception const& e) {
        // a broken descriptor only costs the name, not the whole model
        logger.warn("failed to parse legacy descriptor: {}", e.what());
        logger.debug("descriptor was {}", json);
        legacy.objectName = file;
    }
    manifest.descriptor.author = legacy.authorName;
    manifest.descriptor.objectName = legacy.objectName;
    manifest.descriptor.description = legacy.description;
    return true;
}

// Thumbnails in legacy bundles are almost never marked readable, and
// EncodeToPNG throws on a non-readable texture. That exception used to abort
// the whole conversion, which is why most .qsaber files never converted. Copy
// the texture through the GPU into a small readable one instead, and if even
// that fails just go without a cover image.
static UnityEngine::Texture2D* ReadableCopy(UnityEngine::Texture2D* texture) {
    int width = texture->width;
    int height = texture->height;
    if (width <= 0 || height <= 0)
        return nullptr;
    int const maxSize = 256;
    if (std::max(width, height) > maxSize) {
        float scale = (float) maxSize / std::max(width, height);
        width = std::max(1, (int) (width * scale));
        height = std::max(1, (int) (height * scale));
    }
    auto target = UnityEngine::RenderTexture::GetTemporary(width, height, 0);
    if (!target)
        return nullptr;
    auto previous = UnityEngine::RenderTexture::get_active();
    UnityEngine::Graphics::Blit(texture, target);
    UnityEngine::RenderTexture::set_active(target);
    auto copy = UnityEngine::Texture2D::New_ctor(width, height, UnityEngine::TextureFormat::RGBA32, false);
    copy->ReadPixels(UnityEngine::Rect(0, 0, width, height), 0, 0);
    copy->Apply();
    UnityEngine::RenderTexture::set_active(previous);
    UnityEngine::RenderTexture::ReleaseTemporary(target);
    return copy;
}

static void
LoadThumbnail(UnityEngine::AssetBundle* bundle, CustomModels::Descriptor& descriptor, std::vector<std::pair<std::string, std::string>>& files) {
    try {
        auto thumbnail = bundle->LoadAsset<UnityEngine::Texture2D*>("thumbnail");
        if (!thumbnail)
            return;
        UnityEngine::Texture2D* source = thumbnail;
        UnityEngine::Texture2D* copy = nullptr;
        if (!thumbnail->isReadable) {
            copy = ReadableCopy(thumbnail);
            if (!copy)
                return;
            source = copy;
        }
        auto bytes = UnityEngine::ImageConversion::EncodeToPNG(source);
        if (copy)
            UnityEngine::Object::DestroyImmediate(copy);
        if (!bytes || bytes.size() == 0)
            return;
        descriptor.coverImage = "thumbnail.png";
        files.emplace_back(descriptor.coverImage, std::string(bytes.begin(), bytes.end()));
    } catch (std::exception const& e) {
        logger.warn("could not extract legacy thumbnail: {}", e.what());
    } catch (...) {
        logger.warn("could not extract legacy thumbnail");
    }
}

static bool WriteZip(
    UnityEngine::AssetBundle* bundle,
    CustomModels::Manifest& manifest,
    std::filesystem::path const& bundlePath,
    std::filesystem::path const& folder,
    std::filesystem::path const& file
) {
    std::vector<std::pair<std::string, std::string>> files;

    LoadThumbnail(bundle, manifest.descriptor, files);
    std::error_code ec;
    if (std::filesystem::file_size(bundlePath, ec) == 0 || ec) {
        logger.error("legacy AssetBundle {} is empty or unreadable", bundlePath.string());
        return false;
    }
    if (manifest.descriptor.objectName.empty())
        manifest.descriptor.objectName = bundlePath.stem().string();
    files.emplace_back("package.json", WriteToString(manifest));

    std::string name = MetaCore::Strings::UniqueFileName(file, folder);
    auto const output = folder / name;
    if (!CustomModels::WriteZipFile(output.string(), files, {{manifest.androidFileName, bundlePath.string()}})) {
        logger.error("failed to write converted legacy model {}", output.string());
        return false;
    }

    logger.info("converted {} to {}", bundlePath.filename().string(), name);
    return true;
}

template <class T>
static T TryLoadLegacyConfig(UnityEngine::AssetBundle* bundle, std::string const& type) {
    auto text = bundle->LoadAsset<UnityEngine::TextAsset*>("config");
    if (!text) {
        logger.warn("legacy {} config not found", type);
        return {};
    }
    std::string json = text->text;
    try {
        return ReadFromString<T>(json);
    } catch (std::exception const& e) {
        logger.error("failed to parse legacy {} config: {}", type, e.what());
        logger.debug("config was {}", json);
    }
    return {};
}

static CustomModels::SaberInfo LoadSaberConfig(UnityEngine::AssetBundle* bundle) {
    auto legacy = TryLoadLegacyConfig<CustomModels::LegacySaberConfig>(bundle, "saber");
    CustomModels::SaberInfo config;
    config.hasTrail = legacy.hasCustomTrails;
    config.keepFakeGlow = legacy.enableFakeGlow;
    config.isLegacy = true;
    return config;
}

template <class F>
static bool ConvertLegacy(std::filesystem::path const& path, char const* subfolder, char const* extension, F&& loadConfig) {
    std::filesystem::path folder = getDataDir(MOD_ID);
    folder /= subfolder;

    logger.info("converting {}", path.string());

    auto output = path.stem().string() + extension;

    auto bundle = UnityEngine::AssetBundle::LoadFromFile(path.string());
    if (!bundle) {
        logger.error("Unity could not open legacy bundle {}", path.string());
        return false;
    }

    bool written = false;
    try {
        CustomModels::Manifest manifest;
        if (LoadManifest(bundle, manifest, path.stem())) {
            manifest.config = loadConfig(bundle);
            written = WriteZip(bundle, manifest, path, folder, output);
        }
    } catch (std::exception const& e) {
        logger.error("failed to convert legacy model {}: {}", path.string(), e.what());
    } catch (...) {
        logger.error("failed to convert legacy model {}: unknown error", path.string());
    }
    // always release the bundle, even after a failure, or the next model
    // built from the same template cannot be opened
    bundle->Unload(true);
    return written;
}

static bool ConvertQSaber(std::filesystem::path const& path) {
    return ConvertLegacy(path, "Sabers", ".whacker", LoadSaberConfig);
}

static CustomModels::NoteInfo LoadNoteConfig(UnityEngine::AssetBundle* bundle) {
    auto legacy = TryLoadLegacyConfig<CustomModels::LegacyNoteConfig>(bundle, "note");
    CustomModels::NoteInfo config;
    config.hasDebris = legacy.hasDebris;
    config.hasBomb = legacy.hasBomb;
    config.showArrows = !legacy.disableBaseGameArrows;
    config.isLegacy = true;
    return config;
}

static bool ConvertQBloq(std::filesystem::path const& path) {
    return ConvertLegacy(path, "Notes", ".cyoob", LoadNoteConfig);
}

static CustomModels::WallInfo LoadWallConfig(UnityEngine::AssetBundle* bundle) {
    auto legacy = TryLoadLegacyConfig<CustomModels::LegacyWallConfig>(bundle, "wall");
    CustomModels::WallInfo config;
    config.replaceCoreMaterial = legacy.replaceCoreMaterial;
    config.replaceFrameMaterial = legacy.replaceFrameMaterial;
    config.replaceCoreMesh = legacy.replaceCoreMesh;
    config.replaceFrameMesh = legacy.replaceFrameMesh;
    config.disableFrame = legacy.disableFrame;
    config.disableFakeGlow = legacy.disableFakeGlow;
    config.isLegacy = true;
    return config;
}

static bool ConvertQWall(std::filesystem::path const& path) {
    return ConvertLegacy(path, "Walls", ".box", LoadWallConfig);
}

static std::string LowerExtension(std::filesystem::path const& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

static constexpr char LegacyGuard[] = ".converting-legacy";

void CustomModels::ConvertLegacyModels() {
    std::filesystem::path root = getDataDir(MOD_ID);
    std::filesystem::path folder = root / "Legacy";

    try {
        ExtractZipFiles(folder);
    } catch (std::exception const& e) {
        logger.error("failed to extract legacy zips: {}", e.what());
    }

    // A legacy bundle that crashes Unity while loading would otherwise crash
    // the game on every launch. Set aside whatever was being converted last.
    std::error_code ec;
    if (std::filesystem::exists(root / LegacyGuard, ec)) {
        std::string source;
        {
            std::ifstream in(root / LegacyGuard);
            std::getline(in, source);
        }
        std::filesystem::remove(root / LegacyGuard, ec);
        if (!source.empty() && std::filesystem::exists(source, ec)) {
            logger.error("legacy conversion of {} did not finish last time, skipping it", source);
            std::filesystem::rename(source, source + ".failed", ec);
        }
    }

    logger.info("converting legacy models");

    // Legacy files are routed to Legacy/ by the qmod, but people copy them
    // into the model folders by hand just as often, where they were ignored.
    std::vector<std::filesystem::path> candidates;
    for (auto const& subfolder : {"Legacy", "Sabers", "Notes", "Walls"}) {
        auto directory = root / subfolder;
        if (!std::filesystem::is_directory(directory, ec))
            continue;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeError;
            if (it->is_directory(typeError))
                continue;
            auto extension = LowerExtension(it->path());
            if (extension == ".qsaber" || extension == ".qbloq" || extension == ".qwall")
                candidates.emplace_back(it->path());
        }
        ec.clear();
    }

    for (auto const& path : candidates) {
        auto extension = LowerExtension(path);
        bool converted = false;
        {
            std::ofstream guard(root / LegacyGuard, std::ios::trunc);
            guard << path.string();
        }
        try {
            if (extension == ".qsaber")
                converted = ConvertQSaber(path);
            else if (extension == ".qbloq")
                converted = ConvertQBloq(path);
            else
                converted = ConvertQWall(path);
        } catch (std::exception const& exc) {
            logger.error("failed to convert legacy model {}: {}", path.string(), exc.what());
        } catch (...) {
            logger.error("failed to convert legacy model {}: unknown error", path.string());
        }
        std::filesystem::remove(root / LegacyGuard, ec);

        if (converted)
            std::filesystem::remove(path, ec);
        else {
            // don't pay for (or risk) the same failure on every launch
            std::filesystem::rename(path, path.string() + ".failed", ec);
        }
    }
}

static void CopyFolder(std::string const& source, std::string const& subfolder) {
    if (!direxists(source))
        return;

    std::filesystem::path folder = getDataDir(MOD_ID);
    auto dest = folder / subfolder;

    logger.info("moving models from {} to {}", source, dest.string());

    for (auto const& entry : std::filesystem::directory_iterator(source)) {
        if (entry.is_directory())
            continue;
        std::error_code ignore;
        std::filesystem::rename(entry, dest / entry.path().filename(), ignore);
    }
}

void CustomModels::MoveQosmeticsFolders() {
    std::filesystem::path qos = getDataDir("Qosmetics");
    CopyFolder(qos / "Whackers", "Sabers");
    CopyFolder(qos / "Cyoobs", "Notes");
    CopyFolder(qos / "Boxes", "Walls");
}
