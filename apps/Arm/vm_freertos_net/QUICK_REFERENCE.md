# NetworkFilter POC - Quick Reference Card

## Build & Run (One-liner)

```bash
cd ~/phd/camkes-vm-examples && rm -rf build && mkdir build && cd build && \
env PYTHONPATH=../projects/camkes-tool:../projects/capdl/python-capdl-tool \
cmake -G Ninja -DCAMKES_VM_APP=vm_freertos_net -DPLATFORM=qemu-arm-virt \
-DSIMULATION=1 -DLibUSB=OFF -DSEL4_CACHE_DIR="../.sel4_cache" \
-C "../projects/vm-examples/settings.cmake" "../projects/vm-examples" && \
ninja && ./simulate
```

## File Locations

| File | Purpose |
|------|---------|
| `components/NetworkFilter/network_filter.c` | Main filter logic |
| `vm_freertos_net.camkes` | CAmkES assembly |
| `CMakeLists.txt` | Build configuration |
| `README_NETWORK_FILTER.md` | Full documentation |

## Key Code Sections

### Add Filter Rule
**File**: `components/NetworkFilter/network_filter.c`
**Function**: `apply_filter_rules()`
```c
// Example: Block HTTP traffic
if (ip->protocol == IPPROTO_TCP) {
    struct tcphdr *tcp = ...;
    if (ntohs(tcp->dest) == 80) {
        return FILTER_DROP;
    }
}
```

### Modify VirtQueue Buffer Size
**File**: `vm_freertos_net.camkes`
```c
vm0.recv_shmem_size = 65536;  // 32KB → 64KB
vm0.send_shmem_size = 65536;
network_filter.vm_recv_shmem_size = 65536;
network_filter.vm_send_shmem_size = 65536;
```

### Enable/Disable Verbose Logging
**File**: `components/NetworkFilter/network_filter.c`
```c
#define FILTER_VERBOSE 0  // Set to 0 for quiet mode
```

## Configuration

### MAC Addresses
- VM: `02:00:00:00:AA:01`
- NetworkFilter: `02:00:00:00:AA:02`

### VirtQueue IDs
- VM recv: 0, send: 1
- NetworkFilter recv: 0, send: 1

### Shared Memory
- 32KB per virtqueue (configurable)

## Common Tasks

### Add lwIP Integration
```c
// In network_filter.c run()
init_lwip_stack();  // Initialize TCP/IP

// In handle_packet_from_vm()
feed_packet_to_lwip(packet, packet_size);
```

### Add External Network
```c
// In vm_freertos_net.camkes
component NetworkFilter {
    dataport Buf(4096) external_rx;
    dataport Buf(4096) external_tx;
};
```

### Monitor Statistics
Check the `stats` struct:
```c
printf("Total packets: %lu\n", stats.packets_from_vm);
printf("Dropped: %lu\n", stats.packets_dropped);
```

## Debugging

### Enable Debug Output
```bash
# In CMakeLists.txt
set(CMAKE_BUILD_TYPE Debug)
```

### Check VirtQueue Status
Look for these log messages:
```
✓ Recv virtqueue initialized (ID=0)
✓ Send virtqueue initialized (ID=1)
```

### Packet Not Received?
1. Verify FreeRTOS has virtio-net driver
2. Check MAC addresses match
3. Confirm VirtQueue IDs are correct

## Performance Tuning

### Reduce Logging Overhead
```c
#define FILTER_VERBOSE 0
```

### Increase Buffer Size
```c
vm0.recv_shmem_size = 131072;  // 128KB
```

### Batch Processing
```c
// Process multiple packets before notify
while (more_packets) {
    process_packet();
}
vm_send_virtqueue.notify();  // Single notify
```

## Research Extensions

### Multi-VM Setup
```c
// Add VM 1
VM_COMPOSITION_DEF(1)
connection seL4VirtQueues net_virtq_vm1(
    from network_filter.vm1_recv,
    from vm1.send
);
```

### IDS Integration
```c
typedef struct {
    char signature[256];
    filter_action_t action;
} ids_rule_t;

ids_rule_t rules[] = {
    {"malware_pattern", FILTER_DROP},
    ...
};
```

### Rate Limiting
```c
#define MAX_RATE_PPS 1000  // packets per second
uint64_t last_time = 0;
uint64_t packet_count = 0;

if (get_time() - last_time > 1000000) {
    packet_count = 0;
    last_time = get_time();
}
if (++packet_count > MAX_RATE_PPS) {
    return FILTER_DROP;
}
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Build fails | Check `PYTHONPATH` is set |
| Component not found | Verify `DeclareCAmkESComponent` in CMakeLists.txt |
| VirtQueue init fails | Check IDs match in assembly |
| No packets received | Verify FreeRTOS has virtio driver |
| Packets dropped unexpectedly | Review filter rules in `apply_filter_rules()` |

## Quick Tests

### Test 1: Build
```bash
ninja | grep -i "networkfilter\|error"
```

### Test 2: Boot
```bash
./simulate | grep -i "networkfilter"
```

### Test 3: Packet Flow
```bash
# Look for these in output:
# "← Received X bytes from VM"
# "✓ ALLOW - Forwarding"
```

## Documentation

- **Full Guide**: README_NETWORK_FILTER.md
- **POC Summary**: NETWORK_FILTER_POC_SUMMARY.md
- **Architecture**: ~/phd/research-docs/sddf-camkes-bridge-component-approach.md

---

**Quick Start**: Build → Run → Check "NetworkFilter is READY" → Test with FreeRTOS
