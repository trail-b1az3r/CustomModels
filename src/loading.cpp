#include "loading.hpp"

#include "UnityEngine/AssetBundleCreateRequest.hpp"
#include "UnityEngine/AssetBundleRequest.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Renderer.hpp"
#include "UnityEngine/Texture.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "main.hpp"
#include "metacore/shared/operators.hpp"
#include "utils.hpp"
#include "vivify/VivifyTextureDecode.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>

UnityEngine::GameObject* CustomModels::Asset::GetChild(std::string const& name) {
    if (!asset)
        return nullptr;
    if (auto child = asset->transform->Find(name))
        return child->gameObject;
    return nullptr;
}

UnityEngine::GameObject* CustomModels::Asset::InstantiateChild(std::string const& name) {
    if (auto child = GetChild(name))
        return CustomModels::Instantiate(child);
    return nullptr;
}

UnityEngine::GameObject* CustomModels::Asset::Instantiate() {
    if (!asset)
        return nullptr;
    return CustomModels::Instantiate(asset);
}

struct BundleReference {
    int refs = 0;
    UnityEngine::AssetBundle* bundle = nullptr;
    std::vector<std::function<void(UnityEngine::AssetBundle*)>> callbacks = {};
};

static std::map<std::string, BundleReference> bundles;

// removes a reference from an asset, handling any necessary cleanup, returns true if the file is no longer in bundles
static bool RemoveLoad(std::string const& file) {
    auto& bundle = bundles[file];
    if (--bundle.refs != 0)
        return false;
    logger.debug("unloading asset bundle {}", file);
    if (bundle.bundle)
        bundle.bundle->Unload(true);
    bundles.erase(file);
    return true;
}

static void LoadBundle(std::string const& bytes, std::string file) {
    static auto LoadFromMemoryAsync = il2cpp_utils::resolve_icall<UnityEngine::AssetBundleCreateRequest*, ArrayW<uint8_t>, uint>(
        "UnityEngine.AssetBundle::LoadFromMemoryAsync_Internal"
    );

    // keep a reference while loading in case it's cancelled partway through
    bundles[file].refs++;

    logger.debug("loading asset bundle {}", file);
    auto array = ArrayW<uint8_t>({(uint8_t*) bytes.data(), bytes.size()});
    auto request = LoadFromMemoryAsync(array, 0);

    BSML::MainThreadScheduler::ScheduleUntil(
        [request]() { return request->isDone; },
        [request = SafePtr(request), file]() {
            auto& bundle = bundles[file];
            auto loaded = request->assetBundle;
            bundle.bundle = loaded;
            decltype(bundle.callbacks) callbacks;
            callbacks.swap(bundle.callbacks);
            RemoveLoad(file);
            for (auto& callback : callbacks)
                callback(loaded);
        }
    );
}

// loads a file, reusing reference if already loading or loaded
static void AddLoad(std::string const& file, std::function<void(UnityEngine::AssetBundle*)> onDone) {
    bool present = bundles.contains(file);
    logger.debug("adding reference for asset {}, will load: {}", file, !present);

    auto& bundle = bundles[file];
    bundle.refs++;
    if (bundle.bundle)
        onDone(bundle.bundle);
    else
        bundle.callbacks.emplace_back(onDone);

    if (!present) {
        std::string bytes = CustomModels::ReadFileFromZip(file, CustomModels::files[file].androidFileName);
        if (!bytes.empty())
            LoadBundle(bytes, file);
        else
            onDone(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Prefab name resolution
// ---------------------------------------------------------------------------

static std::string AssetStem(std::string const& path) {
    auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.resize(dot);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return name;
}

// PC models keep the root prefab names they were authored with (_CustomSaber /
// _CustomNote / _CustomWall) while Quest containers use _Whacker / _Cyoob /
// _Box. Loading by one hardcoded name therefore silently fails for a converted
// PC model, which is the "saber falls back to default" / "note model does not
// show up" symptom. Try the expected name, then every known alias, then the
// bundle's only prefab.
static std::string ResolveAssetName(UnityEngine::AssetBundle* bundle, std::string const& preferred) {
    static std::vector<std::string> const aliases = {
        "_whacker", "_customsaber", "_cyoob", "_customnote", "_custombloq", "_box", "_customwall",
    };

    std::vector<std::string> candidates;
    std::vector<std::string> stems;
    for (auto const& name : bundle->GetAllAssetNames()) {
        std::string assetName = name;
        std::string lower = assetName;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (lower.size() < 7 || lower.compare(lower.size() - 7, 7, ".prefab") != 0)
            continue;
        std::string stem = AssetStem(assetName);
        if (stem.empty())
            continue;
        candidates.emplace_back(std::move(assetName));
        stems.emplace_back(std::move(stem));
    }

    if (candidates.empty())
        return preferred;

    auto find = [&](std::string const& wanted) -> std::string {
        auto it = std::find(stems.begin(), stems.end(), wanted);
        if (it == stems.end())
            return {};
        return candidates[std::distance(stems.begin(), it)];
    };

    if (auto match = find(AssetStem(preferred)); !match.empty())
        return match;
    for (auto const& alias : aliases) {
        if (auto match = find(alias); !match.empty())
            return match;
    }

    // No known name: if the bundle holds exactly one prefab it can only be that
    // one, whatever it is called.
    std::string onlyPrefab;
    int prefabCount = 0;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (stems[i].empty())
            continue;
        onlyPrefab = candidates[i];
        prefabCount++;
    }
    if (prefabCount == 1)
        return onlyPrefab;

    return preferred;
}

// ---------------------------------------------------------------------------
// Runtime texture decoding
// ---------------------------------------------------------------------------

// Decoding is deliberately size-capped: BC1 is half a byte per pixel while
// RGBA32 is four, so a texture-heavy model (T7 has 84 textures, twenty of them
// 2048x2048) would otherwise balloon to well over half a gigabyte the moment it
// is selected, which risks an out-of-memory crash on the headset. 512 is in the
// same range as Beat Saber's own textures, so the loss is not visible in-game.
constexpr int kMaxDecodedDimension = 512;

// Box-filter a decoded RGBA32 image down to `dw` x `dh`. Only the base level is
// read, so an unused mip chain on the source is harmless.
static std::vector<uint8_t> DownscaleRgba32(std::vector<uint8_t> const& src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (int dy = 0; dy < dh; dy++) {
        int const sy0 = dy * sh / dh;
        int const sy1 = std::max(sy0 + 1, (dy + 1) * sh / dh);
        for (int dx = 0; dx < dw; dx++) {
            int const sx0 = dx * sw / dw;
            int const sx1 = std::max(sx0 + 1, (dx + 1) * sw / dw);
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                size_t const row = static_cast<size_t>(sy) * sw * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    size_t const i = row + static_cast<size_t>(sx) * 4;
                    r += src[i];
                    g += src[i + 1];
                    b += src[i + 2];
                    a += src[i + 3];
                    n++;
                }
            }
            size_t const o = (static_cast<size_t>(dy) * dw + dx) * 4;
            dst[o] = (uint8_t) (r / n);
            dst[o + 1] = (uint8_t) (g / n);
            dst[o + 2] = (uint8_t) (b / n);
            dst[o + 3] = (uint8_t) (a / n);
        }
    }
    return dst;
}

