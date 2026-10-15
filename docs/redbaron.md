# Multi-Player Red Baron in dosrt

How INN starts Dynamix's Red Baron (INN conversion, `RB.EXE`), what it needs from the host, and how
far it runs under dosrt (milestone M8). Message layouts are in the sibling project's
`docs/protocol/int14h-census/redbaron.md`; this page describes the run.

## Files

`TSN.PRG` block `RedBaron`: `cd BARON`, `rb.exe -C..\LSCI.CFG`. The directory `\INN\BARON` holds the
LZEXE-packed `RB.EXE`, `SIM.IN`, `SIMVOLS.MAP` and the Dynamix archive `VOLUME.RMF` + `VOLUME.001`-`003`.
Everything else (scenery, aircraft, fonts, palettes, sounds, and two code overlays `vmcode2.ovl` for
video and `sxcode2.ovl` for sound) is read out of the volumes. `-C` names `LSCI.CFG` only for its
`music =` line.

The program is Borland C with fixed-point math: **no x87 instructions, no floating point**. It
reprograms PIT channel 0 to 236.7 Hz, masks PIC lines, hooks INT 8, 9, 10h and 1Ch, programs the DAC,
CRTC and graphics controller directly and polls the game port (201h) every frame.

A scratch C: drive is the installed client plus `\INN\BARON` (`work/baron-c`).

## Boot path from INN

Red Baron belongs to SierraLand (land type 2), not to a land of its own. Found in the SierraLand
scripts (`SL/script.001`, `120`, `905`, `908`, `101`; by a delegated read of the bytecode, not yet seen
live):

1. **Player.** SierraLand needs a player record (Create: name, age, city, face, game skills,
   interests; saved as `SL\<name>.DTA` and `SL\PLAYER.DIR`). Play then asks which games the player is
   interested in and enters the Waiting Room.
2. **Choosing the game.** The Waiting Room's Play dialog lists only games that pass three filters: the
   game is named in `GAME.CFG`, and with nobody highlighted only games with a minimum of one player
   appear. Red Baron needs 2 to 4 players, so it is offered only after highlighting the other players
   in the room. Two catalogue entries exist (client version 3 or later, and an older one).
3. **Invitation room** (`script.905 rbInviteRoom`, "Red Baron Invitation"): sides and planes are
   chosen, a lock is taken on each invited player (the host must grant it), and Send messages (app
   types 56, 61, 63, 999) go between the players' clients.
4. **Queue** (`script.120 RedBaronQueue`, a `QueuedGroup`): a queue object and its `GameGroup`; the
   host must answer the joinNet with ObjID ("For some reason the host is not authorizing this game"
   otherwise), and forward GrpJoin, GrpDel and GrpMem for the group.
5. **Hand-off** (`runRedBaronScript`, then `script.101` export 1): the land writes the shared block
   (user id, the game group's SID at `+4`, name at `+0x10`, the 15 mission bytes at `+0x80`: player
   count, side, skill, the two planes, date and time-of-day modes, options), calls SetNextProgram
   `RedBaron` and exits; TSNEXEC runs `rb.exe`.
6. **Game.** `RB.EXE` calls GetPreviousProgram and makes it the next program (exiting returns to
   SierraLand), reads the block, then runs `initNet` and `joinGame` against the host (below), loads
   the scenery and aircraft, and flies.

Run alone (`inn work/baron-c 'INN\BARON\RB.EXE' '-C..\LSCI.CFG'`), RB takes the BIOS's INT 14h answer
as TSNEXEC's table and jumps to 0000:0000. Started through `INN\TSNEXEC.EXE RedBaron` without a land
it prints "Parent failure: application has been orphaned." and exits 1.

### Against innkeeperd (2026-10-14)

From INN under dosrt: logon, the map, SierraLand, creating a player, the games-of-interest dialog and
the Waiting Room all work. With one client the Play dialog offers 3-D Golf, MiniGolf, Stratego,
Graffiti and NTN Trivia, as the filters above predict. A two-client run was not attempted: the host
does not implement the Red Baron invitation and queue (sibling `docs/server/fanout-plan.md` D1, D3).

## Running Red Baron alone

`--tsn-host` replaces TSNEXEC with dosrt's stand-in and a rule file that plays the host and a second
player ([runtime.md](runtime.md), "TSN host stand-in"):

```
build/inn --headless --run-ms 30000 --tsn-host private/lifted/scenarios/rb_host.txt \
    --script play.txt work/baron-c 'INN\BARON\RB.EXE' '-C..\LSCI.CFG'
```

The rules write the shared block (two players, a Sopwith Pup against a Spad 7), answer the service
lookup (41/2), properties 9 (`0x0201`, the version check) and 0xF (game length), host time, the
player object's joinNet, GrpJoin (the echo plus a second member) and GrpMem, send that member's player
info (0xC8), and echo each 0xC9 state multicast back to the sender. RB then runs its whole start-up
and flies: cockpit view, gauges, "Time Left" counting down in real time on the 236.7 Hz clock,
messages ("You sense an enemy to the west"), keyboard flight controls, engine sound. The interpreter
runs it at about four times real speed natively. Nothing unhandled shows up: no unknown DOS or BIOS
call, no invalid opcode, no unhandled port; the only device it needed that dosrt did not model
explicitly was the game port, which now reports nothing attached (FFh).

## Host requirements

`innkeeperd` (2026-10-14) does none of the Red Baron-specific steps. A host needs, in order:

| Step | Host must | Without it |
|---|---|---|
| Land, invitation | grant the invitation locks (cmd 4 + Ack), relay the invitation Sends between players | the invitation never completes |
| Land, queue | answer the queue's joinNet (kind 4, type 101) with ObjID; GrpJoin/GrpDel/GrpMem for the game group; let the land form the group of 2-4 players and free it afterwards | "the host is not authorizing this game" |
| RB `initNet` | answer 41/2 type 101 with cmd 41 (service SID at w@6) | "get perm obj timeout" after 119.6 s (fatal) |
| | answer 17/0 prop 9 with setInt `0x0201`, prop 0xF with setInt 5..30 (minutes), each with its own reply whose first pair is the requested prop | "get group stats timeout"; a wrong prop 9 gives "incompatible version" |
| | answer 36/2 with the host date and time | "get host time timeout" |
| RB `joinGame` | joinNet kind 3 type 101 → ObjID; GrpJoin echo to every member including the joiner; GrpMem with all members | "create object timeout", "join group timeout", "get group mem timeout" |
| In game | relay Send (0xC8) to the other members with the real `fromSid`; split multicast 28 per recipient including the sender's own copy (0xC9 echo gates the next update); GrpDel to the others when a player leaves or drops; absorb 50 | no second plane, frozen updates, ghost aircraft |

The current server already routes Send and multicast (router C3) and answers host time 36/2; the service object (41/2 type 101 → `None` in `rpg.rs`), its two
properties and the land-side queue are missing. Golf's needs are similar (joinNet kind 1 type 0x66,
GrpJoin echo, GrpMem); dosrt's GOLF reaches the first tee against a five-rule stand-in.

## Status

RB boots and flies under the interpreter against the stand-in host; from INN it stops where the
server stops (no Red Baron service or queue). Lifting plan: `private/lifted/BARON-PLAN.md`.
