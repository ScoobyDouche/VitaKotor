# GUI viewport clipping on hardware

Status: fixed and validated on real Vita hardware and Vita3K.

## Symptom

The minimap and character-generation panels rendered correctly in Vita3K but
overflowed their intended frames on real hardware. Layout telemetry was identical
on both platforms, ruling out resolution scaling.

## Root cause

KOTOR uses `AurGUISetupViewport`/`AurGUICloseViewport` as a nested GUI clipping
stack. `AurGUISetupViewport` calls `glViewport`, but a viewport is a coordinate
transform rather than a clipping rectangle.

vitaGL correctly translates `glViewport` to `sceGxmSetViewport`; it only updates
the GXM region clip when `GL_SCISSOR_TEST` is enabled. Vita3K concealed KOTOR's
assumption, while real GXM allowed panned or oversized geometry outside the GUI
viewport.

Affected paths included:

- `CSWGuiMainInterface::DrawMap(float)` at `libKOTOR.so + 0x29950c`.
- `CSWGuiClassSelection::Draw(float)` at `+0x24ac08`.
- `CSWGuiMainCharGen::Draw(float)` at `+0x287f7c`.
- Shared `CSWGuiScene` and `CSWGuiPanel` drawing.

## Fix

The loader replaces KOTOR's internal `AurGUISetupViewport` and
`AurGUICloseViewport` `R_ARM_JUMP_SLOT` entries. Each successful GUI viewport
setup saves the caller's scissor box and enabled state, mirrors the resulting
viewport to `glScissor`, and enables scissoring. The matching close restores the
exact previous state. A bounded stack preserves nested panel behavior.

PLT replacement is required. `AurGUICloseViewport` begins with a PC-relative
literal load, so copying its prologue into the generic trampoline reads from the
wrong address and crashes on hardware.

The fix remains at KOTOR's semantic GUI boundary rather than changing vitaGL's
standards-correct `glViewport` behavior globally.

## Validation

- The minimap viewport was `{5,437 102x102}` with full-screen restoration.
- Character selection exercised six nested model viewports at depth two.
- Quick/Custom character generation exercised panel `{117,0 726x544}` and model
  `{292,233 184x211}` viewports.
- All three affected screens rendered correctly on real hardware.
- Additional nested GUI viewports executed without stack overflow or crash.
- Vita3K retained its previously correct rendering.

Earlier NPOT resampling, forced image scaling, texture-format, blend-state, and
screen-specific clipping theories were rejected by hardware A/B tests. Their
detailed records remain available in Git history before this cleanup.
