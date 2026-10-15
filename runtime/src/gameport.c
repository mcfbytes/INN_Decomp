/* Game port (201h) with no joystick attached: the one-shots never fire and no button is pressed,
 * so every read is FFh, as on a card with nothing plugged in. Red Baron polls it every frame. */
#include "devices.h"

enum { PORT_GAME = 0x201, NOTHING_ATTACHED = 0xFF };

static uint32_t gameport_in(Machine *m, uint16_t port, int size) { return NOTHING_ATTACHED; }
static void gameport_out(Machine *m, uint16_t port, uint32_t value, int size) {} /* fires the one-shots */

void gameport_init(Machine *m)
{
    port_register(m, PORT_GAME, PORT_GAME, gameport_in, gameport_out);
}
