#include "converter.hpp"

#include "beatsaber-hook/shared/utils/utils.h"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "main.hpp"
#include "vivify/VivifyBundleConvert.hpp"
#include "zip.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace CustomModels::Converter {
namespace {

// One-line summary of a conversion, built from the counters Vivify reports.
std::string Describe(Vivify::BundleConvert::ShaderConversion const& conversion) {
    std::string summary = conversion.message;
    if (summary.empty())
        summary = std::string(Vivify::BundleConvert::StatusText(conversion.status));
    if (conversion.shadersSeen > 0)
        summary += " [" + std::to_string(conversion.shadersTranslated) + " shaders, " +
                   std::to_string(conversion.texturesMarkedReadable) + " textures]";
    return summary;
}

// Write a minimal package.json descriptor for a converted Quest model.
std::string MakePackageJson(std::string const& objectName, std::string const& author,
                            std::string const& description, std::string const& androidFileName,
                            std::string const& configJson, std::string const& bundleSource = "") {
    // Escape backslashes and quotes for safe embedding.
    auto escape = [](std::string const& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '\\' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    return "{\n"
           "  \"androidFileName\": \"" +
           escape(androidFileName) + "\",\n" +
           (bundleSource.empty() ? "" : "  \"bundleSource\": \"" + escape(bundleSource) + "\",\n") +
           "  \"descriptor\": {\n"
           "    \"author\": \"" +
           escape(author) + "\",\n"
           "    \"objectName\": \"" +
           escape(objectName) + "\",\n"
           "    \"description\": \"" +
           escape(description) + "\",\n"
           "    \"coverImage\": \"thumbnail.png\"\n"
           "  },\n"
           "  \"config\": " +
           (configJson.empty() ? "{}" : configJson) + "\n"
           "}\n";
}

// The config CustomModels needs to load a converted PC model. Everything is
// detected at load time where it can be, so these only say what a converted
// model is guaranteed to be: a custom model (not the game's legacy Quest
// container format) with all of its own parts.
std::string TrailPackageConfig() {
    // The trail selector loads this package as a Saber asset so the existing
    // TrailInfo/TrailObject discovery path can retain all Unity dependencies.
    return R"({"hasTrail":true,"keepFakeGlow":false,"isLegacy":false,"fromPc":true})";
}

constexpr char kTrailPackageExtension[] = ".weeeeeeeeeeeeeeeeeeeee";

std::string ConfigFor(PcModelType type) {
    switch (type) {
        case PcModelType::Saber:
            // PC trails are found at load time (see saber.cpp), and a saber
            // whose trail cannot be found falls back to the default trail
            // rather than having none.
            return R"({"hasTrail":true,"keepFakeGlow":false,"isLegacy":false,"fromPc":true})";
        case PcModelType::Block:
            return R"({"hasDebris":false,"hasBomb":false,"hasSlider":false,"showArrows":false,"isMirrorable":true,"isLegacy":false,"fromPc":true})";
        default:
            return "{}";
    }
}

std::string DefaultThumbnailPng() {
    // A 1x1 transparent PNG, used when the source bundle has no extractable thumbnail.
    static uint8_t const pngBytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0xE2, 0x21, 0xCA, 0x40, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82,
    };
    return std::string(reinterpret_cast<char const*>(pngBytes), sizeof(pngBytes));
}

// Choose an output stem that does not collide with an existing Quest model.
// MetaCore::Strings::UniqueFileName considers every file in the directory,
// including the PC source that has not been removed yet, which would name the
// output "Model_1" even though there is no "Model.cyoob" in the way.
std::string UniqueStem(std::string const& stem, std::filesystem::path const& folder, std::string const& extension) {
    std::error_code ec;
    std::string candidate = stem;
    auto exists = [&](std::string const& value) {
        if (std::filesystem::exists(folder / (value + extension), ec))
            return true;
        return extension == ".whacker" &&
               std::filesystem::exists(folder / (value + kTrailPackageExtension), ec);
    };
    for (int i = 1; exists(candidate); i++)
        candidate = stem + "_" + std::to_string(i);
    return candidate;
}

}  // namespace

PcModelType PcModelTypeFromExtension(std::string const& ext) {
    if (ext == ".saber") return PcModelType::Saber;
    if (ext == ".bloq") return PcModelType::Block;
    if (ext == ".pixie") return PcModelType::Block;
    return PcModelType::Unknown;
}

std::string QuestExtensionFor(PcModelType type) {
    switch (type) {
        case PcModelType::Saber: return ".whacker";
        case PcModelType::Block: return ".cyoob";
        default: return "";
    }
}

