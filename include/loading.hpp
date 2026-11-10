#pragma once

#include "UnityEngine/AssetBundle.hpp"
#include "UnityEngine/GameObject.hpp"
#include "json.hpp"

namespace CustomModels {
    struct Asset {
        std::string currentFile;
        std::string loadingFile;
        std::string currentBundleFile;
        std::string loadingBundleFile;
        UnityEngine::AssetBundle* bundle = nullptr;
        UnityEngine::GameObject* asset = nullptr;
        uint64_t loadId = 0;

        UnityEngine::GameObject* GetChild(std::string const& name);
        UnityEngine::GameObject* InstantiateChild(std::string const& name);
        UnityEngine::GameObject* Instantiate();

        void Load(std::string file, std::string assetName, std::function<void(bool, bool)> onDone);
        void Unload();
    };

    extern std::map<std::string, Manifest> files;

    void LoadManifests();
    // Removes cached bundle files that no longer belong to an installed model.
    // Only safe before anything has been loaded (startup).
    void CleanBundleCache();
}
