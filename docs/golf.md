# 3-D Golf in dosrt

How INN starts 3-D Golf (`GOLF.EXE`), what it needs from the host, and how to run it alone. Message layouts are
in the sibling project's `docs/protocol/int14h-census/golf.md`; this page describes the run.

## Files

`TSN.PRG` block `Golf`: `cd GOLF`, `golf.exe`. The directory `\INN\GOLF` holds the unpacked `GOLF.EXE` (Borland
C++ 3.1, large model, Borland VROOMM overlays at the end of the file, Borland's floating-point emulator), the
holes (`HOLE01.DA`-`HOLE18.DA`), the course file, `.TFX` graphics, palettes, fonts and `SOUND.SFX`. 3-D Golf is
SierraLand's only DOS program: MiniGolf, Stratego, Graffiti, PaintBall and the other Waiting Room games are LSCI
scripts that `LSCITV.EXE` runs. A scratch C: drive is a copy of the installed client (`work/golf-c`).

From INN the land writes the shared block (user SID at +0, the game group's SID at +4, the name at +0x10, 14
launch bytes at +0x80: player count, holes, game type...), names `Golf` as the next program and exits; GOLF
reads the block, creates its object (joinNet kind 1, type 0x66), joins the group, asks for the members, waits
for every player's announcement and seeds its random numbers from the lowest SID's. `scenarios/inn_sierraland.txt`
(private) reaches it from the Waiting Room against `innkeeperd`.

## Floating point

The game's x87 code is linked as emulator interrupts (INT 34h-3Dh, 2581 of them). With a coprocessor present,
which dosrt has by default, the emulator rewrites each into FWAIT and the x87 instruction the first time it
runs, so the code in memory changes as the game plays ([runtime.md](runtime.md), "FPU").
`tools/borland_emu_patch.py` applies the same rewrite to a copy of the file for disassemblers.

## Running it alone

The TSN host stand-in ([runtime.md](runtime.md), "TSN host stand-in") with five reply rules plays the host:
joinNet answered with an ObjID, GrpJoin echoed, GrpMem with the player as the only member, and the position
multicast echoed back to its sender (the game sends the next update only after its own copy came back).

```
build/inn --headless --run-ms 60000 --tsn-host private/lifted/scenarios/golf_host.txt \
    --script play.txt work/golf-c 'INN\GOLF\GOLF.EXE'
```

The title and "getting player data..." take about 25 emulated seconds; the first tee shows at 30 s. Controls
seen so far: Space three times runs the swing meter (start, power, accuracy; about 2 s between the first two
for a full drive, 1 s for a putt); typing a letter opens the chat line, one key per 400 ms or so, Enter sends
it. A second rule file plays a scripted opponent for a two-player round. The interpreter runs the game at
about six times real speed natively.

## Verifying lifts

```
INN_DRIVE=$PWD/work/golf-c INN_TSN_HOST=$PWD/private/lifted/scenarios/golf_host.txt \
    INN_PROGRAM='INN\GOLF\GOLF.EXE' \
    tools/verify_lifted.sh build-<area> <port> 306000 100000 private/lifted/scenarios/golf_hole.txt
```

The lifting plan, census and the other scenarios are in the private tree (`private/lifted/GOLF-PLAN.md`).
