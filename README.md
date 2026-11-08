# Custom Models

The continuation of the popular mod Qosmetics for Beat Saber 1.40+. Allows the selection of user-created custom models for sabers, notes, and walls, along with small tweaks such as scale and trail length.

## How to install and use

The mod can be installed through MBF on available versions. QMOD files can also be downloaded directly from the releases tab. Unlike Qosmetics, all the features are contained in the single mod, with no additional modules needing to be downloaded.

No custom models are available by default, but can instead be downloaded from the Qosmetics discord server ([invite link](https://discord.gg/JNuFC7XMTk)). `.whacker`, `.weeeeeeeeeeeeeeeeeeeee`, `.box`, and `.cyoob` files are currently supported, along with the legacy formats `.qsaber`, `.qwall`, and `.qbloq`. **All of those files can be uploaded directly into MBF after installing the mod**, or can be manually placed into the following locations:

| File extension                | Folder                                                             |
| ----------------------------- | ------------------------------------------------------------------ |
| `.whacker`                    | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Sabers` |\n| `.weeeeeeeeeeeeeeeeeeeee`      | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Sabers` |
| `.box`                        | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Walls`  |
| `.cyoob`                      | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Notes`  |
| `.qsaber`, `.qwall`, `.qbloq` | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Legacy` |
| `.zip`                        | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Zips`   |

(Due to conflicts with custom songs, zip files cannot be uploaded into MBF, but can still be placed into the folder manually if desired.)

PC `.saber` conversion creates both a `.whacker` saber and a separate `.weeeeeeeeeeeeeeeeeeeee` trail-selection package. The trail package is selectable in the Trail and Menu Trail lists; it keeps the Unity bundle dependencies intact so trail materials and endpoint transforms remain valid.\n\nAfter uploading model files, they can be selected and tweaked through the "Custom Models" mod tab, located directly to the left of the main menu.