bool IsPcModelBundle(std::string const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    char signature[8] = {};
    if (!in.read(signature, sizeof(signature))) return false;
    return std::memcmp(signature, "UnityFS\0", 8) == 0;
}

ConvertResult ConvertBundleToAndroid(std::string const& sourcePath, std::string const& destPath) {
    ConvertResult result;
    if (!IsPcModelBundle(sourcePath)) {
        result.message = "source is not a UnityFS AssetBundle";
        return result;
    }

    Vivify::BundleConvert::ShaderConversionOptions options;
    // Beat Saber on Quest draws everything through the two eye cameras, so the
    // multiview programs belong in the plain variants (the default). Leaving
    // the stereo split off is what makes converted models draw instead of
    // rendering nothing in the headset.
    options.separateStereoVariants = false;
    options.splitUnity2019 = false;
    options.singleViewOnly = false;

    auto conversion = Vivify::BundleConvert::ConvertShadersToGles(sourcePath, destPath, options);
    switch (conversion.status) {
        case Vivify::BundleConvert::Status::Success:
            result.ok = true;
            result.message = Describe(conversion);
            break;
        case Vivify::BundleConvert::Status::AlreadyAndroid: {
            // Copy it through: the caller wants a Quest bundle either way.
            std::ifstream in(sourcePath, std::ios::binary);
            std::ofstream out(destPath, std::ios::binary);
            if (!in.is_open() || !out.is_open()) {
                result.message = "bundle is already Android but could not be copied";
                return result;
            }
            out << in.rdbuf();
            out.close();
            if (!out) {
                result.message = "bundle is already Android but could not be copied";
                return result;
            }
            result.ok = true;
            result.message = "bundle already targets Android";
            break;
        }
        default:
            result.message = Describe(conversion);
            return result;
    }

    std::error_code ec;
    auto const size = std::filesystem::file_size(destPath, ec);
    if (ec || size == 0) {
        result.ok = false;
        result.message = "converter reported success but produced no readable output bundle";
        std::filesystem::remove(destPath, ec);
        return result;
    }
    result.outputBytes = static_cast<uint64_t>(size);
    return result;
}

