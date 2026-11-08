#pragma once

// On-device Unity AssetBundle platform converter.
//
// Vivify maps ship their assets as a Unity AssetBundle. A bundle built for
// Windows/PCVR cannot be loaded by the Android/ARM64 Unity runtime on Quest:
// UnityEngine.AssetBundle.LoadFromFile either returns null or returns a bundle
// whose GetAllAssetNames() is empty, which is the "the experimental Windows
// bundle doesn't have any assets" symptom.
//
// The part of the bundle that actually makes it platform-specific at *load*
// time is a single int32 -- m_TargetPlatform -- in the header of each
// SerializedFile inside the archive. This converter unpacks the archive,
// rewrites that field to Android, and repacks it uncompressed under a new
// name, so Unity will accept and enumerate it.
//
// WHAT THIS CAN AND CANNOT FIX
//   Meshes, prefabs, GameObject hierarchies, animations, animator controllers,
//   audio, text assets and material *definitions* are stored in a
//   platform-independent form and come through intact.
//   Shaders need translating, and ConvertShadersToGles does it: a Windows
//   bundle carries DirectX bytecode, which is cross-compiled to GLSL ES (see
//   VivifyDxbc) and written back into the bundle. A shader using anything
//   outside the translated subset is left as it was and still falls to
//   Vivify's stand-in path (see Runtime::RepairMaterialShader) at load time.
//   Block-compressed textures are not recompressed -- BC/DXT data is not
//   something an Adreno GPU can consume, and re-encoding it to ETC2 or ASTC on
//   a headset is not realistic. Instead each such texture has its m_IsReadable
//   flag set, which is what keeps its CPU copy alive after load so the mod can
//   decode it to RGBA32 at level load (see Runtime::ResolveUsableTexture).
//   Without the flag Unity drops that copy and the decoder is handed an empty
//   buffer, which decodes to solid black.
//
// The implementation is deliberately free of Unity/il2cpp dependencies so it
// can be unit-tested on a host compiler.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Vivify::BundleConvert {

// Unity BuildTarget ids that show up in SerializedFile headers.
inline constexpr int32_t kBuildTargetStandaloneOSX = 2;
inline constexpr int32_t kBuildTargetStandaloneWindows = 5;
inline constexpr int32_t kBuildTargetiOS = 9;
inline constexpr int32_t kBuildTargetAndroid = 13;
inline constexpr int32_t kBuildTargetStandaloneWindows64 = 19;
inline constexpr int32_t kBuildTargetWebGL = 20;
inline constexpr int32_t kBuildTargetStandaloneLinux64 = 24;

enum class Status {
  Success,
  // Every SerializedFile in the archive already targets Android; nothing to do.
  AlreadyAndroid,
  SourceUnreadable,
  // Not a UnityFS archive (wrong signature, or a legacy UnityWeb/UnityRaw one).
  NotAUnityBundle,
  // LZMA/LZHAM or an unknown compression id we have no decoder for.
  UnsupportedCompression,
  // Well-formed signature but the structure did not parse.
  Corrupt,
  DestUnwritable,
  OutOfMemory,
};

struct Result {
  Status status = Status::Corrupt;
  // Human-readable detail, safe to log or show in a UI.
  std::string message;
  // Name of the platform the source bundle was built for, when known.
  std::string sourcePlatform;
  int serializedFilesSeen = 0;
  int serializedFilesRetargeted = 0;
  uint64_t outputBytes = 0;

  bool ok() const { return status == Status::Success; }
};

// Reads the UnityFS bundle at sourcePath, retargets every SerializedFile in it
// to Android, and writes an uncompressed UnityFS bundle to destPath.
//
// destPath is only created when the conversion succeeds; on any failure the
// (partial) destination is removed so a later run never sees a truncated file.
// Converting a bundle that is already Android-targeted returns
// Status::AlreadyAndroid and writes nothing.
Result ConvertToAndroid(std::string const& sourcePath, std::string const& destPath);

// Unpacks a bundle, rebuilds every SerializedFile inside it through
// SerializedFileParse::RewriteSerializedFile, and repacks the result.
//
// This is conversion step 4 with no shader edits in it, and it exists to be
// run: the whole point of the rewrite path is that a resized object must leave
// a bundle that still loads, and the only way to have any confidence in that
// before there is a shader to resize is to put real bundles through the exact
// same code and require the result to come back identical.
//
// A bundle whose files all rebuild unchanged produces output equivalent to what
// ConvertToAndroid's writer would have produced from the untouched data.
Result RepackBundle(std::string const& sourcePath, std::string const& destPath);

