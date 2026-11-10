#pragma once

#include "GlobalNamespace/SaberType.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Transform.hpp"
#include "json.hpp"
#include "loading.hpp"
#include "selection.hpp"

namespace CustomModels {
    struct Trail {
        TrailInfo info;
        UnityEngine::Material* material;
        UnityEngine::Vector3 topPos;
        UnityEngine::Vector3 bottomPos;
    };

    struct Saber : AssetInfo {
        SaberInfo info;
        // set on the Trail / Menu Trail selections: any saber picked there is
        // used for its trail, so trails are always searched for and the
        // default trail is the fallback
        bool trailSelection = false;
        std::vector<Trail> leftTrails;
        std::vector<Trail> rightTrails;

        Saber(bool trailSelection = false) : trailSelection(trailSelection) {}

        void SetDefault() override;
        bool ParseInfo(std::string const& file) override;
        std::string ObjectName() override;
        void PostLoad() override;
    };

    extern int const SaberLayer;

    void InitSaber(UnityEngine::Transform* parent, bool menu, GlobalNamespace::SaberType type);
    void UpdateSaberColor(UnityEngine::Transform* parent, bool menu, GlobalNamespace::SaberType type);

    UnityEngine::Transform* PreviewSabers(UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, bool menu);
    void UpdateSabersPreview(UnityEngine::Transform* preview, bool menu);
}
