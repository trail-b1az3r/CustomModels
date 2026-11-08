#pragma once

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Material.hpp"
#include "UnityEngine/Shader.hpp"

namespace CustomModels {
    namespace Material {
        extern int const CustomColors;
        extern int const Glow;
        extern int const Bloom;
        extern int const Color;
        extern int const OtherColor;
        extern int const Alpha;
        extern int const StencilRefID;
        extern int const StencilComp;
        extern int const StencilOp;
        extern int const BlendSrcFactor;
        extern int const BlendDstFactor;
        extern int const BlendSrcFactorA;
        extern int const BlendDstFactorA;
        extern int const SlicePos;
        extern int const CutPlane;
        extern int const TransformOffset;
        extern int const SizeParams;
    }

    bool ShouldColor(UnityEngine::Material* material);
    bool ShouldColorReplaced(UnityEngine::Material* material);

    void SetMirrorableProperties(UnityEngine::Material* material, bool mirror);

    void ReplaceMaterials(UnityEngine::GameObject* object);
    void ClearMaterialsCache();

    // Cross-compiled PC shaders have instancing variants that misplace every
    // copy but one on Quest, where Beat Saber batches notes as instanced draws.
    // Turning instancing off makes each object draw through the plain variant.
    void DisableInstancing(UnityEngine::GameObject* object);
}