// True if the file at path begins with the UnityFS archive signature.
//
// Reads 8 bytes; it does not validate the rest of the archive. Vivify maps do
// not agree on a file name for their bundles -- PC maps commonly ship
// "bundleWindows2019"/"bundleWindows2021" with no extension at all -- so
// content is the only dependable way to find one in a song folder.
bool IsUnityBundleFile(std::string const& path);

// What the shaders inside a bundle were actually compiled for.
//
// Reads the bundle's Shader assets and reports the union of the GPU platforms
// their programs target. This exists because "a PC bundle's shaders are DirectX
// bytecode" had been asserted throughout this port without ever being measured
// against a real bundle -- and because it is the first thing any actual shader
// conversion needs: you cannot rewrite programs you cannot locate.
struct ShaderScan {
  bool parsed = false;
  bool typeTreeStripped = false;   // built with DisableWriteTypeTree: contents undecodable
  int serializedFiles = 0;
  int shaderObjects = 0;
  int shadersRunnableOnQuest = 0;  // shaders carrying a GLES3 or Vulkan program
  std::vector<int32_t> platforms;  // union across every shader, ascending
  std::vector<std::string> shaderNames;
  std::string unityVersion;
  std::string message;

  // What the compiled programs inside those shaders turned out to be, once the
  // blob is decompressed and split.
  //
  // The distinction that matters for conversion is glslSourcePrograms: a
  // program of that kind is GLSL *text*, so producing one is writing a string
  // rather than emitting a binary. A bundle that is entirely
  // binaryPrograms is the case that needs a real cross-compiler.
  int programs = 0;
  int glslSourcePrograms = 0;
  int binaryPrograms = 0;
  int undecodableShaders = 0;
  std::vector<int32_t> programTypes;  // union across every program, ascending
  // Shaders whose program store holds no program at all: Unity failed to
  // compile them for the bundle's platform and shipped them empty. Nothing can
  // draw them from this bundle; the PC build of the same map can stand in.
  std::vector<std::string> emptyShaderNames;
  // Whether any program names _CameraDepthTexture (GLSL text, a DXBC resource
  // table, or a parameter blob): only then does the map need the depth
  // pre-pass that renders it.
  bool samplesCameraDepth = false;
  std::vector<std::string> cameraDepthShaderNames;
};

// What a shader-translating conversion did.
struct ShaderConversion {
  Status status = Status::Corrupt;
  std::string message;
  int shadersSeen = 0;
  int shadersTranslated = 0;
  int shadersLeftAlone = 0;   // already runnable here, or nothing to translate
  int shadersRefused = 0;     // used something outside the translated subset
  int programsTranslated = 0;
  // Shaders converted through m_ParsedForm (the path that makes Unity select
  // the translated programs), keyword variants linked into GLES programs, and
  // variants left on DirectX because one of their stages did not translate.
  int shadersLinked = 0;
  int variantsLinked = 0;
  int variantsRefused = 0;
  // Keyword variants pointed at their single-pass instanced twin, which carries
  // the per-eye stereo maths that multiview needs.
  int stereoVariantsRemapped = 0;
  int stereoVariantsSplit = 0;  // separateStereoVariants: variants renamed to STEREO_MULTIVIEW_ON
  int geometryStagesDropped = 0;  // variants linked without the geometry stage they could not use
  int variantsSkipped = 0;  // program refs of variants a Quest never selects, left untranslated
  // Block-compressed textures seen, and how many had their m_IsReadable flag
  // set so the mod can decode them on device. A texture that is already
  // readable, or in a format a Quest can sample, is not counted as marked.
  int texturesSeen = 0;
  int texturesMarkedReadable = 0;
  int texturesStreamed = 0;
  uint64_t outputBytes = 0;
  // The first few reasons a shader was refused, for the log. Bounded on
  // purpose: a bundle with two hundred unsupported shaders would otherwise put
  // two hundred lines in a session log, all saying much the same thing.
  std::vector<std::string> refusals;
  // Why individual keyword variants stayed on DirectX in shaders that did
  // convert (first few distinct reasons).
  std::vector<std::string> variantRefusals;

  bool ok() const { return status == Status::Success; }
};

