# Authoring Presets

NodeSynth presets are plain `.json` patches — identical format to user-saved files. There's no separate preset schema, no special fields. Whatever the **Save As…** menu produces is a valid preset.

## Where presets live

There are two preset roots, scanned at startup:

1. **Bundled presets** ship with the build, in `<repo>/presets/<Category>/<Name>.json`. These are committed to git and copied next to the binary by CMake.
2. **User presets** live in `~/.nodesynth/user_presets/<Category>/<Name>.json` (`%USERPROFILE%\.nodesynth\user_presets\` on Windows). Not version-controlled.

Both directories use the same layout: one level of category subdirectories, each holding `.json` files. Files at the root of the preset directory show up as uncategorised entries above the category submenus.

When the same preset filename exists in both bundled and user dirs, the user version wins. This lets you locally override a bundled preset without modifying the repo.

## Adding a new bundled preset

The fastest workflow:

1. **Build a patch in the running app.** Tweak nodes, params, links until it sounds the way you want. Set the patch metadata (Patch Info panel) — Name, Author, Notes.
2. **Save As…** into `<repo>/presets/<Category>/<Name>.json`. Pick or create whichever category fits (Bass / Lead / Pad / FX / Init are the conventional buckets, but any subdirectory name works — it shows up as a submenu label verbatim).
3. **Rebuild.** The CMake `POST_BUILD` step copies the updated `presets/` next to the binary so the next launch sees it. If the category directory is brand-new, re-run `cmake -S . -B build` once so the configure-time scan picks it up.
4. **Commit the JSON.** Pretty-printed JSON diffs cleanly; reviewers can see exactly which params changed.

For programmatically generated presets (e.g. variants of a base patch with different params), see `tests/EmitPresets.test.cpp`. It's a hidden Catch2 test (`[.][preset-emit]` tag) that builds graphs in code and calls `SavePatch`. Regenerate with:

```bash
./build/Release/nodesynth_tests.exe "[preset-emit]"
```

This runs from the repo root and writes to `./presets/`. Useful for keeping a family of related presets in lockstep — change one parameter in the test, rerun, get N updated `.json` files.

## Refreshing without restarting

The **File → Presets → Refresh** menu item rescans both directories and rebuilds the index. Drop a new `.json` into `~/.nodesynth/user_presets/<Category>/` and click Refresh — no rebuild, no restart.

## Sample-rate hint

Saved patches record `metadata.sample_rate_hint`. If a preset was saved at 48 kHz and the user opens it on a 96 kHz device (or vice versa), a non-blocking warning surfaces in the UI. Time-based effects (`FDelay`, `FReverb`, smoother time constants) recompute on `Prepare(SampleRate)` so they still sound correct — the warning just flags that the preset author tested at a different rate.

If you author at an unusual rate (192 kHz, 22050 Hz), that warning will fire for most users. Consider opening at 48 kHz for testing before saving the bundled version.

## Style conventions for bundled presets

- **Master gain ≤ 0.20** when the patch is polyphonic and uses the seeded-style topology (per-voice ADSR feeding a sum). Per `docs/PLAN-PHASE-3-VOICES.md` §1.9 and `CLAUDE.md`'s polyphony note, the synthesised mixer does a straight sum of N voices with no auto-attenuation. 0.15 is the seeded default; 0.18 is fine for sparse-chord patches; above 0.25 will clip on full chords.
- **Display name ≤ 20 chars.** Keeps the menu narrow.
- **Notes are useful, not aspirational.** Write what the preset *is*, not what it could become with further editing. Future-you will skim them in the menu and pick fast.
- **One topology per preset.** Don't stash exotic effect chains in the FX category if the preset's identity is the oscillator + envelope. Save the effects-heavy variant as its own preset.

## What's deferred

Tagging, search, favourites, and BPM-aware preset filtering are Phase 6+ ideas. v1 presets are browse-by-directory only; if the menu gets unwieldy, the bar to add tagging is "we have >40 presets and the submenus are painful to navigate".
