#include "main.hpp"

#include "assets.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSMLDataCache.hpp"
#include "config.hpp"
#include "converter.hpp"
#include "custom-types/shared/register.hpp"
#include "defaults.hpp"
#include "hooks.hpp"
#include "lapiz/shared/zenject/Zenjector.hpp"
#include "legacy.hpp"
#include "loading.hpp"
#include "registration.hpp"
#include "scotland2/shared/modloader.h"
#include "selection.hpp"
#include "settings.hpp"

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

static bool NonMinorVersionEquals(std::string v1, std::string v2) {
    return v1.substr(0, v1.rfind(".")) == v2.substr(0, v2.rfind("."));
}

extern "C" void setup(CModInfo* info) {
    *info = modInfo.to_c();

    std::filesystem::path dataFolder = getDataDir(MOD_ID);
    for (auto& subfolder : {"Sabers", "Notes", "Walls", "Legacy", "Zips", "Cache"}) {
        auto path = dataFolder / subfolder;
        if (!direxists(path.string()))
            mkpath(path.string());
    }
    getConfig().Init(modInfo);

    auto fails = getConfig().LoadingFailures.GetValue();

    if (!NonMinorVersionEquals(getConfig().ModVersion.GetValue(), VERSION))
        fails.clear();
    else if (!getConfig().LoadingGuard.GetValue().empty())
        fails.emplace_back(getConfig().LoadingGuard.GetValue());

    // allow retrying by deleting the file, if you really want to
    std::erase_if(fails, [](std::string const& file) { return !fileexists(file); });

    getConfig().LoadingFailures.SetValue(fails);
    getConfig().LoadingGuard.SetValue("");
    getConfig().ModVersion.SetValue(VERSION);

    Paper::Logger::RegisterFileContextId(MOD_ID);
    logger.info("Completed setup!");
}

extern "C" void late_load() {
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    Hooks::Install();

    // eagerly converting the files on the filesystem has two advantages:
    // 1. it only has to be done once per file, so I can do it with an invisible lag spike instead of async stuff
    // 2. it allows most of the later loading logic to be unified (and it's complicated enough that way)
    CustomModels::ConvertLegacyModels();
    CustomModels::MoveQosmeticsFolders();

    std::filesystem::path modelFolder = getDataDir(MOD_ID);
    // If the last launch died converting a PC model, set that file aside
    // first so the same crash cannot repeat on every launch.
    CustomModels::Converter::RecoverInterruptedConversion(modelFolder);

    CustomModels::LoadDefaults();
    CustomModels::LoadManifests();
    CustomModels::CleanBundleCache();
    CustomModels::LoadSelections();

    // PC models (.saber/.bloq/.pixie) are converted on a background thread.
    // This used to run here, on the main thread, before the game had even
    // finished starting: with a few large models it took long enough (and
    // enough memory) for the game to be killed on every launch. Converted
    // models and their .weeee... trail packages appear once it finishes.
    CustomModels::Converter::ConvertAllAsync(modelFolder, false, [](CustomModels::Converter::BatchResult result) {
        if (result.converted || result.failed)
            logger.info("startup conversion: {} converted, {} failed", result.converted, result.failed);
        for (auto const& error : result.errors)
            logger.warn("startup model conversion: {}", error);
        if (result.converted > 0) {
            CustomModels::LoadManifests();
            CustomModels::SelectionSettings::RefreshIfCreated();
        }
    });

    Lapiz::Zenject::Zenjector::Get(modInfo)->Install(Lapiz::Zenject::Location::Player | Lapiz::Zenject::Location::Tutorial, CustomModels::Register);

    BSML::Register::RegisterMenuButton("Custom Models", "Select and modify custom models", CustomModels::SettingsCoordinator::Present);
}

#define BSML_ICON(name) \
    BSML_DATACACHE(name) { return IncludedAssets::icons::name##_png; }

BSML_ICON(saber_length);
BSML_ICON(saber_width);
BSML_ICON(trail_duration);
BSML_ICON(trail_width);
BSML_ICON(trail_offset);
BSML_ICON(note_size);
BSML_ICON(bomb);
BSML_ICON(debris);
BSML_ICON(no_frame);
BSML_ICON(no_core);
