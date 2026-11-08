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
        UnityEngine::AssetBundle* bundle;
        UnityEngine::GameObject* asset;

        UnityEngine::GameObject* GetChild(std::string const& name);
        UnityEngine::GameObject* InstantiateChild(std::string const& name);
        UnityEngine::GameObject* Instantiate();

        void Load(std::string file, std::string assetName, std::function<void(bool, bool)> onDone);
        void Unload();
    };

    extern std::map<std::string, Manifest> files;

    void LoadManifests();
}
