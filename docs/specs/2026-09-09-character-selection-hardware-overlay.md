# Character-selection hardware overlay investigation

Status: root cause confirmed on hardware; shared KOTOR GUI viewport clipping
promoted to the normal build.

## Symptom

The first new-game class-selection screen is correct in Vita3K. On real Vita,
translucent blue panel geometry extends far outside its intended controls and
overlays the screen. Text, portrait, and character placement remain aligned.

## Render path

The Quick Character / Custom Character screen is drawn by
`CSWGuiMainCharGen::Draw(float)` at `libKOTOR.so + 0x287f7c`. It draws the
selected `CSWGuiScene`, then delegates the enclosing panel draw. Both
`CSWGuiScene::Draw(float)` and `CSWGuiPanel::Draw(float)` bracket their contents
with `AurGUISetupViewport`/`AurGUICloseViewport`.

The preceding six-class selector is drawn by
`CSWGuiClassSelection::Draw(float)` at `+0x24ac08`. It uses the same GUI viewport
stack for its enclosing panel and six individual `CSWGuiScene` controls. The
minimap uses that stack as well.

As confirmed by the minimap hardware fix, the pinned vitaGL `glViewport` path
does not create a GXM region clip. A viewport transforms coordinates but does not
contain geometry. The emulator renders this screen correctly, while hardware can
expose geometry outside these nested viewports.

## Fix

The first diagnostic hooked only `CSWGuiClassSelection::Draw(float)`. It did not
change the later Quick/Custom screen because it was the wrong screen boundary.
An exact `CSWGuiMainCharGen::Draw(float)` diagnostic then corrected that screen
on hardware. Its trace confirmed the panel viewport `{117,0 726x544}`, model
viewport `{292,233 184x211}`, and full-screen restoration.

The production fix applies the same behavior to the common
`AurGUISetupViewport`/`AurGUICloseViewport` abstraction. It maintains a nested
stack of the caller's scissor box and enabled state. Every successful GUI
viewport setup mirrors its resulting `glViewport` to `glScissor`; the matching
close restores exactly the prior state.

The first global implementation hooked both function prologues. That was unsafe:
`AurGUICloseViewport` starts with a PC-relative literal load, which the generic
trampoline copied without relocation. It crashed on hardware at
`libKOTOR.so + 0x42ca7a`. The corrected implementation replaces the functions'
own `R_ARM_JUMP_SLOT` entries instead, preserving the original function bodies
for wrapper call-through.

The exact-screen Vita3K and hardware controls both passed. The corrected global
PLT implementation then passed on hardware across the minimap, six-class
selector, Quick/Custom screen, and additional nested GUI viewports, with no crash
or stack overflow.

## Validation

1. Vita3K retained correct rendering across both character-generation screens.
2. Real Vita rendered the six-class selector and Quick/Custom screen correctly.
3. Real Vita retained the corrected minimap.
4. Hardware telemetry covered nested viewport depths one and two, with no stack
   overflow or crash.
