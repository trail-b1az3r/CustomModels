#include "VivifyBundleConvert.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace Vivify::BundleConvert;
    if (argc < 2) {
        std::cerr << "usage: vivify_asset_smoke MODEL.saber|MODEL.bloq|MODEL.pixie [...]\n";
        return 2;
    }

    ShaderConversionOptions options;
    options.separateStereoVariants = false;
    options.splitUnity2019 = false;
    options.singleViewOnly = false;

    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path const source(argv[i]);
        auto extension = source.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".saber" && extension != ".bloq" && extension != ".pixie") {
            std::cerr << source << ": unsupported model extension\n";
            failed++;
            continue;
        }
        if (!IsUnityBundleFile(source.string())) {
            std::cerr << source << ": not a UnityFS AssetBundle\n";
            failed++;
            continue;
        }

        auto const nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        auto const output = std::filesystem::temp_directory_path() /
                            ("custommodels-smoke-" + std::to_string(nonce) + ".bundle");
        auto converted = ConvertShadersToGles(source.string(), output.string(), options);
        std::filesystem::path const inspectPath =
            converted.status == Status::Success ? output : source;
        if (converted.status != Status::Success && converted.status != Status::AlreadyAndroid) {
            std::cerr << source << ": conversion failed: " << converted.message << "\n";
            failed++;
        } else {
            auto scan = ScanShaders(inspectPath.string());
            if (!scan.parsed) {
                std::cerr << source << ": converted bundle did not pass serialized-file validation: "
                          << scan.message << "\n";
                failed++;
            } else {
                std::cout << source.filename().string() << ": " << converted.message << "\n";
            }
        }
        std::error_code ec;
        std::filesystem::remove(output, ec);
    }
    return failed == 0 ? 0 : 1;
}
