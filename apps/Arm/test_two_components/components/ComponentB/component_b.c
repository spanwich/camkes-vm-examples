/*
 * ComponentB - Test component with notification wait
 *
 * Stage 3: CRITICAL TEST - Does startup message appear BEFORE blocking on wait()?
 *
 * This component:
 * 1. Prints startup banner (with fflush)
 * 2. Blocks on data_ready_wait() waiting for notification
 * 3. When notified, reads from dataport and processes
 *
 * If startup banner appears immediately after boot, run() was called.
 * If startup banner only appears after first notification, run() is blocked.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>

int run(void)
{
    /* CRITICAL: Print startup banner BEFORE blocking */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  COMPONENT B: Started successfully!                     ║\n");
    printf("║  COMPONENT B: run() function executing                  ║\n");
    printf("║  COMPONENT B: Dataport address = %p                     ║\n", shared_buffer);
    printf("║  COMPONENT B: About to block on data_ready_wait()      ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    fflush(stdout);

    /* Now enter event loop - blocks waiting for notifications */
    printf("ComponentB: Entering event loop (will block on wait)...\n");
    fflush(stdout);

    int count = 0;
    while (1) {
        /* BLOCKING CALL - waits for ComponentA to emit notification */
        printf("ComponentB: [%d] Waiting for notification...\n", count);
        fflush(stdout);

        data_ready_wait();

        /* Notification received - read from dataport */
        char *msg = (char *)shared_buffer;
        printf("ComponentB: [%d] Notification received! Read: \"%s\"\n", count, msg);
        fflush(stdout);

        count++;
    }

    return 0;
}
