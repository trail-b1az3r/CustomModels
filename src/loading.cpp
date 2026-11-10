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
#include <atomic>
#include <map>
#include <set>
#include <thread>

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
    auto it = bundles.find(file);
    if (it == bundles.end()) {
        logger.warn("ignoring release for untracked asset bundle {}", file);
        return true;
    }
    if (it->second.refs > 0)
        --it->second.refs;
    if (it->second.refs > 0)
        return false;
    logger.debug("unloading asset bundle {}", file);
    if (it->second.bundle)
        it->second.bundle->Unload(true);
    bundles.erase(it);
    return true;
}

// Hands a finished (or failed) load to everyone waiting on it and drops the
// temporary in-flight reference taken when the load started.
static void FinishLoad(std::string const& file, UnityEngine::AssetBundle* loaded) {
    auto it = bundles.find(file);
    if (it == bundles.end()) {
        // cannot happen while the in-flight reference is held, but never leak
        if (loaded)
            loaded->Unload(true);
        return;
    }
    it->second.bundle = loaded;
    auto callbacks = std::move(it->second.callbacks);
    it->second.callbacks.clear();
    bool const gone = RemoveLoad(file);
    for (auto& callback : callbacks)
        callback(gone ? nullptr : loaded);
}

// ---------------------------------------------------------------------------
// Bundle cache
//
// Model packages are zips. Loading the bundle with LoadFromMemory means the
// whole bundle sits in RAM twice (the extracted bytes and Unity's copy) for as
// long as it is loaded. A converted PC bundle is stored uncompressed and can
// be several hundred megabytes, which is enough to get the game killed on a
// Quest. Instead each bundle is extracted once to a cache file, off the main
// thread, and loaded with LoadFromFileAsync, which streams from disk and only
// keeps what is actually in use resident. Later loads skip extraction entirely.
// ---------------------------------------------------------------------------

static std::filesystem::path CacheFolder() {
    return std::filesystem::path(getDataDir(MOD_ID)) / "Cache";
}

static std::filesystem::path CachePathFor(std::string const& file, std::string const& entry) {
    std::error_code ec;
    auto size = std::filesystem::file_size(file, ec);
    if (ec)
        size = 0;
    ec.clear();
    auto time = std::filesystem::last_write_time(file, ec);
    long long stamp = ec ? 0 : static_cast<long long>(time.time_since_epoch().count());
    size_t hash = std::hash<std::string>{}(file + "|" + entry + "|" + std::to_string(size) + "|" + std::to_string(stamp));
    return CacheFolder() / fmt::format("{:016x}.bundle", hash);
}

void CustomModels::CleanBundleCache() {
    std::set<std::string> keep;
    for (auto const& [file, manifest] : files)
        keep.emplace(CachePathFor(file, manifest.androidFileName).filename().string());

    std::error_code ec;
    auto folder = CacheFolder();
    if (!std::filesystem::is_directory(folder, ec)) {
        std::filesystem::create_directories(folder, ec);
        return;
    }
    int removed = 0;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (keep.contains(it->path().filename().string()))
            continue;
        std::error_code ignore;
        if (std::filesystem::remove(it->path(), ignore))
            removed++;
    }
    if (removed > 0)
        logger.info("removed {} stale cached bundle(s)", removed);
}

static void LoadBundleFromMemory(std::string const& file, std::string const& bytes) {
    static auto LoadFromMemoryAsync = il2cpp_utils::resolve_icall<UnityEngine::AssetBundleCreateRequest*, ArrayW<uint8_t>, uint>(
        "UnityEngine.AssetBundle::LoadFromMemoryAsync_Internal"
    );

    logger.debug("loading asset bundle {} from memory", file);
    auto array = ArrayW<uint8_t>({(uint8_t*) bytes.data(), bytes.size()});
    auto request = LoadFromMemoryAsync(array, 0);
    if (!request) {
        logger.error("Unity failed to create an asynchronous AssetBundle request for {}", file);
        FinishLoad(file, nullptr);
        return;
    }
    // keep the managed byte array alive until Unity is finished with it
    BSML::MainThreadScheduler::ScheduleUntil(
        [request]() { return request->isDone; },
        [request = SafePtr(request), array = SafePtr<Array<uint8_t>>(static_cast<Array<uint8_t>*>(array)), file]() {
            FinishLoad(file, request->assetBundle);
        }
    );
}

