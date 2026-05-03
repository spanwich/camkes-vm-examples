# test_two_components - Minimal CAmkES Concurrent Component Test

## Purpose

This application proves that **two pure CAmkES components with `control;` attribute start concurrently without any special initialization**.

Created to debug vm_ethernet_echo where EchoComponent wasn't starting.

## Test Results

### Stage 1: No Connections ✅ **SUCCESS**

Both components started and ran concurrently without any connections.

### Stage 2: With Dataport Connection ✅ **SUCCESS**

**FINDING**: Adding dataport connection does NOT block component startup!

Both components printed startup messages IMMEDIATELY, proving `run()` was called at boot.

### Stage 3: With Notification Connection ✅ **CRITICAL BREAKTHROUGH**

**🎯 CRITICAL FINDING**: Notification wait() does NOT prevent startup message!

ComponentB's startup banner appeared IMMEDIATELY after boot, BEFORE blocking on `data_ready_wait()`!

#### Stage 3 Output (Notification Wait Test)
```
Booting all finished, dropped to user space

═══════════════════════════════════════════════════════════
  COMPONENT A: Started successfully!
  COMPONENT A: run() function executing
  COMPONENT A: Dataport address = 0x6a000
═══════════════════════════════════════════════════════════

ComponentA: Waiting 2 seconds for ComponentB to initialize...

╔══════════════════════════════════════════════════════════╗
║  COMPONENT B: Started successfully!                     ║  <- APPEARS IMMEDIATELY!
║  COMPONENT B: run() function executing                  ║
║  COMPONENT B: Dataport address = 0x78000                ║
║  COMPONENT B: About to block on data_ready_wait()      ║  <- BEFORE BLOCKING!
╚══════════════════════════════════════════════════════════╝

ComponentB: Entering event loop (will block on wait)...
ComponentB: [0] Waiting for notification...                 <- NOW BLOCKED
ComponentA: Starting to send messages...
ComponentA: [0] Wrote to dataport: "Message 0 from ComponentA"
ComponentA: [0] Emitted notification                        <- WAKE COMPONENTB
ComponentB: [0] Notification received! Read: "Message 0 from ComponentA"
ComponentB: [1] Waiting for notification...
... (continues successfully)
```

**Key Observations**:
- ✅ ComponentB startup banner appears IMMEDIATELY after boot
- ✅ ComponentB prints "About to block on data_ready_wait()" BEFORE calling wait()
- ✅ ComponentB enters wait loop and blocks
- ✅ When ComponentA emits notification, ComponentB wakes and processes
- ✅ **PROOF**: run() is called at startup even when component will block on wait()
- ✅ Notification-based event loop works perfectly

## Key Learnings

1. **No special initialization required** - Components with `control;` start automatically
2. **Dataports do NOT block startup** - Components print messages before touching dataport
3. **Notifications do NOT block startup** - Component prints banner BEFORE calling wait()
4. **No VM infrastructure needed** - Pure CAmkES components work fine
5. **No cross_vm_connections.c equivalent** - That's VM-specific
6. **Concurrent execution works** - Both components run with equal priority (100)
7. **Printf works immediately** - Output appears at startup without buffering issues
8. **Notification event loops work perfectly** - wait()/emit() pattern is standard CAmkES

## Architecture

### Stage 2 Configuration (Current)

```camkes
component ComponentA {
    control;
    dataport Buf(4096) shared_buffer;  /* Write to shared memory */
}

component ComponentB {
    control;
    dataport Buf(4096) shared_buffer;  /* Read from shared memory */
}

assembly {
    composition {
        component ComponentA comp_a;
        component ComponentB comp_b;

        /* Shared memory connection */
        connection seL4SharedData data_conn(from comp_a.shared_buffer,
                                             to comp_b.shared_buffer);
    }
    configuration {
        comp_a.priority = 100;
        comp_b.priority = 100;
    }
}
```

**Components communicate via zero-copy shared memory (dataport)**

### Component Implementations

- **ComponentA**: Writes messages to `shared_buffer` dataport
- **ComponentB**: Reads messages from `shared_buffer` dataport
- Both print startup banner immediately in `run()` before touching dataport

## Implications for vm_ethernet_echo

**🚨 CRITICAL BREAKTHROUGH**: All progressive tests PASSED!

The problem in vm_ethernet_echo is **NOT** caused by:

1. ❌ CAmkES initialization (proven: Stage 1)
2. ❌ Dataport connections (proven: Stage 2)
3. ❌ Notification wait() blocking (proven: Stage 3)
4. ❌ Missing cross_vm_connections.c (VM-specific, not needed)
5. ❌ Printf buffering (fflush works correctly)

**So what IS blocking EchoComponent?**

Since test_two_components proves:
- ✅ Components with dataports + notifications print startup messages immediately
- ✅ Blocking on notification_wait() doesn't prevent startup message
- ✅ Event-driven notification pattern works perfectly

**The problem must be in vm_ethernet_echo's SPECIFIC CODE**:

Possible causes to investigate:
1. **EchoComponent's run() is never called** - Check .camkes file has `control;`
2. **Printf is going to wrong output** - Check stdout configuration
3. **Component priority issue** - Check if EchoComponent has lower priority
4. **Infinite loop before printf** - Check run() for blocking code before printf
5. **Component not instantiated** - Check .camkes assembly actually creates it

**Next action**: Go back to vm_ethernet_echo and check EchoComponent's actual run() implementation and .camkes configuration

## Build Instructions

```bash
cd /home/konton-otome/phd/camkes-vm-examples
mkdir -p build-test-two-components && cd build-test-two-components

env PYTHONPATH=../projects/camkes-tool:../projects/capdl/python-capdl-tool \
    cmake -G Ninja \
    -DCAMKES_VM_APP=test_two_components \
    -DPLATFORM=qemu-arm-virt \
    -DSIMULATION=1 \
    -DLibUSB=OFF \
    -DSEL4_CACHE_DIR="../.sel4_cache" \
    -C ../projects/vm-examples/settings.cmake \
    ../projects/vm-examples

env PYTHONPATH=../projects/camkes-tool:../projects/capdl/python-capdl-tool ninja

./simulate
```

## Files

- `test_two_components.camkes` - Main assembly
- `components/ComponentA/component_a.c` - Component A implementation
- `components/ComponentB/component_b.c` - Component B implementation
- `CMakeLists.txt` - Build configuration
- `settings.cmake` - Minimal settings file
- `README.md` - This file

## Date Created

2025-10-05
