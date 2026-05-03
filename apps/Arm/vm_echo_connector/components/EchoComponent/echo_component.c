/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <camkes.h>

int run(void)
{
    printf("EchoComponent: Starting echo service\n");
    
    // Initialize the destination buffer
    memset(dest, '\0', 4096);
    strcpy(dest, "Echo service ready. Send data to be echoed back.\n");

    while (1) {
        // Wait for VM to send data
        ready_wait();
        printf("EchoComponent: Received event from VM\n");
        
        // Read data from src buffer (VM writes here)
        if (strlen((char*)src) > 0) {
            printf("EchoComponent: Echoing data: %s", (char*)src);
            
            // Echo the data back to dest buffer (VM reads from here)
            memset(dest, '\0', 4096);
            snprintf((char*)dest, 4096, "ECHO: %s", (char*)src);
            
            // Clear src buffer for next message
            memset(src, '\0', 4096);
        } else {
            printf("EchoComponent: No data received\n");
            strcpy((char*)dest, "Echo service: No data received\n");
        }
        
        // Notify VM that echo processing is complete
        done_emit_underlying();
    }

    return 0;
}
