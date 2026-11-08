#pragma once

#include "config-utils/shared/config-utils.hpp"

namespace CustomModels {
    DECLARE_JSON_STRUCT(TrailObject) {
        VALUE(int, trailId);
        VALUE(bool, isTop);
    };

    DECLARE_JSON_STRUCT(TrailInfo) {
        VALUE(int, trailId);
        VALUE(int, colorType);
        VALUE(ConfigUtils::Color, trailColor);
        VALUE(ConfigUtils::Color, multiplierColor);
        VALUE(int, length);
        VALUE(float, whiteStep);
    };

    DECLARE_JSON_STRUCT(SaberInfo) {
        VALUE_DEFAULT(bool, hasTrail, false);
        VALUE_DEFAULT(bool, keepFakeGlow, false);
        VALUE_DEFAULT(bool, isLegacy, false);
        // Set on models this mod converted from a PC bundle: their shaders are
        // cross-compiled, and the translated instancing variants misplace
        // copies on Quest, so instancing is turned off before use.
        VALUE_DEFAULT(bool, fromPc, false);
    };

    DECLARE_JSON_STRUCT(NoteInfo) {
        VALUE_DEFAULT(bool, hasDebris, false);
        VALUE_DEFAULT(bool, hasChainHeadDebris, false);
        VALUE_DEFAULT(bool, hasChainLinkDebris, false);
        VALUE_DEFAULT(bool, hasSlider, false);
        VALUE_DEFAULT(bool, hasBomb, false);
        VALUE_DEFAULT(bool, showArrows, false);
        VALUE_DEFAULT(bool, isMirrorable, false);
        VALUE_DEFAULT(bool, isLegacy, false);
        VALUE_DEFAULT(bool, fromPc, false);
    };

    DECLARE_JSON_STRUCT(WallInfo) {
        VALUE_DEFAULT(bool, replaceCoreMaterial, false);
        VALUE_DEFAULT(bool, replaceFrameMaterial, false);
        VALUE_DEFAULT(bool, replaceCoreMesh, false);
        VALUE_DEFAULT(bool, replaceFrameMesh, false);
        VALUE_DEFAULT(bool, disableCore, false);
        VALUE_DEFAULT(bool, disableFrame, false);
        VALUE_DEFAULT(bool, disableFakeGlow, false);
        VALUE_DEFAULT(bool, isMirrorable, false);
        VALUE_DEFAULT(bool, isLegacy, false);
    };

    DECLARE_JSON_STRUCT(Descriptor) {
        VALUE(std::string, author);
        VALUE(std::string, objectName);
        VALUE(std::string, description);
        VALUE_DEFAULT(std::string, coverImage, "");
    };

    DECLARE_JSON_STRUCT(Manifest) {
        VALUE(std::string, androidFileName);
        VALUE(Descriptor, descriptor);
        VALUE(UnparsedJSON, config);
    };

    DECLARE_JSON_STRUCT(LegacyDescriptor) {
        VALUE(std::string, authorName);
        VALUE(std::string, objectName);
        VALUE(std::string, description);
    };

    DECLARE_JSON_STRUCT(LegacyTrail) {
        VALUE(std::string, name);
        VALUE(int, colorType);
        VALUE(ConfigUtils::Color, trailColor);
        VALUE(ConfigUtils::Color, multiplierColor);
        VALUE(int, length);
        VALUE(float, whiteStep);
    };

    DECLARE_JSON_STRUCT(LegacySaberConfig) {
        VALUE_DEFAULT(bool, enableFakeGlow, false);
        VALUE_DEFAULT(bool, hasCustomTrails, false);
        VECTOR_DEFAULT(LegacyTrail, leftTrails, {});
        VECTOR_DEFAULT(LegacyTrail, rightTrails, {});
    };

    DECLARE_JSON_STRUCT(LegacyNoteConfig) {
        VALUE_DEFAULT(bool, disableBaseGameArrows, true);
        VALUE_DEFAULT(bool, hasBomb, false);
        VALUE_DEFAULT(bool, hasDebris, false);
    };

    DECLARE_JSON_STRUCT(LegacyWallConfig) {
        VALUE_DEFAULT(bool, replaceCoreMaterial, false);
        VALUE_DEFAULT(bool, replaceCoreMesh, false);
        VALUE_DEFAULT(bool, disableFrame, false);
        VALUE_DEFAULT(bool, disableFakeGlow, false);
        VALUE_DEFAULT(bool, replaceFrameMaterial, false);
        VALUE_DEFAULT(bool, replaceFrameMesh, false);
        VALUE_DEFAULT(bool, scoreSubmissionDisabled, false);
        VALUE_DEFAULT(bool, moreThan1Core, false);
        VALUE_DEFAULT(bool, moreThan1Frame, false);
    };
}
