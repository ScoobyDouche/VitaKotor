# GUI viewport clipping on hardware

Status: enabled by default and validated on real Vita hardware and Vita3K.

## Current behavior

After the startup PLT replacement succeeds, every successful KOTOR
`AurGUISetupViewport` call establishes a matching hardware scissor region.
`AurGUICloseViewport` restores the exact scissor box and enabled state that
existed before that viewport was opened. Nested GUI panels are handled by a
bounded 16-entry state stack.

If setup rejects a viewport, the state pushed by the loader is restored
immediately. Calls beyond the bounded depth pass through without additional
clipping and are counted so the matching closes remain balanced. A stack overflow
is logged; none occurred during hardware validation.

## Root cause

KOTOR uses `AurGUISetupViewport`/`AurGUICloseViewport` as a nested GUI clipping
abstraction. `AurGUISetupViewport` calls `glViewport`, but OpenGL defines a
viewport as a coordinate transform rather than a clipping rectangle.

vitaGL correctly translates `glViewport` to `sceGxmSetViewport`; it updates the
GXM region clip only when `GL_SCISSOR_TEST` is enabled. Vita3K concealed KOTOR's
assumption, while real GXM allowed panned or oversized geometry outside the GUI
viewport.

The failure appeared in:

- The in-game minimap.
- The six-class new-game selector.
- The Quick Character / Custom Character screen.

The same shared GUI mechanism is also used by scene controls, list boxes,
computer panels, Pazaak overlays, and other nested panels.

## Implementation

`loader/main.c` locates KOTOR's internal `R_ARM_JUMP_SLOT` entries for:

- `_Z19AurGUISetupViewportiiiiRK6Vectorbf`
- `_Z19AurGUICloseViewportv`

It verifies that both slots exist before changing either, saves the resolved
original targets, and replaces both slots with loader wrappers. PLT replacement
is required because `AurGUICloseViewport` starts with a PC-relative literal load.
Copying that prologue into the generic Thumb trampoline reads from the wrong
address and caused a hardware data abort at `libKOTOR.so + 0x42ca7a` during the
rejected prototype.

While a GUI viewport is active, `loader/gl_patch.c` mirrors each KOTOR
`glViewport` call to `glScissor` and enables `GL_SCISSOR_TEST`. Closing a viewport
calls the untouched original KOTOR function first, then restores the saved
scissor state.

The change intentionally remains at KOTOR's semantic GUI boundary. Altering
vitaGL so every `glViewport` implied a scissor would violate OpenGL behavior and
could clip legitimate rendering in this and other applications.

## Validation

The shared PLT implementation passed on real Vita hardware and Vita3K:

- Minimap viewport: `{5,437 102x102}`.
- Character-selection panel: `{117,0 726x544}`.
- Six class-model viewports ran at nested depth two.
- Quick/Custom model viewport: `{292,233 184x211}`.
- Additional nested GUI viewports ran at depths one and two.
- The minimap and both character-generation screens rendered correctly.
- No stack overflow, state leak, or crash occurred.

The production build retains one startup marker,
`[gui:viewport] nested AurGUI clipping enabled via PLT`, and logs installation
failure or stack overflow. If either PLT slot is absent, neither is replaced and
the game continues without this correction. Per-viewport diagnostic traces are
removed.

Earlier NPOT resampling, forced image scaling, texture-format, blend-state, and
screen-specific clipping theories were rejected by hardware A/B tests. Their
detailed records remain available in Git history before the documentation
cleanup.
