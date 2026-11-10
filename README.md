# Custom Models

The continuation of the popular mod Qosmetics for Beat Saber 1.40+. Allows the selection of user-created custom models for sabers, notes, and walls, along with small tweaks such as scale and trail length.

## How to install and use

The mod can be installed through MBF on available versions. QMOD files can also be downloaded directly from the releases tab. Unlike Qosmetics, all the features are contained in the single mod, with no additional modules needing to be downloaded.

No custom models are available by default, but can instead be downloaded from the Qosmetics discord server ([invite link](https://discord.gg/JNuFC7XMTk)). `.whacker`, `.weeeeeeeeeeeeeeeeeeeee`, `.box`, and `.cyoob` files are currently supported, along with the legacy formats `.qsaber`, `.qwall`, and `.qbloq`. **All of those files can be uploaded directly into MBF after installing the mod**, or can be manually placed into the following locations:

| File extension                | Folder                                                             |
| ----------------------------- | ------------------------------------------------------------------ |
| `.whacker`                    | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Sabers` |
| `.weeeeeeeeeeeeeeeeeeeee`      | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Sabers` |
| `.box`                        | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Walls`  |
| `.cyoob`                      | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Notes`  |
| `.qsaber`, `.qwall`, `.qbloq` | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Legacy` |
| `.zip`                        | `/sdcard/ModData/com.beatgames.beatsaber/Mods/CustomModels/Zips`   |

(Due to conflicts with custom songs, zip files cannot be uploaded into MBF, but can still be placed into the folder manually if desired.)

### Trails

The **Trail** tab (for both Ingame and Menu) lists every trail package (`.weeeeeeeeeeeeeeeeeeeee`) **and every installed saber**, so any trail can be used with any saber. Sabers listed for their trail are marked "(trail)". A saber that has a trail package is listed once, through the package. If a model has no trail of its own, the default trail is used, so picking a trail always shows one.

PC `.saber` conversion creates both a `.whacker` saber and a separate `.weeeeeeeeeeeeeeeeeeeee` trail package. The trail package references the paired `.whacker` bundle instead of storing a second copy. Deleting a saber also deletes trail packages that point at it. Trails on PC sabers are found from their trail start and end points; they use the game's trail material.

**Reserved for future Unity 6 support:** `.qweeeeeeeeeeeeeeeeeeeee`. This extension is only reserved by name; no loader, selector, converter, or runtime support is implemented for it.

### Conversion

- PC models (`.saber`, `.bloq`, `.pixie`) are converted in the background after the game starts, using the vendored Vivify Quest bundle converter (0.15.5). Converted models show up in the lists when it finishes. `.bloq`/`.pixie` always go to `Notes` and `.saber` to `Sabers`, wherever the source was placed.
- Legacy Quest models (`.qsaber`, `.qbloq`, `.qwall`) are converted at startup from `Legacy` **or** any of the model folders.
- A file that fails to convert is renamed to `*.failed` so it is not retried, slowing down or crashing every launch. If the game closes during a conversion, that file is set aside the same way on the next launch. **Convert All PC Models** retries `*.failed` files.
- Model bundles are unpacked once into `CustomModels/Cache` and loaded from there, which keeps memory use low for large models. The folder is cleaned automatically and can be deleted at any time.

After uploading model files, they can be selected and tweaked through the "Custom Models" mod tab, located directly to the left of the main menu.