// Reads a PC bundle, translates every DirectX shader program inside it to
// GLSL ES, retargets the archive to Android, and writes the result to destPath.
//
// This is the whole conversion, end to end: locate the Shader assets, decode
// their program store, cross-compile the bytecode (VivifyDxbc), re-encode the
// store, rebuild each Shader object around it, and relay the archive around the
// objects that changed size.
//
// A shader carrying anything outside the translated subset is left exactly as
// it was rather than half-converted -- it keeps its DirectX programs, does not
// run here, and falls to the stand-in path at load time, which is what it would
// have done anyway. Success therefore does not mean every shader converted; it
// means the bundle was rewritten and is loadable, and the counters say how much
// of it will actually render.
struct ShaderConversionOptions {
  // Give each stereo-aware shader separate single-view and multiview programs,
  // the way Unity builds its own Quest shaders. The plain variants get a
  // single-view translation of the plain program -- what blits, render
  // textures and CustomRenderTextures draw with -- and the stereo variants get
  // the multiview translation of the single-pass-stereo program, with their
  // keyword renamed to STEREO_MULTIVIEW_ON, which Unity turns on for two-eye
  // rendering. Off, the multiview program goes into the plain variants (the
  // pre-0.14 behaviour): right for the eye cameras, but GL refuses a multiview
  // program in any single-view framebuffer, which is what made offscreen draws
  // on converted maps flicker or show garbage. Only turn this on when the game
  // actually uses STEREO_MULTIVIEW_ON, or the eye cameras draw nothing.
  // A 2021 bundle's keyword is renamed in m_ParsedForm.m_KeywordNames.
  bool separateStereoVariants = false;
  // Also split Unity 2019 bundles, renaming STEREO_INSTANCING_ON in each
  // pass's m_NameIndices, which is what a 2019 bundle picks variants by.
  // (0.14.0-0.14.4 renamed it in the program entries instead, which Unity
  // does not read, and the eye cameras drew nothing.) Without it a 2019
  // bundle's plain variants hold multiview programs: right for the headset,
  // invisible to single-screen cameras such as replay renderers.
  bool splitUnity2019 = false;
  // Replay render mode: every program translated single-view, plain variants
  // keeping their own code. For replay and recording renderers, which draw
  // through one single-screen camera; the headset's two-eye pass draws none
  // of it. Overrides the options above.
  //
  // (The splits above rename the stereo keyword to STEREO_MULTIVIEW_ON so the
  // headset picks the multiview variant. On the headset it never did, in
  // either the 2021 or the 2019 layout: 0.14.0-0.14.4 and 0.14.8 made
  // converted maps invisible, and 0.14.8's merged Hold My Hand kaleidoscope
  // drew nothing. The mod no longer turns them on.)
  bool singleViewOnly = false;
  // Give the output's internal files ("CAB-...") new names, so it can be
  // loaded beside the main conversion of the same bundle. Its textures do not
  // load (their streams name the old files); it is for its shaders.
  bool renameArchiveFiles = false;
  // Link a variant whose geometry stage cannot run (multiview forbids one)
  // without it, adding the object-to-clip transform when the geometry stage
  // did the projection, instead of leaving the variant on DirectX. Off in the
  // mod since 0.14.13: maps looked better and started faster without it.
  bool dropUntranslatableGeometry = false;
  // Leave untranslated the variants a Quest never selects in a converted
  // map: GPU instancing (turned off on converted materials) and unsplit
  // single-pass stereo. Most of a PC bundle's programs; far less to load and
  // compile. Not for donor builds, whose materials keep their instancing.
  bool skipQuestUnusedVariants = false;
};

ShaderConversion ConvertShadersToGles(std::string const& sourcePath,
                                      std::string const& destPath,
                                      ShaderConversionOptions const& options = {});

ShaderScan ScanShaders(std::string const& bundlePath);

// Puts working shaders into a Quest bundle that shipped them empty.
//
// A Quest bundle's shader can come with no programs at all: the mapper's Unity
// failed to compile it for Android (Hold My Hand's raymarched kaleidoscope).
// `donorPath` is the same map's PC bundle, already run through
// ConvertShadersToGles. For each shader named in `names`, the Quest bundle's
// empty Shader object is replaced, body and all, by the donor's translated
// one -- same path ID, so every material that uses it now gets the working
// shader -- and the result is written to `destPath`. The Quest bundle itself
// is not changed.
//
// A body is only moved between files built by the same Unity version, since
// it is read through the destination's type tree. Its object references
// (dependencies, non-modifiable textures) name objects of the PC bundle, so
// they are replaced by the Quest shader's own when both have the same number,
// and cleared otherwise. A shader missing from the donor, not translated there, or
// from a different Unity version is left as it was and listed in `skipped`.
struct ShaderMerge {
  Status status = Status::Corrupt;
  std::string message;
  int merged = 0;
  std::vector<std::string> mergedNames;
  std::vector<std::string> skipped;  // "name: why"
  std::vector<std::string> notes;    // merged, with a caveat
  uint64_t outputBytes = 0;
};

ShaderMerge MergeShadersInto(std::string const& questPath, std::string const& donorPath,
                             std::vector<std::string> const& names, std::string const& destPath);

// Human-readable one-line summary of a ShaderScan, for the log.
std::string DescribeShaderScan(ShaderScan const& scan);

std::string_view StatusText(Status status);
std::string BuildTargetName(int32_t target);

}
