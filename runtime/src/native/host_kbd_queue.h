/* Native-mode scancode queue, stored in the 8042 block so it dies with kbd_init's state. */
#ifndef DOSRT_HOST_KBD_QUEUE_H
#define DOSRT_HOST_KBD_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

enum { HOST_KBD_QUEUE = 64 };

typedef struct HostKbdQueue {
    uint8_t bytes[HOST_KBD_QUEUE];
    int head, count;
    uint8_t latched;
    bool has_latch;
    bool latch_read;
    bool saw_service; /* IRQ 1 was in service for this byte; ISR clear before that is not an EOI */
} HostKbdQueue;

/* NULL before kbd_init. */
HostKbdQueue *kbd_host_queue(Machine *m);

#endif
