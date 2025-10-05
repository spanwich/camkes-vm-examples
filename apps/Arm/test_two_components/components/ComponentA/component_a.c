/*
 * ComponentA - Test component with notification emit
 *
 * Stage 3: Write to dataport then emit notification to wake ComponentB
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>

int run(void)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  COMPONENT A: Started successfully!\n");
    printf("  COMPONENT A: run() function executing\n");
    printf("  COMPONENT A: Dataport address = %p\n", shared_buffer);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("\n");
    fflush(stdout);

    /* Give ComponentB time to print startup and enter wait loop */
    printf("ComponentA: Waiting 2 seconds for ComponentB to initialize...\n");
    fflush(stdout);
    for (volatile int j = 0; j < 200000000; j++);

    /* Now send messages with notifications */
    printf("ComponentA: Starting to send messages...\n");
    fflush(stdout);

    for (int i = 0; i < 5; i++) {
        /* Clear buffer */
        memset(shared_buffer, 0, 4096);

        /* Write message */
        snprintf((char *)shared_buffer, 4096, "Message %d from ComponentA", i);

        printf("ComponentA: [%d] Wrote to dataport: \"%s\"\n", i, (char *)shared_buffer);
        fflush(stdout);

        /* EMIT NOTIFICATION to wake ComponentB */
        data_ready_emit();
        printf("ComponentA: [%d] Emitted notification\n", i);
        fflush(stdout);

        /* Delay between messages */
        for (volatile int j = 0; j < 100000000; j++);
    }

    printf("ComponentA: Entering infinite periodic loop\n");
    fflush(stdout);

    /* Continue sending periodic messages */
    int counter = 5;
    while (1) {
        for (volatile int j = 0; j < 200000000; j++);

        memset(shared_buffer, 0, 4096);
        snprintf((char *)shared_buffer, 4096, "Periodic message %d from ComponentA", counter);

        printf("ComponentA: [%d] Wrote: \"%s\"\n", counter, (char *)shared_buffer);
        fflush(stdout);

        /* Emit notification */
        data_ready_emit();
        printf("ComponentA: [%d] Emitted notification\n", counter);
        fflush(stdout);

        counter++;
    }

    return 0;
}
