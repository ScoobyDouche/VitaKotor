# ProfileCrowdSkip — test plan

What this branch tries against the busy-area slowdown (Taris Upper City South,
the Undercity), and what each test needs from a hardware run. Every option
below is off by default, so the build behaves like v0.5.2 until one is set
under `[Vita Options]` in `ux0:data/kotor/swkotor.ini`.

## 1. Find where the 30 ms goes: `Profile=3`

A true program-counter profile of the game thread (`loader/pcsamp.c`). The
existing call-site sampler (`Profile=1`) only sees the game thread when it
makes a call, so leaf loops (matrix maths, culling tests, skinning) are
invisible to it. This one sees the instruction.

1. Set `Profile=3`, start the game, check `log.txt` has
   `[pcs] armed: libKOTOR text ...`. If it says `kuKernelMemProtect refused`,
   the installed kubridge is too old for it.
2. Stand in Upper City South with a crowd for a minute, then quit.
3. Off-device:
   ```
   arm-vita-eabi-nm -D -C --defined-only libKOTOR.so > k.syms
   arm-vita-eabi-nm -D -C --defined-only libandroid_port.so > p.syms
   python3 tools/samp_report.py log.txt k.syms p.syms <first window> <last window>
   ```
   The windows are the `[pcs] window N` lines spent in the crowd.

Each window also logs a summary line:

    [pcs]   game thread busy 93% of the window; samples in libKOTOR 11800, port 1200; ...

`busy` is the game thread's own CPU time: the rest of the window it was
blocked (waiting on the graphics thread, the vblank, a lock). Samples are
taken in libKOTOR and libandroid_port; busy time missing from both is the
loader, vitaGL or libc.

The report prints four tables:
- **by function containing the PC** -- self time, where the CPU actually is;
- **by return address** -- who called the leaf;
- **inclusive** -- samples anywhere inside a function *or the functions it
  called*, from call sites found on the game thread's stack. This is the
  one that says which engine system (scene walk, animation, lighting,
  shadows, object updates) the frame belongs to;
- **hottest instructions** -- offsets to look up in the disassembly. That
list decides the next step: a few hot leaf functions can be replaced with
NEON versions through the existing hook mechanism; one self-contained
per-object loop is the only real candidate for splitting across cores.

## 2. See what the crowd is made of: `CrowdCensus=1`

`loader/crowd.c` hooks `Gob::Render` inside the dynamic pass and logs, every
15 s, objects drawn per class, milliseconds per class, and a distance
histogram. Look for:

- `[crowd] on: position at Gob+N` — the position field was found. If it says
  `Gob::SetPosition not understood`, post the hex lines after it.
- `[crowd] camera class ...: a Gob, distances on` — distances are valid.
- The per-class lines: which class names are the pedestrians, and how much
  of the frame they take.

## 3. Skip distant crowds: `CrowdDistance=30`

With the census on as well, compare frame times and the per-class ms against
the run above. Then narrow it with `CrowdClasses=` set to the pedestrian
class name the census showed, so doors and placeables are left alone. Try
20, 30 and 40 m and note where pop-in becomes noticeable.

## Not in this branch: multithreading the scene

Splitting the game's own per-object work across cores needs a disassembly of
the loop in question and a proof that it touches no shared state. Step 1 is
what identifies that loop; until then there is nothing safe to split.