// A PC bundle stores textures as BC/DXT, which an Adreno GPU cannot sample, so
// a converted model would draw untextured (or black). Conversion marks those
// textures readable, which keeps their CPU copy alive; this decodes them to
// plain RGBA32 once and swaps them into the materials that use them.
static UnityEngine::Texture2D* DecodeTexture(UnityEngine::Texture2D* texture) {
    if (!texture)
        return nullptr;

    auto format = Vivify::TextureDecode::FromUnityTextureFormat((int32_t) texture->format);
    if (format == Vivify::TextureDecode::Format::Unsupported)
        return nullptr;
    if (!texture->isReadable) {
        logger.warn("texture {} is block-compressed but not readable, cannot decode", texture->name);
        return nullptr;
    }

    int width = texture->width;
    int height = texture->height;
    if (width <= 0 || height <= 0)
        return nullptr;

    auto raw = texture->GetRawTextureData();
    if (!raw || raw.size() == 0)
        return nullptr;

    std::vector<uint8_t> decoded;
    if (!Vivify::TextureDecode::DecodeToRgba32(format, raw.begin(), raw.size(), width, height, texture->mipmapCount, decoded)) {
        logger.warn("failed to decode texture {} ({})", texture->name, Vivify::TextureDecode::FormatName(format));
        return nullptr;
    }
    if (Vivify::TextureDecode::IsBlankRgba32(decoded.data(), decoded.size())) {
        logger.warn("decoded texture {} came out blank, keeping the original", texture->name);
        return nullptr;
    }

    int mipCount = std::max(1, texture->mipmapCount);
    int decodedWidth = width;
    int decodedHeight = height;

    if (std::max(width, height) > kMaxDecodedDimension) {
        double scale = static_cast<double>(kMaxDecodedDimension) / std::max(width, height);
        decodedWidth = std::max(1, static_cast<int>(width * scale));
        decodedHeight = std::max(1, static_cast<int>(height * scale));
        decoded = DownscaleRgba32(decoded, width, height, decodedWidth, decodedHeight);
        // The mip chain was built for the original size, so let Unity generate
        // none and rely on filtering.
        mipCount = 1;
    }
    if (decoded.empty())
        return nullptr;

    auto replacement = UnityEngine::Texture2D::New_ctor(decodedWidth, decodedHeight, UnityEngine::TextureFormat::RGBA32, mipCount, false);
    if (!replacement)
        return nullptr;

    replacement->LoadRawTextureData(ArrayW<uint8_t>({(uint8_t*) decoded.data(), decoded.size()}));
    replacement->Apply(false, true);
    replacement->name = texture->name;
    replacement->filterMode = texture->filterMode;
    replacement->wrapMode = texture->wrapMode;
    replacement->anisoLevel = texture->anisoLevel;
    logger.debug("decoded texture {} {}x{} (from {}x{}) mips {} to RGBA32", texture->name, decodedWidth, decodedHeight,
                 width, height, mipCount);
    return replacement;
}