static void LoadBundleFromMemoryFallback(std::string const& file, std::string const& entry) {
    std::string bytes = CustomModels::ReadFileFromZip(file, entry);
    if (bytes.empty()) {
        logger.error("asset bundle entry '{}' is missing or empty in {}", entry, file);
        FinishLoad(file, nullptr);
        return;
    }
    LoadBundleFromMemory(file, bytes);
}

static void LoadBundleFromCache(std::string const& file, std::string const& entry, std::filesystem::path const& cache) {
    logger.debug("loading asset bundle {} from {}", file, cache.string());
    auto request = UnityEngine::AssetBundle::LoadFromFileAsync(cache.string());
    if (!request) {
        logger.warn("Unity could not open cached bundle for {}, falling back to memory", file);
        LoadBundleFromMemoryFallback(file, entry);
        return;
    }
    BSML::MainThreadScheduler::ScheduleUntil(
        [request]() { return request->isDone; },
        [request = SafePtr(request), file, entry, cache]() {
            auto loaded = request->assetBundle;
            if (!loaded) {
                // a damaged cache file must not poison every later load
                logger.warn("cached bundle for {} failed to load, retrying from the package", file);
                std::error_code ec;
                std::filesystem::remove(cache, ec);
                LoadBundleFromMemoryFallback(file, entry);
                return;
            }
            FinishLoad(file, loaded);
        }
    );
}

