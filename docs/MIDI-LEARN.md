# MIDI Learn

Bind a hardware controller's dials/sliders/buttons to NodeSynth node parameters in three clicks. Mappings save with the patch.

## Quick start

1. **Add a MIDI Input node** to the graph. Right-click background → **Create Node** → **MIDI Input**. Even if you don't wire its outputs into anything, the node owns the connection to your controller — MIDI Learn won't function without it.
2. **Pick your device** in the MIDI Input node's `Device` Choice param. Verify it's connected by playing a note (Gate output should go to 1.0).
3. **Right-click any param's slider** in the property panel of any node → **MIDI Learn**. The slider gains a yellow `[LEARN]` badge. The status bar (or just the slider) prompts you to wiggle a control.
4. **Wiggle a knob/slider** on the controller. The first CC event past a 200 ms guard window establishes the binding. The `[LEARN]` badge turns into a blue `CC<n>` indicator.
5. From now on, that physical control drives that param, scaled to the param's full range.

To remove a mapping: right-click the slider → **Unmap MIDI**, or use the **MIDI Mappings** docked panel.

To cancel a learn-in-progress: press **Esc**.

## How values are scaled

CC values are 7-bit (0..127). The scaling depends on the param's kind:

- **Float**: linear interpolation across the param's `[Min, Max]` range. For params declared `bLogarithmic = true` (e.g. `FOscillator.Frequency`), exponential mapping so a midpoint CC produces a perceptually-midpoint frequency.
- **Choice**: integer index — CC 0..127 divided into N equal regions where N is the choice count.
- **Bool**: threshold at CC ≥ 64.
- **String** (e.g. `FSidPlayer.File`): not mappable. The MIDI Learn menu item is hidden.

## Channel handling

A learned mapping captures the **specific channel** the CC came in on (1..16). If your controller is on channel 1, only channel 1 messages will trigger that mapping. To make a mapping match all channels, you'd edit the mapping's channel field to 0 — currently that's only reachable via patch-JSON editing; a UI for it is deferred.

## Mappings panel

The **MIDI Mappings** docked panel shows every current binding, one row per mapping:

```
[x]  Ch1  CC16   -> Gain.Gain
[x]  Ch1  CC17   -> SVF.Cutoff
[x]  Ch1  CC18   -> SVF.Resonance
```

Click `[x]` to remove. Same effect as right-click → Unmap MIDI on the param.

## Persistence

Mappings save in the patch JSON under a top-level `midi_mappings` array:

```json
"midi_mappings": [
  { "channel": 1, "cc": 16, "node_id": 4, "param_index": 0 },
  { "channel": 1, "cc": 17, "node_id": 4, "param_index": 1 }
]
```

Older patches without this field load with no mappings (back-compat). When a target node is deleted from the graph, the associated mappings are swept and the deletion is recorded in the undo history alongside the node — undoing the node deletion restores its mappings too.

## Conflicts

If you learn the same physical control twice, the second mapping silently replaces the first. Both halves of that replacement are captured in one composite undo entry, so a single Ctrl+Z restores the original mapping.

## Limitations

| | |
|---|---|
| **Requires an `FMidiInput` node** | The node owns RtMidi. Without it, MIDI Learn has no event source. |
| **One MIDI device at a time** | Only the first `FMidiInput` in the graph supplies CC events. A multi-device setup is deferred. |
| **CC sweeps don't enter undo history** | Treated as external input, like MIDI notes. The mappings themselves are undoable; the param automation they produce isn't. |
| **No channel wildcard UI** | A mapping with `channel = 0` ("any") matches all channels, but the Learn flow always captures the specific channel that arrived. Edit the JSON manually if you need wildcard behaviour. |
| **No range / curve overrides per mapping** | Mappings cover the param's full declared range with the kind's default curve (linear or log). v2 enhancement. |
| **Some controllers send a snapshot dump on connection** | The 200 ms guard window after entering Learn mode skips initial events to avoid binding to the dump. If your controller's snapshot takes longer, wait a beat before wiggling. |
| **Nanokontrol2 transport buttons emit CC** | A misclick during Learn could bind a button (CC 41 / 42 / 43 etc.) to the slider instead of the actual fader. Unmap and try again — no harm done. |
