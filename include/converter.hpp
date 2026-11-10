#pragma once

// Unity asset bundle (PC -> Quest) converter.
//
// Beat Saber on Quest runs Android/ARM64 Unity. A bundle built for
// StandaloneWindows/StandaloneWindows64 cannot be loaded by that runtime:
// AssetBundle::LoadFromFile either returns null or enumerates no assets -- the
// "experimental Windows bundle doesn't have any assets" symptom. The one field
// that makes a bundle platform-specific at load time is m_TargetPlatform inside
// every SerializedFile, so conversion retargets it to Android and repacks the
// archive uncompressed. Block-compressed textures (DXT/BC) also have their
// m_IsReadable flag set so their CPU copy survives load.
//
// Conversion is delegated to the vendored Vivify bundle converter
// (src/vivify), which retargets every SerializedFile, cross-compiles the
// DirectX shader bytecode to GLSL ES, and marks block-compressed textures
// readable so the mod can decode them to RGBA32 at load time (see
// loading.cpp). Meshes, prefabs and GameObject hierarchies come through intact.
//
// This module is deliberately free of Unity/il2cpp dependencies so it can be
// reviewed and unit-tested on a host compiler.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace CustomModels::Converter {

// Reserved for a future Unity 6 trail package format. Intentionally not
// recognized by loaders, scanners, selectors, or converters yet.
inline constexpr char ReservedUnity6TrailExtension[] = ".qweeeeeeeeeeeeeeeeeeeee";


// Kind of PC model, used to pick the Quest output container and load category.
enum class PcModelType {
    Saber,   // .saber  -> .whacker  (Sabers/)
    Block,   // .bloq/.pixie -> .cyoob (Notes/)
    Unknown, // not a recognised PC model extension
};

PcModelType PcModelTypeFromExtension(std::string const& ext);
// Quest container extension for a PC model type (.whacker / .cyoob).
std::string QuestExtensionFor(PcModelType type);

// True when the file at path begins with the UnityFS archive signature, i.e. it
// is a PC standalone asset bundle rather than a Quest zip container.
bool IsPcModelBundle(std::string const& path);

// Outcome of a conversion.
struct ConvertResult {
    bool ok = false;
    std::string message;
    uint64_t outputBytes = 0;
};

// Convert one PC UnityFS bundle into an Android-targeted, uncompressed UnityFS
// bundle written to destPath. Returns AlreadyAndroid-like success when nothing
// needed changing. destPath is only created on success.
ConvertResult ConvertBundleToAndroid(std::string const& sourcePath, std::string const& destPath);

// Convert one PC model file into a Quest-format zip (.whacker/.cyoob/.box)
// placed next to the source (same stem, Quest extension). The zip contains the
// converted model.bundle, a package.json descriptor and a thumbnail.png.
// Returns the path to the written zip in message when ok.
ConvertResult ConvertPcModelToQuest(std::string const& sourcePath,
                                    std::filesystem::path const& destFolder,
                                    PcModelType type);

// Convert every PC model bundle found under directory (recursively) into Quest
// format, removing the source PC files as they are converted. Reports counts.
struct BatchResult {
    int converted = 0;
    int failed = 0;
    std::vector<std::string> errors;
};
// When modelRoot is given (the mod's data folder), outputs go to the folder
// for their type (Sabers/ or Notes/) instead of next to the source, sources
// that fail are renamed to *.failed so they are not retried on every launch,
// and a guard file records the conversion in progress (see
// RecoverInterruptedConversion).
BatchResult ConvertDirectoryModels(std::filesystem::path const& directory, std::filesystem::path const& modelRoot = {});

// If the game died during a conversion, mark that source failed so the crash
// does not repeat on every launch. Call once at startup before converting.
void RecoverInterruptedConversion(std::filesystem::path const& modelRoot);

// Converts every PC model under modelRoot's Sabers/Notes/Walls/Legacy on a
// background thread, then calls onDone on the main thread. retryFailed first
// restores *.failed sources. Returns false if a conversion is already running.
bool ConvertAllAsync(std::filesystem::path const& modelRoot, bool retryFailed, std::function<void(BatchResult)> onDone);
bool ConversionRunning();

}  // namespace CustomModels::Converter
