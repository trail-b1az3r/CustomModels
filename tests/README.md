# Asset conversion tests

The host tests compile the exact Unity-independent converter sources shipped in
`src/vivify`; they do not substitute a Python or external Unity conversion
implementation.

Run on Linux/macOS:

```sh
cmake -S tests -B build/converter-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/converter-tests --parallel
ctest --test-dir build/converter-tests --output-on-failure
```

To exercise real PC model bundles (including the supplied legacy-era Unity
2018/2019 `.bloq` and `.saber` samples), pass the files to the integration
smoke runner:

```sh
./build/converter-tests/vivify_asset_smoke "T7-Eternal Construct.saber" \
  "LGBT Notes (Trans).bloq" "Television Notes.bloq"
```

The smoke runner calls the same vendored Vivify shader/bundle converter used by
the mod, then reparses the converted SerializedFiles. A failure is reported
with the source filename and converter diagnostic. The test files are not
copied into the repository because user model bundles can be large and may
contain third-party artwork.