static void FixAssetTextures(UnityEngine::GameObject* asset) {
    if (!asset)
        return;

    auto start = std::chrono::steady_clock::now();
    std::map<UnityEngine::Texture2D*, UnityEngine::Texture2D*> cache;
    int replaced = 0;

    for (auto renderer : asset->GetComponentsInChildren<UnityEngine::Renderer*>(true)) {
        if (!renderer)
            continue;
        for (auto material : renderer->sharedMaterials) {
            if (!material)
                continue;
            for (auto id : material->GetTexturePropertyNameIDs()) {
                auto texture = material->GetTexture(id);
                if (!texture)
                    continue;
                auto texture2d = texture.try_cast<UnityEngine::Texture2D>();
                if (!texture2d)
                    continue;

                auto original = texture2d->ptr();
                UnityEngine::Texture2D* replacement = nullptr;
                auto cached = cache.find(original);
                if (cached != cache.end()) {
                    replacement = cached->second;
                } else {
                    replacement = DecodeTexture(original);
                    cache[original] = replacement;
                }
                if (replacement) {
                    material->SetTexture(id, replacement);
                    replaced++;
                }
            }
        }
    }

    if (replaced > 0 || !cache.empty()) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        logger.info("decoded {} block-compressed texture(s) ({} material reference(s)) in {} ms", cache.size(), replaced, ms);
    }
}

void CustomModels::Asset::Load(std::string file, std::string assetName, std::function<void(bool, bool)> onDone) {
    if (!files.contains(file) || file.empty() || file == currentFile) {
        onDone(false, !files.contains(file) || file.empty());
        return;
    }

    logger.info("getting asset {} for {}", assetName, file);
    Unload();
    loadingFile = file;

    // nesting callbacks is definitely annoying, but a coroutine would be too
    AddLoad(file, [this, file, assetName, onDone](UnityEngine::AssetBundle* bundle) {
        if (file != loadingFile) {
            onDone(false, false);
            return;
        }
        if (!bundle) {
            logger.error("failed to load asset bundle for {}", file);
            loadingFile = "";
            RemoveLoad(file);
            onDone(false, true);
            return;
        }

        logger.debug("loading asset {} from {}", assetName, file);
        std::string resolved = ResolveAssetName(bundle, assetName);
        if (resolved != assetName)
            logger.info("loading asset {} (requested {}) from {}", resolved, assetName, file);
        auto request = bundle->LoadAssetAsync<UnityEngine::GameObject*>(resolved);

        BSML::MainThreadScheduler::ScheduleUntil(
            [request]() { return request->isDone; },
            [this, bundle, request = SafePtr(request), assetName, resolved, file, onDone]() {
                if (file != loadingFile) {
                    onDone(false, false);
                    return;
                }

                logger.debug("finished loading asset {} for {}", resolved, file);
                currentFile = file;
                loadingFile = "";

                this->bundle = bundle;
                if (auto object = request->asset.try_cast<UnityEngine::GameObject>()) {
                    asset = object->ptr();
                    asset->hideFlags = asset->hideFlags | UnityEngine::HideFlags::DontUnloadUnusedAsset;
                    FixAssetTextures(asset);
                } else
                    logger.error("failed to load asset {} for {}", resolved, file);

                onDone(true, asset == nullptr);
            }
        );
    });
}

void CustomModels::Asset::Unload() {
    logger.info("removing reference to {} (and {})", currentFile, loadingFile);
    if (!loadingFile.empty())
        RemoveLoad(loadingFile);
    if (!currentFile.empty())
        RemoveLoad(currentFile);
    loadingFile = "";
    currentFile = "";
    bundle = nullptr;
    asset = nullptr;
}

std::map<std::string, CustomModels::Manifest> CustomModels::files;

static std::set<std::string> const extensions = {".whacker", ".cyoob", ".box"};

static void LoadManifest(std::string const& file) {
    logger.debug("loading manifest for {}", file);
    auto package = CustomModels::ReadFileFromZip(file, "package.json");
    if (package.empty()) {
        logger.error("failed to read package.json from {}", file);
        return;
    }

    CustomModels::Manifest manifest;
    try {
        ReadFromString(package, manifest);
    } catch (std::exception const& e) {
        logger.error("failed to parse package.json from {}: {}", file, e.what());
        return;
    }
    CustomModels::files[file] = manifest;
}

static void LoadDirectory(std::string const& directory) {
    logger.debug("loading manifests in {}", directory);

    for (auto const& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_directory())
            continue;
        auto path = entry.path();
        if (extensions.contains(path.extension()))
            LoadManifest(path);
        else
            logger.debug("skipping loading manifest for {} due to invalid extension {}", path.string(), path.extension().string());
    }
}

void CustomModels::LoadManifests() {
    logger.info("refreshing manifests");
    files.clear();

    std::filesystem::path folder = getDataDir(MOD_ID);
    for (auto& subfolder : {"Sabers", "Notes", "Walls"})
        LoadDirectory(folder / subfolder);
}