ConvertResult ConvertPcModelToQuest(std::string const& sourcePath, std::filesystem::path const& destFolder,
                                    PcModelType type) {
    ConvertResult result;
    if (type == PcModelType::Unknown) {
        result.message = "unrecognised PC model extension";
        return result;
    }
    if (!IsPcModelBundle(sourcePath)) {
        result.message = "source is not a UnityFS AssetBundle";
        return result;
    }

    std::filesystem::path convertedBundle;
    std::filesystem::path temporaryQuest;
    std::filesystem::path temporaryTrail;
    struct TemporaryFiles {
        std::filesystem::path& bundle;
        std::filesystem::path& quest;
        std::filesystem::path& trail;
        ~TemporaryFiles() {
            std::error_code ec;
            if (!bundle.empty()) std::filesystem::remove(bundle, ec);
            if (!quest.empty()) std::filesystem::remove(quest, ec);
            if (!trail.empty()) std::filesystem::remove(trail, ec);
        }
    } cleanup{convertedBundle, temporaryQuest, temporaryTrail};
    try {
        std::error_code ec;
        std::filesystem::create_directories(destFolder, ec);
        if (ec) {
            result.message = "could not create destination directory: " + ec.message();
            return result;
        }

        // A fixed _converted.bundle name collides when multiple models are
        // converted in one batch (or two conversion requests overlap).
        auto const nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        convertedBundle = destFolder / (std::filesystem::path(sourcePath).stem().string() +
                                         ".converted-" + std::to_string(nonce) + ".bundle");
        result = ConvertBundleToAndroid(sourcePath, convertedBundle.string());
        if (!result.ok)
            return result;

        std::string const stem = UniqueStem(std::filesystem::path(sourcePath).stem().string(), destFolder,
                                            QuestExtensionFor(type));
        std::string const androidFileName = "model.bundle";
        std::string const packageJson = MakePackageJson(stem, "- PC model -", "Converted from PC bundle",
                                                        androidFileName, ConfigFor(type));

        // The converted bundle is streamed into the archive from disk rather
        // than read into memory: it is uncompressed and can be very large.
        std::error_code sizeError;
        if (std::filesystem::file_size(convertedBundle, sizeError) == 0 || sizeError) {
            result.ok = false;
            result.message = "converted UnityFS bundle is empty or unreadable";
            return result;
        }

        // A trail package gets its own lightweight archive and selection entry.
        // It references the paired .whacker bundle instead of duplicating it:
        // trail meshes, materials, and endpoint transforms remain valid, while
        // both selections share the same loaded Unity AssetBundle at runtime.
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("package.json", packageJson);
        files.emplace_back("thumbnail.png", DefaultThumbnailPng());
        std::vector<std::pair<std::string, std::string>> diskFiles;
        diskFiles.emplace_back(androidFileName, convertedBundle.string());

        std::string const outputName = stem + QuestExtensionFor(type);
        std::filesystem::path const outputPath = destFolder / outputName;
        temporaryQuest = outputPath.string() + ".tmp";
        std::filesystem::remove(temporaryQuest, ec);
        if (!WriteZipFile(temporaryQuest.string(), files, diskFiles)) {
            result.ok = false;
            result.message = "could not create a valid Quest model archive";
            return result;
        }

        // Rename only after the complete zip has closed successfully. The
        // original PC file is removed by the batch caller only after this.
        std::filesystem::rename(temporaryQuest, outputPath, ec);
        if (ec) {
            result.ok = false;
            result.message = "could not finalize Quest model archive: " + ec.message();
            return result;
        }

        auto outputSize = std::filesystem::file_size(outputPath, ec);
        if (ec || outputSize == 0) {
            result.ok = false;
            result.message = "Quest model archive is empty or unreadable";
            std::filesystem::remove(outputPath, ec);
            return result;
        }

        if (type == PcModelType::Saber) {
            std::string const trailName = stem + kTrailPackageExtension;
            std::filesystem::path const trailPath = destFolder / trailName;
            temporaryTrail = trailPath.string() + ".tmp";
            std::filesystem::remove(temporaryTrail, ec);

            std::vector<std::pair<std::string, std::string>> trailFiles;
            trailFiles.emplace_back("package.json",
                MakePackageJson(stem + " Trail", "- PC model -",
                                "Trail-only selector linked to its paired saber bundle",
                                "model.bundle", TrailPackageConfig(), outputName));
            trailFiles.emplace_back("thumbnail.png", DefaultThumbnailPng());

            if (!WriteZipFile(temporaryTrail.string(), trailFiles)) {
                std::filesystem::remove(outputPath, ec);
                result.ok = false;
                result.message = "could not create the standalone trail selection package";
                return result;
            }
            std::filesystem::rename(temporaryTrail, trailPath, ec);
            if (ec) {
                std::filesystem::remove(outputPath, ec);
                result.ok = false;
                result.message = "could not finalize trail package: " + ec.message();
                return result;
            }

            auto const trailSize = std::filesystem::file_size(trailPath, ec);
            if (ec || trailSize == 0) {
                std::filesystem::remove(outputPath, ec);
                std::filesystem::remove(trailPath, ec);
                result.ok = false;
                result.message = "trail package is empty or unreadable";
                return result;
            }
            outputSize += trailSize;
            result.message = "converted to " + outputName + " and " + trailName;
        } else {
            result.message = "converted to " + outputName;
        }

        result.ok = true;
        result.outputBytes = static_cast<uint64_t>(outputSize);
    } catch (std::exception const& e) {
        result.ok = false;
        result.message = std::string("conversion failed: ") + e.what();
    }

    return result;
}

namespace {

constexpr char kFailedSuffix[] = ".failed";
constexpr char kGuardName[] = ".converting";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::filesystem::path OutputFolderFor(PcModelType type, std::filesystem::path const& directory, std::filesystem::path const& modelRoot) {
    if (modelRoot.empty())
        return directory;
    // .bloq/.pixie can end up in Sabers or Legacy (manual copies, MBF, old
    // installs). Writing the .cyoob next to them hid it: only Notes is
    // indexed for notes, so the model "did not convert".
    return modelRoot / (type == PcModelType::Saber ? "Sabers" : "Notes");
}

// Renames a source that cannot be converted so it is not retried, and its
// cost paid again, on every launch. "Convert All" retries these explicitly.
void MarkFailed(std::filesystem::path const& path) {
    std::error_code ec;
    auto target = path.string() + kFailedSuffix;
    std::filesystem::remove(target, ec);
    std::filesystem::rename(path, target, ec);
    if (ec)
        logger.warn("could not mark {} as failed: {}", path.string(), ec.message());
}

void WriteGuard(std::filesystem::path const& modelRoot, std::filesystem::path const& source) {
    if (modelRoot.empty())
        return;
    std::ofstream out(modelRoot / kGuardName, std::ios::trunc);
    out << source.string();
}

void ClearGuard(std::filesystem::path const& modelRoot) {
    if (modelRoot.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(modelRoot / kGuardName, ec);
}

void RestoreFailed(std::filesystem::path const& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
        return;
    std::vector<std::filesystem::path> failed;
    for (std::filesystem::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == kFailedSuffix)
            failed.emplace_back(it->path());
    }
    for (auto const& path : failed) {
        auto original = path;
        original.replace_extension();
        std::error_code renameError;
        if (!std::filesystem::exists(original, renameError))
            std::filesystem::rename(path, original, renameError);
    }
}

}  // namespace

