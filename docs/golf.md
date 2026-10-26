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

More controls, from the scenarios `golf_holes`, `golf_water` and `golf_depart` (private tree):

- **Swing.** Space (set-1 scan code 39) or a mouse click in the 3-D view starts the meter; the second press
  sets the power (a full drive peaks about 3 s after the first press, a putt of 60 ft about 2 s, a 6 ft putt
  0.8 s) and the third the accuracy, best about 0.8 s after the power press. A press 6 s late or early only
  bends the shot a little. Keys pressed while the ball still flies or the result bar is up are lost, so a
  scenario waits about 18 s after a full shot, 26 s after a chip, 16 s after a long putt and 5 s after a
  penalty drop before the next swing.
- **Aim.** The left and right arrows (4B, 4D) turn the aim marker only while they are held (`down 4B`, then
  `up 4B` later) at about 45 pixels per second; the aim stays where it was left. Up (48) changes the club.
- **Scorecard.** It comes up after the cup and closes itself about 22 s later; keys and clicks do not close it.
- **Starting hole.** Launch byte 5 picks it: 0 and 1 start on hole 1, 2 (back nine) on hole 10.
- **Hazards.** Hole 10 has a lake 50 yd from the tee: a drive pressed at 1 s lands in it, the game adds the
  stroke, shows "DROPPING THE BALL" and puts the ball back at the tee side. Holes 4 and 5 also show water in the course view. An
  aim turned 6 s left on the hole 2 tee gives the same penalty drop.
- **Departures.** A reply rule that matches one of our messages can inject a group event: a chat line
  (message type `cd`) answered with `0b 00 00 03 02 06` is a GrpDel for the opponent, so `gcApplyDeparture`
  prints "NICKLAUS has left the game" and, in match play, "Switching to Stroke Play".

Launch variants are rule files that change the launch bytes of `golf_host.txt` (byte 5 holes, byte 7 game
type: 0 Stroke Play, 1 Match Play, 2 Match Play vs. Par, 3 Straight Skins, 4 Carryover Skins, 5 Drop Out,
6 Shoot Out, 7 Bingo-Bango-Bongo, then Best Ball, Alternate, Aggregate, Straight Scramble, Drop Out Scramble).

## Verifying lifts

```
INN_DRIVE=$PWD/work/golf-c INN_TSN_HOST=$PWD/private/lifted/scenarios/golf_host.txt \
    INN_PROGRAM='INN\GOLF\GOLF.EXE' \
    tools/verify_lifted.sh build-<area> <port> 306000 100000 private/lifted/scenarios/golf_hole.txt
```

The lifting plan, census and the other scenarios are in the private tree (`private/lifted/GOLF-PLAN.md`).
