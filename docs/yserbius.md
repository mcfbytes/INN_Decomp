# Shadow of Yserbius and Fates of Twinion in dosrt

How INN starts the two Ybarra games, what each step needs from the host, and how far they run under
dosrt (milestone M7). Message layouts are in the sibling project's
`docs/protocol/int14h-census/yserbius.md` and `twinion.md`; this page describes the run.

## Files

| Game | Directory (from `LAND.CFG`) | Land type | Programs (`TSN.PRG` block) |
|---|---|---|---|
| The Shadow of Yserbius | `\INN\YSERBIUS` | 4 | `Yserbius` = `cd YSERBIUS`, `cgenn.exe` (character gallery); `Yserba` = `darkstrt.exe` (the game) |
| The Fates of Twinion | `\INN\TWINION` | 8 | `Twinion` = `twgenn.exe`; `Twina` = `fates.exe` |

All four are Borland C++ large-model programs. `DARKSTRT.EXE` and `FATES.EXE` carry Borland overlays
(an `FBOV` trailer, 51 overlays each, loaded through INT 3Fh); the two gallery programs have none. Music
is Miles AIL with XMIDI files and the `ADLIB.ADV` driver (`SBFM.ADV`, `MT32MPU.ADV` for other cards).

A scratch C: drive is the installed client with the game directory copied under `\INN`
(`work/yserbius-c`). Neither game needs EMS, XMS or an x87.

## Boot path from INN

1. **Map.** On the town map, MedievaLand is the place after the Clubhouse in Tab order. It asks
   "Which land do you want to go to?" with the two games.
2. **Access check** (hub `script.050`). The land is allowed when bit `landType - 1` is set in the word
   at byte 8 of the logon Ack (the sibling docs call it `rating`; 0 is stored as 1, Clubhouse only).
   Yserbius needs bit 3 (`0x08`), Twinion bit 7 (`0x80`). Otherwise: "Sorry, but you do not have access
   rights for The Shadow of Yserbius."
3. **Place list.** The land directory the host sends at logon must have rows for land type 4 (and 8),
   or the dialog says "There are no places available right now for this land." The client caches the
   directory in `LANDADDR` with the stamp in `LANDADDR.TIM`; a host that adds rows must change its stamp.
4. **Hand-off.** Choosing a place frees the game object, LSCITV writes the shared block, registers the
   next `TSN.PRG` block and exits; TSNEXEC runs `cgenn.exe` in `\INN\YSERBIUS` with the connection up.
5. **Gallery (`CGENN`).** Finds TSNEXEC through INT 14h, reads the shared block (empty: "Application
   Orphaned. Press Any Key To Exit"), sends joinNet kind 1 (land type `0x8B`) and the service lookup
   41/2. The screen stays black until a command 41 reply arrives (the client resends every 60 s).
   Then the character gallery appears. Play registers `Yserba` and exits.
6. **Game (`DARKSTRT`).** The overlay manager opens the program's own EXE and keeps it open; the game
   reads `..\LSCI.CFG`, loads the AIL driver, `MYCHARS.DAT`, `IMAGES.DAT`, `PALETTE.DAT`, `ANIM_SEQ`,
   XMIDI music, hooks INT 8, and runs the join sequence (41/2, land group, GrpJoin with version 1.1.7,
   personal party, name, map group, party roster to the map). The town screen follows; entering the
   volcano loads `WALLS.DAT`, `RESOURCE.0xx` and `AUTOMAPS.DAT` and shows the first-person dungeon.

Run alone (`inn work/yserbius-c 'INN\YSERBIUS\CGENN.EXE'`), `CGENN` takes the BIOS's INT 14h status as
TSNEXEC's export table and jumps to 0000:0000. Started through `INN\TSNEXEC.EXE Yserbius` without a
host it reaches its first screen, the "Application Orphaned" message.

## Status (2026-10-14)

From the INN map, `CGENN` and `DARKSTRT` run under the interpreter against `innkeeperd` with three host
additions (access bit, directory rows, the 41 reply): the gallery, Play, the town and the dungeon with
walls, movement and AdLib music. Twinion takes the same path (second choice in the land dialog):
`TWGENN`'s gallery, then `FATES` to its town screen. No runtime gaps showed up: no unhandled DOS or BIOS calls, no invalid
opcodes. The overlay manager behaves like a program reading its own file, which dosrt already served;
lifted overlay code is armed per overlay as it is read ([lifting.md](lifting.md), "Overlays").

## Host requirements found on the way

| Step | Host must | Without it |
|---|---|---|
| 2 | set bit 3 (Yserbius) / bit 7 (Twinion) of the logon Ack word at byte 8 | "no access rights" |
| 3 | list land types 4 and 8 in the land directory, with a new stamp | "no places available" |
| 5, 6 | answer 41/2 with command 41, the service SID in the word at byte 6 | black screen forever |
| 6 | joinNet kinds 5, 4, 2 with ObjID, GrpJoin echo for self | "Unable To Join Land!" and similar |