static void StartBundleLoad(std::string const& file) {
    // keep a reference while loading in case it's cancelled partway through
    bundles[file].refs++;

    auto manifest = CustomModels::files.find(file);
    if (manifest == CustomModels::files.end()) {
        logger.error("no manifest for asset bundle {}", file);
        FinishLoad(file, nullptr);
        return;
    }
    std::string const entry = manifest->second.androidFileName;
    auto const cache = CachePathFor(file, entry);
    std::error_code ec;
    if (std::filesystem::exists(cache, ec)) {
        LoadBundleFromCache(file, entry, cache);
        return;
    }

    std::filesystem::create_directories(cache.parent_path(), ec);
    logger.debug("extracting asset bundle {} to cache", file);
    std::thread([file, entry, cache]() {
        bool const ok = CustomModels::ExtractFileFromZip(file, entry, cache.string());
        BSML::MainThreadScheduler::Schedule([file, entry, cache, ok]() {
            if (ok)
                LoadBundleFromCache(file, entry, cache);
            else {
                logger.warn("could not cache bundle for {}, loading it from memory", file);
                LoadBundleFromMemoryFallback(file, entry);
            }
        });
    }).detach();
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
        bundle.callbacks.emplace_back(std::move(onDone));

    if (!present)
        StartBundleLoad(file);
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
//
// Only reading the source and creating the replacement touch Unity; the
// decode and downscale are pure CPU work, so they run on worker threads. A
// texture-heavy model used to stall the main thread for seconds on selection.
struct DecodeJob {
    UnityEngine::Texture2D* original = nullptr;
    Vivify::TextureDecode::Format format = Vivify::TextureDecode::Format::Unsupported;
    std::string name;
    int width = 0;
    int height = 0;
    int mipCount = 1;
    std::vector<uint8_t> raw;
    // results
    bool ok = false;
    int outWidth = 0;
    int outHeight = 0;
    int outMips = 1;
    std::vector<uint8_t> decoded;
};

static bool PrepareDecode(UnityEngine::Texture2D* texture, DecodeJob& job) {
    if (!texture)
        return false;
    auto format = Vivify::TextureDecode::FromUnityTextureFormat((int32_t) texture->format);
    if (format == Vivify::TextureDecode::Format::Unsupported)
        return false;
    if (!texture->isReadable) {
        logger.warn("texture {} is block-compressed but not readable, cannot decode", texture->name);
        return false;
    }
    int width = texture->width;
    int height = texture->height;
    if (width <= 0 || height <= 0)
        return false;
    auto raw = texture->GetRawTextureData();
    if (!raw || raw.size() == 0)
        return false;

    job.original = texture;
    job.format = format;
    job.name = static_cast<std::string>(texture->name);
    job.width = width;
    job.height = height;
    job.mipCount = std::max(1, (int) texture->mipmapCount);
    job.raw.assign(raw.begin(), raw.end());
    return true;
}

static void RunDecode(DecodeJob& job) {
    std::vector<uint8_t> decoded;
    if (!Vivify::TextureDecode::DecodeToRgba32(job.format, job.raw.data(), job.raw.size(), job.width, job.height, job.mipCount, decoded))
        return;
    job.raw.clear();
    job.raw.shrink_to_fit();
    if (decoded.empty() || Vivify::TextureDecode::IsBlankRgba32(decoded.data(), decoded.size()))
        return;

    job.outWidth = job.width;
    job.outHeight = job.height;
    job.outMips = job.mipCount;
    if (std::max(job.width, job.height) > kMaxDecodedDimension) {
        double scale = static_cast<double>(kMaxDecodedDimension) / std::max(job.width, job.height);
        job.outWidth = std::max(1, static_cast<int>(job.width * scale));
        job.outHeight = std::max(1, static_cast<int>(job.height * scale));
        decoded = DownscaleRgba32(decoded, job.width, job.height, job.outWidth, job.outHeight);
        // The mip chain was built for the original size, so let Unity generate
        // none and rely on filtering.
        job.outMips = 1;
    } else if (job.outMips > 1) {
        // DecodeToRgba32 returns the full chain; only keep it when it is
        // complete, otherwise LoadRawTextureData would reject the size.
        size_t expected = 0;
        int levels = 0;
        int w = job.width, h = job.height;
        for (int i = 0; i < job.outMips; i++) {
            expected += static_cast<size_t>(w) * h * 4;
            levels++;
            if (w == 1 && h == 1)
                break;
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        if (decoded.size() != expected || levels != job.outMips) {
            decoded.resize(static_cast<size_t>(job.width) * job.height * 4);
            job.outMips = 1;
        }
    }
    job.decoded = std::move(decoded);
    job.ok = !job.decoded.empty();
}

static UnityEngine::Texture2D* FinishDecode(DecodeJob& job) {
    if (!job.ok) {
        logger.warn("failed to decode texture {} ({})", job.name, Vivify::TextureDecode::FormatName(job.format));
        return nullptr;
    }
    auto replacement = UnityEngine::Texture2D::New_ctor(job.outWidth, job.outHeight, UnityEngine::TextureFormat::RGBA32, job.outMips, false);
    if (!replacement)
        return nullptr;
    replacement->LoadRawTextureData(ArrayW<uint8_t>({job.decoded.data(), job.decoded.size()}));
    // upload and release the CPU copy
    replacement->Apply(false, true);
    replacement->name = job.name;
    replacement->filterMode = job.original->filterMode;
    replacement->wrapMode = job.original->wrapMode;
    replacement->anisoLevel = job.original->anisoLevel;
    replacement->hideFlags = replacement->hideFlags | UnityEngine::HideFlags::DontUnloadUnusedAsset;
    logger.debug("decoded texture {} {}x{} (from {}x{}) mips {} to RGBA32", job.name, job.outWidth, job.outHeight, job.width, job.height, job.outMips);
    job.decoded.clear();
    job.decoded.shrink_to_fit();
    return replacement;
}

static void FixAssetTextures(UnityEngine::GameObject* asset) {
    if (!asset)
        return;

    auto start = std::chrono::steady_clock::now();

    struct Use {
        UnityEngine::Material* material;
        int id;
        UnityEngine::Texture2D* texture;
    };
    std::vector<Use> uses;
    std::map<UnityEngine::Texture2D*, size_t> jobIndex;
    std::vector<DecodeJob> jobs;
    std::set<UnityEngine::Material*> seenMaterials;

    for (auto renderer : asset->GetComponentsInChildren<UnityEngine::Renderer*>(true)) {
        if (!renderer)
            continue;
        for (auto material : renderer->sharedMaterials) {
            if (!material || !seenMaterials.emplace(material.ptr()).second)
                continue;
            for (auto id : material->GetTexturePropertyNameIDs()) {
                auto texture = material->GetTexture(id);
                if (!texture)
                    continue;
                auto texture2d = texture.try_cast<UnityEngine::Texture2D>();
                if (!texture2d)
                    continue;
                auto original = texture2d.value().ptr();
                if (!jobIndex.contains(original)) {
                    DecodeJob job;
                    if (PrepareDecode(original, job)) {
                        jobIndex[original] = jobs.size();
                        jobs.emplace_back(std::move(job));
                    } else
                        jobIndex[original] = SIZE_MAX;
                }
                if (jobIndex[original] != SIZE_MAX)
                    uses.push_back({material.ptr(), id, original});
            }
        }
    }

    if (jobs.empty())
        return;

    // decode on a few worker threads, the main thread takes a share too
    std::atomic<size_t> next = 0;
    auto worker = [&jobs, &next]() {
        for (size_t i = next++; i < jobs.size(); i = next++)
            RunDecode(jobs[i]);
    };
    unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u) - 1;
    threads = std::min<unsigned>(threads, jobs.size() > 0 ? jobs.size() - 1 : 0);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; i++)
        pool.emplace_back(worker);
    worker();
    for (auto& thread : pool)
        thread.join();

    std::map<UnityEngine::Texture2D*, UnityEngine::Texture2D*> replacements;
    for (auto& job : jobs)
        replacements[job.original] = FinishDecode(job);

    int replaced = 0;
    for (auto const& use : uses) {
        auto it = replacements.find(use.texture);
        if (it == replacements.end() || !it->second)
            continue;
        use.material->SetTexture(use.id, it->second);
        replaced++;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    logger.info("decoded {} block-compressed texture(s) ({} material reference(s)) in {} ms using {} thread(s)", jobs.size(), replaced, ms,
                threads + 1);
}

void CustomModels::Asset::Load(std::string file, std::string assetName, std::function<void(bool, bool)> onDone) {
    if (!files.contains(file) || file.empty() || file == currentFile) {
        onDone(false, !files.contains(file) || file.empty());
        return;
    }

    logger.info("getting asset {} for {}", assetName, file);
    Unload();

    // Trail sidecars point at their paired .whacker package. Resolve only a
    // plain sibling filename so a manifest cannot escape the model directory.
    std::string bundleFile = file;
    auto const& sourceName = files[file].bundleSource;
    if (!sourceName.empty()) {
        std::filesystem::path const sourcePath(sourceName);
        if (sourcePath.has_parent_path() || sourcePath.filename() != sourcePath ||
            sourceName == "." || sourceName == ".." || !sourceName.ends_with(".whacker")) {
            logger.error("invalid bundleSource '{}' in {}", sourceName, file);
            onDone(false, true);
            return;
        }
        bundleFile = (std::filesystem::path(file).parent_path() / sourcePath).string();
        if (!files.contains(bundleFile)) {
            logger.error("trail package {} references missing saber package {}", file, bundleFile);
            onDone(false, true);
            return;
        }
    }

    loadingFile = file;
    loadingBundleFile = bundleFile;
    // A generation number, not the file name, identifies this request:
    // unloading and quickly reselecting the same file used to let the stale
    // callback run as well, loading and post-processing the prefab twice.
    uint64_t const id = ++loadId;

    // nesting callbacks is definitely annoying, but a coroutine would be too
    AddLoad(bundleFile, [this, id, file, bundleFile, assetName, onDone](UnityEngine::AssetBundle* bundle) {
        if (id != loadId) {
            onDone(false, false);
            return;
        }
        if (!bundle) {
            logger.error("failed to load asset bundle for {}", file);
            loadingFile = "";
            RemoveLoad(bundleFile);
            loadingBundleFile = "";
            onDone(false, true);
            return;
        }

        logger.debug("loading asset {} from {}", assetName, file);
        std::string resolved = ResolveAssetName(bundle, assetName);
        if (resolved != assetName)
            logger.info("loading asset {} (requested {}) from {}", resolved, assetName, file);
        auto request = bundle->LoadAssetAsync<UnityEngine::GameObject*>(resolved);
        if (!request) {
            logger.error("Unity could not start loading asset {} for {}", resolved, file);
            loadingFile = "";
            RemoveLoad(bundleFile);
            loadingBundleFile = "";
            onDone(false, true);
            return;
        }

        BSML::MainThreadScheduler::ScheduleUntil(
            [request]() { return request->isDone; },
            [this, id, bundle, request = SafePtr(request), assetName, resolved, file, bundleFile, onDone]() {
                if (id != loadId) {
                    onDone(false, false);
                    return;
                }

                logger.debug("finished loading asset {} for {}", resolved, file);
                currentFile = file;
                currentBundleFile = bundleFile;
                loadingFile = "";
                loadingBundleFile = "";

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
    ++loadId;
    if (!loadingBundleFile.empty())
        RemoveLoad(loadingBundleFile);
    if (!currentBundleFile.empty())
        RemoveLoad(currentBundleFile);
    loadingFile = "";
    currentFile = "";
    loadingBundleFile = "";
    currentBundleFile = "";
    bundle = nullptr;
    asset = nullptr;
}

std::map<std::string, CustomModels::Manifest> CustomModels::files;

static std::set<std::string> const extensions = {".whacker", ".weeeeeeeeeeeeeeeeeeeee", ".cyoob", ".box"};

static void LoadManifest(std::string const& file, std::map<std::string, CustomModels::Manifest>& out) {
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
    out[file] = std::move(manifest);
}

static void LoadDirectory(std::filesystem::path const& directory, std::map<std::string, CustomModels::Manifest>& out) {
    logger.debug("loading manifests in {}", directory.string());
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || ec || !std::filesystem::is_directory(directory, ec) || ec) {
        logger.debug("model directory does not exist yet: {}", directory.string());
        return;
    }

    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        auto const& entry = *it;
        if (entry.is_directory(ec) || ec)
            continue;
        auto path = entry.path();
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extensions.contains(extension))
            LoadManifest(path.string(), out);
        else
            logger.debug("skipping loading manifest for {} due to invalid extension {}", path.string(), extension);
    }
    if (ec)
        logger.error("failed while scanning model directory {}: {}", directory.string(), ec.message());
}

void CustomModels::LoadManifests() {
    logger.info("refreshing manifests");
    auto start = std::chrono::steady_clock::now();

    // Build the new index aside and swap it in, so the map is never seen
    // half-filled by anything that reads it.
    std::map<std::string, Manifest> loaded;
    std::filesystem::path folder = getDataDir(MOD_ID);
    for (auto const& subfolder : {"Sabers", "Notes", "Walls"})
        LoadDirectory(folder / subfolder, loaded);
    files.swap(loaded);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    logger.info("indexed {} model(s) in {} ms", files.size(), ms);
}
