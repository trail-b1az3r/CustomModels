#include "converter.hpp"

#include "beatsaber-hook/shared/utils/utils.h"
#include "main.hpp"
#include "vivify/VivifyBundleConvert.hpp"
#include "zip.hpp"

#include <cstring>
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
                            std::string const& configJson) {
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
           escape(androidFileName) + "\",\n"
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
std::string ConfigFor(PcModelType type) {
    switch (type) {
        case PcModelType::Saber:
            return R"({"hasTrail":false,"keepFakeGlow":false,"isLegacy":false,"fromPc":true})";
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
    for (int i = 1; std::filesystem::exists(folder / (candidate + extension), ec); i++)
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
    result.outputBytes = static_cast<uint64_t>(std::filesystem::file_size(destPath, ec));
    return result;
}

ConvertResult ConvertPcModelToQuest(std::string const& sourcePath, std::filesystem::path const& destFolder,
                                    PcModelType type) {
    ConvertResult result;
    if (type == PcModelType::Unknown) {
        result.message = "unrecognised PC model extension";
        return result;
    }

    try {
        std::error_code ec;
        std::filesystem::create_directories(destFolder, ec);

        std::string const convertedBundle = (destFolder / "_converted.bundle").string();
        result = ConvertBundleToAndroid(sourcePath, convertedBundle);
        if (!result.ok) {
            std::filesystem::remove(convertedBundle, ec);
            return result;
        }

        std::string const stem = UniqueStem(std::filesystem::path(sourcePath).stem().string(), destFolder,
                                            QuestExtensionFor(type));
        std::string const objectName = stem;
        std::string const androidFileName = "model.bundle";
        std::string const packageJson = MakePackageJson(objectName, "- PC model -", "Converted from PC bundle",
                                                        androidFileName, ConfigFor(type));

        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back(androidFileName, readfile(convertedBundle));
        files.emplace_back("package.json", packageJson);
        files.emplace_back("thumbnail.png", DefaultThumbnailPng());

        std::string const outputName = objectName + QuestExtensionFor(type);
        std::filesystem::path const outputPath = destFolder / outputName;
        std::error_code zipEc;
        std::filesystem::remove(outputPath, zipEc);
        WriteZipFile(outputPath.string(), files);

        std::filesystem::remove(convertedBundle, ec);
        if (!std::filesystem::exists(outputPath, ec)) {
            result.message = "converted bundle written but quest zip was not created";
            return result;
        }
        result.ok = true;
        result.outputBytes = static_cast<uint64_t>(std::filesystem::file_size(outputPath, ec));
        result.message = "converted to " + outputName;
    } catch (std::exception const& e) {
        result.ok = false;
        result.message = std::string("conversion failed: ") + e.what();
        std::error_code ec;
        std::filesystem::remove(destFolder / "_converted.bundle", ec);
    }
    return result;
}

BatchResult ConvertDirectoryModels(std::filesystem::path const& directory) {
    BatchResult result;
    try {
        if (!std::filesystem::exists(directory)) return result;
        std::set<std::string> const pcExtensions = {".saber", ".bloq", ".pixie"};
        for (auto const& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_directory()) continue;
            auto const& path = entry.path();
            auto const ext = path.extension().string();
            if (!pcExtensions.contains(ext)) continue;
            if (!IsPcModelBundle(path.string())) {
                result.errors.emplace_back(path.string() + ": not a UnityFS bundle");
                result.failed++;
                continue;
            }
            PcModelType const type = PcModelTypeFromExtension(ext);
            auto const out = ConvertPcModelToQuest(path.string(), directory, type);
            if (out.ok) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                result.converted++;
                logger.info("converted {} to {}", path.filename().string(), out.message);
            } else {
                result.failed++;
                result.errors.emplace_back(path.string() + ": " + out.message);
                logger.error("failed to convert {}: {}", path.string(), out.message);
            }
        }
    } catch (std::exception const& e) {
        result.errors.emplace_back(std::string("scan failed: ") + e.what());
    }
    return result;
}

}  // namespace CustomModels::Converter
