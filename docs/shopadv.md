# Shoppers Advantage in dosrt

How INN starts Shoppers Advantage (`SHOPADV.EXE`), what it needs from the host, and how it runs alone
under dosrt for verifying lifts (milestone M8). Message layouts and the session flow are in the sibling
project's `docs/protocol/int14h-census/shopadv.md`; this page describes the run.

## Files

`TSN.PRG` block `ShopAdv`: `cd shopadv`, `shopadv.exe` (no arguments). The directory `\INN\SHOPADV` holds
the LZEXE-packed `SHOPADV.EXE`, the background `CUCBG.PCX`, the frame bitmaps `MARBLE.1` and `MARBLE.2` and
the font `SYSTEM.FNT` (the install media keep them compressed; the sibling's `work/games/SHOPADV` has them
expanded). The program writes `\INN\SHOPADV\CAPTURES.TXT` when the user captures text.

The program is Borland C++ 3.1, large model ([compilers.md](compilers.md)), drawn with the Fastgraph V3.00
library in VGA mode 12h (640x480, 16 colours). No floating point, no sound, no overlays. It hooks INT 1Ch to
run TSNEXEC's Service at every timer tick and installs an INT 33h event handler.

A scratch C: drive is the installed client plus `\INN\SHOPADV` (`work/shop-c`).

## Boot path from INN

The hub's mall door (`gotoCUCmall`) sends the mall gate request (command 54, sub 27). The host answers
"open" (54/19), and the land runs `ShopAdv` through TSNEXEC, or "unavailable" (54/20, with a text). The
program then keeps the X.25 session TSNEXEC already has: it never reads the shared block, joins the mall
gateway (54/25 → 54/17 with a three-word token), and is a text terminal for it: the host sends screen text
(54/16 kind 1) or a notice, the client sends each typed line (54/24) and EXIT (54/26) when the user leaves.
Exiting returns to the program TSNEXEC names as the previous one (the hub). `innkeeperd` answers all of it
with a closed mall: one line of text and the exit acknowledgement (sibling `crates/innkeeper-world/src/mall.rs`).

## Running it alone

`--tsn-host` replaces TSNEXEC with dosrt's stand-in ([runtime.md](runtime.md), "TSN host stand-in"). The
private tree has four rule files that play the mall gateway (`private/lifted/scenarios/shop_host*.txt`):
a mall with two departments that answers browsing, an order and the exit, and three error paths (the mall
goes down, the join is refused, the gateway stops answering).

```
build/inn --headless --run-ms 42000 --tsn-host private/lifted/scenarios/shop_host.txt \
    --script play.txt work/shop-c 'INN\SHOPADV\SHOPADV.EXE'
```

The welcome dialog closes with Esc; the Exit and File buttons sit at (295, 456) and (350, 456) in screen
coordinates. Dialog positions depend on the clock, so scripted clicks into dialogs need a fixed `INN_EPOCH`
(`tools/verify_lifted.sh` sets one).

The mouse driver keeps the virtual screen of the current mode (480 lines in mode 12h), as a driver that
follows INT 10h mode sets does: Fastgraph resets the driver in text mode and only then sets mode 12h.

## Verifying lifts

`tools/verify_lifted.sh` runs the program alone against a rule file, as for Red Baron
([redbaron.md](redbaron.md)):

```
INN_DRIVE=$PWD/work/shop-c INN_PROGRAM='INN\SHOPADV\SHOPADV.EXE' \
    INN_TSN_HOST=$PWD/private/lifted/scenarios/shop_host.txt \
    tools/verify_lifted.sh build-<area> <port> 42000 20 private/lifted/scenarios/shop_browse.txt
```

The private `SHOPADV-PLAN.md` lists the four scenario pairs; each run ends with the program exiting with
code 0.
