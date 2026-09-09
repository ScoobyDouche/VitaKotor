# Minimap hardware clipping investigation

Status: root cause confirmed on hardware; shared KOTOR GUI viewport clipping
promoted to the normal build.

## Symptom

On real Vita hardware the in-game minimap overflows or appears misaligned against
its frame. The same build's minimap is correctly aligned in Vita3K. Related hazy
rectangles have been reported over the character and class-selection panels.

## Findings

The layout is not platform-dependent. Hardware and Vita3K logs both record the
same minimap-frame extent:

```text
{L=83 T=144 W=156 H=156} x0.7083 -> {L=58 T=102 W=111 H=111}
```

Earlier hardware experiments also ruled out two other theories:

- Resampling every NPOT upload to its padded width did not change the boxes.
- Forcing the only unscaled `238x238` `CSWGuiImage` through the normal 0.7083 GUI
  scale fired exactly once and did not change the minimap.

The defect also predates commit `8a89ff7`, which introduced RGBA4444/RGB565
texture conversion. That conversion did not create the minimap or panel
overflow. The shared viewport-clipping A/B subsequently ruled out blend state as
the cause of the reported character-selection haze.

The minimap does not use the ordinary image path by itself. It has dedicated
rendering in:

- `CSWGuiMainInterface::DrawMap(float)` at `libKOTOR.so + 0x29950c`.
- `CSWGuiMainInterface::UpdateAndDrawFogOfWar(...)` at `+0x29a39c`.
- `CSWGuiInGameMap::Draw(float)` at `+0x27b1e8`.
- `CSWGuiMapHider::Draw(float)` at `+0x27b2e0`.

`DrawMap` pans the map label and fog geometry, then brackets them with
`AurGUISetupViewport`/`AurGUICloseViewport`. `AurGUISetupViewport` calls
`glViewport`; it never enables `GL_SCISSOR_TEST` or calls `glScissor`.

The pinned vitaGL implementation only translates `glViewport` into
`sceGxmSetViewport`. It updates the coordinate transform but does not call
`sceGxmSetRegionClip`. vitaGL changes the hardware region clip only for an
enabled scissor test.

This creates a platform-sensitive failure because a viewport is a coordinate
transform rather than a scissor. The dedicated map path emits panned geometry,
and GXM permits it outside the local viewport when no region clip is set. The
hardware A/B confirmed that explicit clipping supplies the missing containment.

The same `AurGUISetupViewport` mechanism is used by character-stack, list-box,
computer-panel, and Pazaak overlay rendering, consistent with the related panel
overflow reports.

## Fix

The loader replaces KOTOR's internal `AurGUISetupViewport` and
`AurGUICloseViewport` PLT slots. It mirrors each nested GUI viewport into
`glScissor`, then restores the caller's prior scissor box and enabled state when
the matching viewport closes. This covers the minimap and other GUI panels that
use the same clipping abstraction without changing vitaGL's global `glViewport`
semantics.

Vita3K control result: passed. The already-correct minimap remained aligned and
gameplay stayed stable with the diagnostic enabled. The trace recorded the local
minimap viewport as `{x=5, y=437, w=102, h=102}`, followed by the expected restore
to `{x=0, y=0, w=960, h=544}` each frame. This confirms the hook targets the
dedicated minimap viewport rather than the earlier unrelated `238x238` image.

The hardware diagnostic package was `build-minimap-scissor/KOTOR.vpk`:

```text
SHA-256  bbe0168da594273928b4c7f67c2251196bf6e44a7b818707ba615a83ef434d05
eboot    9f56420c02a0ebc1d5334c7206a78e6c46cf8ba4e3cb712ffafe0fc0feede881
```

Hardware result: passed. The previously overflowing minimap became correctly
contained. The retrieved log confirmed the hook ran and recorded the same
alternating `{x=5, y=437, w=102, h=102}` local viewport and `960x544` restoration
seen in Vita3K. No crash marker was present.

## Validation

1. Vita3K retained its already-correct minimap and remained stable.
2. Real Vita changed from overflowing to correctly contained at the same known
   problem location.
3. Hardware telemetry confirmed the scoped path and viewport dimensions.
4. The shared implementation also corrected the six-class selector and
   Quick/Custom character screen on hardware.
5. The production implementation removes per-viewport diagnostic traces while
   retaining nested clipping and state restoration.

## Repository report location

The public GitHub issue list does not contain a minimap-specific issue. The
known limitation is recorded in `docs/releases/v0.1.11.md`, and the earlier
hardware experiments are preserved in commits `978c2e7`, `7df5ba3`, `39dce32`,
and `db101b0`.