void RecoverInterruptedConversion(std::filesystem::path const& modelRoot) {
    auto guard = modelRoot / kGuardName;
    std::error_code ec;
    if (!std::filesystem::exists(guard, ec))
        return;
    std::string source;
    {
        std::ifstream in(guard);
        std::getline(in, source);
    }
    ClearGuard(modelRoot);
    if (source.empty() || !std::filesystem::exists(source, ec))
        return;
    // The game died while this file was being converted (most likely out of
    // memory). Set it aside so the next launch does not crash the same way.
    logger.error("conversion of {} did not finish last time, skipping it", source);
    MarkFailed(source);
}

BatchResult ConvertDirectoryModels(std::filesystem::path const& directory, std::filesystem::path const& modelRoot) {
    BatchResult result;
    try {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
            return result;

        // Snapshot candidates before writing outputs. This avoids walking newly
        // created Quest archives during the same recursive directory scan.
        std::vector<std::filesystem::path> candidates;
        for (std::filesystem::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeError;
            if (!it->is_regular_file(typeError))
                continue;
            auto ext = Lower(it->path().extension().string());
            if (ext == ".saber" || ext == ".bloq" || ext == ".pixie")
                candidates.emplace_back(it->path());
        }

        for (auto const& path : candidates) {
            auto ext = Lower(path.extension().string());
            if (!IsPcModelBundle(path.string())) {
                result.errors.emplace_back(path.string() + ": not a readable UnityFS bundle");
                result.failed++;
                MarkFailed(path);
                continue;
            }

            PcModelType const type = PcModelTypeFromExtension(ext);
            auto const destination = OutputFolderFor(type, directory, modelRoot);
            auto const begin = std::chrono::steady_clock::now();

            WriteGuard(modelRoot, path);
            ConvertResult out;
            try {
                out = ConvertPcModelToQuest(path.string(), destination, type);
            } catch (std::exception const& e) {
                out.ok = false;
                out.message = std::string("conversion threw: ") + e.what();
            } catch (...) {
                out.ok = false;
                out.message = "conversion threw an unknown error";
            }
            ClearGuard(modelRoot);

            auto const seconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() / 1000.0;
            if (out.ok) {
                std::error_code removeError;
                std::filesystem::remove(path, removeError);
                if (removeError) {
                    result.failed++;
                    result.errors.emplace_back(path.string() + ": converted, but source could not be removed: " + removeError.message());
                    logger.error("converted {}, but could not remove source: {}", path.string(), removeError.message());
                    continue;
                }
                result.converted++;
                logger.info("converted {} in {:.1f}s: {}", path.filename().string(), seconds, out.message);
            } else {
                result.failed++;
                result.errors.emplace_back(path.string() + ": " + out.message);
                logger.error("failed to convert {}: {}", path.string(), out.message);
                MarkFailed(path);
            }
        }
    } catch (std::exception const& e) {
        result.errors.emplace_back(std::string("scan/conversion failed: ") + e.what());
        result.failed++;
    }
    return result;
}

static std::atomic<bool> conversionRunning = false;

bool ConversionRunning() {
    return conversionRunning;
}

bool ConvertAllAsync(std::filesystem::path const& modelRoot, bool retryFailed, std::function<void(BatchResult)> onDone) {
    bool expected = false;
    if (!conversionRunning.compare_exchange_strong(expected, true))
        return false;

    std::thread([modelRoot, retryFailed, onDone = std::move(onDone)]() {
        BatchResult total;
        try {
            for (auto const& subfolder : {"Sabers", "Notes", "Walls", "Legacy"}) {
                if (retryFailed)
                    RestoreFailed(modelRoot / subfolder);
                auto result = ConvertDirectoryModels(modelRoot / subfolder, modelRoot);
                total.converted += result.converted;
                total.failed += result.failed;
                total.errors.insert(total.errors.end(), result.errors.begin(), result.errors.end());
            }
        } catch (...) {
            total.failed++;
            total.errors.emplace_back("background conversion failed unexpectedly");
        }
        BSML::MainThreadScheduler::Schedule([total = std::move(total), onDone]() {
            conversionRunning = false;
            if (onDone)
                onDone(total);
        });
    }).detach();
    return true;
}

}  // namespace CustomModels::Converter
