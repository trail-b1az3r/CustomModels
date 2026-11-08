#include "VivifyBundleConvert.hpp"
#include "VivifyTextureDecode.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::filesystem::path TempFile(std::string const& name) {
    return std::filesystem::temp_directory_path() / ("custommodels-" + name);
}

void WriteBytes(std::filesystem::path const& path, std::vector<uint8_t> const& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}
}

int main() {
    using namespace Vivify;

    // Extension-independent texture decoder coverage used by PC model loading.
    assert(TextureDecode::FromUnityTextureFormat(10) == TextureDecode::Format::DXT1);
    assert(TextureDecode::FromUnityTextureFormat(12) == TextureDecode::Format::DXT5);
    assert(TextureDecode::FromUnityTextureFormat(25) == TextureDecode::Format::BC7);
    assert(TextureDecode::FromUnityTextureFormat(999) == TextureDecode::Format::Unsupported);
    assert(TextureDecode::CompressedSize(TextureDecode::Format::DXT1, 4, 4) == 8);
    assert(TextureDecode::CompressedSize(TextureDecode::Format::DXT5, 4, 4) == 16);

    std::array<uint8_t, 8> bc1{};
    std::vector<uint8_t> rgba;
    assert(TextureDecode::DecodeToRgba32(TextureDecode::Format::DXT1, bc1.data(), bc1.size(), 4, 4, 1, rgba));
    assert(rgba.size() == 4u * 4u * 4u);
    assert(!TextureDecode::DecodeToRgba32(TextureDecode::Format::DXT1, bc1.data(), 4, 4, 4, 1, rgba));

    auto const invalid = TempFile("not-a-bundle.bin");
    WriteBytes(invalid, {'n', 'o', 't', '-', 'a', '-', 'b', 'u', 'n', 'd', 'l', 'e'});
    assert(!BundleConvert::IsUnityBundleFile(invalid.string()));
    auto invalidResult = BundleConvert::ConvertToAndroid(invalid.string(), TempFile("invalid-out").string());
    assert(invalidResult.status == BundleConvert::Status::NotAUnityBundle ||
           invalidResult.status == BundleConvert::Status::SourceUnreadable);

    // A valid signature with a truncated header must be rejected safely, not
    // treated as a successful no-op or read past the input buffer.
    auto const truncated = TempFile("truncated.bundle");
    WriteBytes(truncated, {'U', 'n', 'i', 't', 'y', 'F', 'S', 0, 0});
    assert(BundleConvert::IsUnityBundleFile(truncated.string()));
    auto truncatedResult = BundleConvert::ConvertToAndroid(truncated.string(), TempFile("truncated-out").string());
    assert(truncatedResult.status == BundleConvert::Status::Corrupt ||
           truncatedResult.status == BundleConvert::Status::UnsupportedCompression);
    auto scan = BundleConvert::ScanShaders(truncated.string());
    assert(!scan.parsed);

    std::error_code ec;
    std::filesystem::remove(invalid, ec);
    std::filesystem::remove(truncated, ec);
    std::filesystem::remove(TempFile("invalid-out"), ec);
    std::filesystem::remove(TempFile("truncated-out"), ec);

    std::cout << "Vivify converter unit tests passed\n";
    return 0;
}
