/* match: CGENN 08A6:000C */
void far orig(void far *p, unsigned v) { *(unsigned far *)((char far *)p + 0x48) = v; }
