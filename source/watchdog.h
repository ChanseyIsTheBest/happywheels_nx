/* Stall watchdog: makes a hang report itself. See watchdog.c. */

#ifndef WATCHDOG_H
#define WATCHDOG_H

void watchdog_start(void);
void watchdog_stop(void);

/* Called once per rendered frame from the main loop. */
void watchdog_frame(unsigned frame);

/* Leaves a short note about what is happening now. If progress stops, the
 * watchdog reports the last one, which is usually enough to place the hang.
 * Cheap, but not free -- do not call per-frame on the happy path. */
void watchdog_mark(const char *what);

#endif
