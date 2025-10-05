# Implementation Roadmap: Incremental Development

## Philosophy: Prove → Build → Extend

Start simple, validate each stage, then add complexity. Each stage builds on the previous one and can be tested independently.

---

## Stage 1: Single NIC Echo via Dataport (PROOF OF CONCEPT)

**Goal**: Prove that **dataport-based packet transfer works** for network data

**Status**: 🎯 **START HERE**

### Architecture

```
External Client                    seL4 System
(netcat)

nc localhost 6000  →  [EthernetDriver + lwIP]  →  [EchoComponent]
                           ↓                             ↓
                      TCP server                    Read from
                      port 1234                     dataport
                           ↓                             ↓
                      Extract payload              Add "ECHO: "
                           ↓                             ↓
                      Write to dataport            Write to
                      (rx_packet_buffer)           dataport
                           ↓                             ↓
                      Signal component             Signal driver
                      (rx_ready_emit)              (tx_done_emit)
                           ↓                             ↓
                      Wait for response
                      (tx_done_wait)
                           ↓
                      Read from dataport
                      (tx_packet_buffer)
                           ↓
                      Send via lwIP
                      (same connection)
                           ↓
                      Client receives:
                      "ECHO: Hello\n"
```

### Components

**1. EthernetDriver** (Modified from vm_freertos_net)
- VirtIO-Net device + lwIP TCP/IP stack
- TCP server on port 1234
- Receives netcat connection
- **NEW**: Instead of echoing directly, forwards to EchoComponent via dataport

**2. EchoComponent** (New, simple)
- Waits for rx_ready notification
- Reads TCP payload from rx_packet_buffer
- Adds "ECHO: " prefix
- Writes to tx_packet_buffer
- Signals tx_done

### Dataports (Zero-Copy)

```c
// Shared between EthernetDriver and EchoComponent
rx_packet_buffer[2048]  // Driver writes, Echo reads
tx_packet_buffer[2048]  // Echo writes, Driver reads
```

### Testing

```bash
# Start system
cd build-ethernet-echo-stage1
./simulate --extra-qemu-args="-global virtio-mmio.force-legacy=false \
    -netdev user,id=net0,hostfwd=tcp::6000-10.0.2.15:1234 \
    -device virtio-net-device,netdev=net0"

# Test from host
echo "Hello World" | nc localhost 6000

# Expected output:
ECHO: Hello World
```

### Success Criteria

- ✅ Netcat connects successfully
- ✅ Data appears in EchoComponent (verify with printf)
- ✅ Echo response received by netcat client
- ✅ Zero-copy verified (no memcpy between components, only at dataport boundaries)

### Implementation Estimate

**Time**: 2-3 hours

**Files to modify**:
- `ethernet_driver.c` - Add dataport forwarding in tcp_echo_recv()
- `echo_component.c` - Create simple echo logic
- `vm_ethernet_echo.camkes` - Already created
- `CMakeLists.txt` - Build configuration

---

## Stage 2: Add Packet Sanitization

**Goal**: Demonstrate **security filtering** in isolated component

**Prerequisites**: Stage 1 complete

### New Features

**EchoComponent becomes FilterComponent**:

```c
int run(void) {
    while (1) {
        rx_ready_wait();

        char *input = (char *)rx_packet_buffer;
        size_t len = strlen(input);

        // SANITIZATION CHECKS
        bool is_safe = true;

        // 1. Check for SQL injection
        if (strstr(input, "DROP TABLE") ||
            strstr(input, "'; --")) {
            printf("BLOCKED: SQL injection attempt\n");
            strcpy(tx_packet_buffer, "ERROR: Malicious pattern detected\n");
            is_safe = false;
        }

        // 2. Check length (prevent buffer overflow)
        if (len > 1024) {
            printf("BLOCKED: Payload too large\n");
            strcpy(tx_packet_buffer, "ERROR: Payload too large\n");
            is_safe = false;
        }

        // 3. Check for invalid characters
        for (size_t i = 0; i < len; i++) {
            if (input[i] < 0x20 && input[i] != '\n' && input[i] != '\r') {
                printf("BLOCKED: Invalid character 0x%02x\n", input[i]);
                strcpy(tx_packet_buffer, "ERROR: Invalid characters\n");
                is_safe = false;
                break;
            }
        }

        if (is_safe) {
            // Safe data - add echo prefix
            snprintf(tx_packet_buffer, 2048, "ECHO: %s", input);
        }

        tx_done_emit();
    }
}
```

### Testing

```bash
# Test 1: Normal data (should pass)
echo "Hello World" | nc localhost 6000
# Expected: ECHO: Hello World

# Test 2: SQL injection (should block)
echo "'; DROP TABLE users; --" | nc localhost 6000
# Expected: ERROR: Malicious pattern detected

# Test 3: Oversized payload (should block)
python3 -c "print('A' * 2000)" | nc localhost 6000
# Expected: ERROR: Payload too large
```

### Success Criteria

- ✅ Normal data passes through with "ECHO: " prefix
- ✅ SQL injection patterns blocked
- ✅ Oversized payloads blocked
- ✅ Invalid characters blocked
- ✅ Error messages sent back to client

### Implementation Estimate

**Time**: 1 hour (just add validation logic to echo_component.c)

---

## Stage 3: Add Second NIC (Dual Network)

**Goal**: Enable **network segmentation** with separate external/internal networks

**Prerequisites**: Stage 2 complete

### New Architecture

```
External Network          |          Internal Network
(10.0.2.0/24)            |          (192.168.100.0/24)

netcat client             |          netcat listener
     ↓                    |               ↓
[NIC1 - VirtIO0]          |          [NIC2 - VirtIO1]
     ↓                    |               ↑
[EthDriverRx]             |          [EthDriverTx]
     ↓                    |               ↑
     └─────> [Filter Component] ─────────┘
```

### New Components

**Split EthernetDriver into two**:

1. **EthDriverRx** (External NIC)
   - VirtIO device 0 at 0xa003000
   - lwIP TCP server
   - Receives from external clients
   - Forwards to Filter via dataport

2. **EthDriverTx** (Internal NIC)
   - VirtIO device 1 at 0xa003200
   - lwIP TCP client
   - Receives from Filter via dataport
   - Sends to internal server

### QEMU Configuration

```bash
# Two separate VirtIO-Net devices
-netdev user,id=net0,net=10.0.2.0/24,hostfwd=tcp::6000-10.0.2.15:1234 \
-device virtio-net-device,netdev=net0 \

-netdev user,id=net1,net=192.168.100.0/24,hostfwd=tcp::7000-192.168.100.15:5678 \
-device virtio-net-device,netdev=net1
```

### Testing

```bash
# Terminal 1: Start system (dual NIC)
./simulate --extra-qemu-args="<dual NIC config>"

# Terminal 2: Simulate internal server
nc -l localhost 7000

# Terminal 3: Connect as external client
echo "Test" | nc localhost 6000

# Expected flow:
# - Client connects to port 6000 (NIC1)
# - Filter processes data
# - Data forwarded to port 7000 (NIC2)
# - Internal server receives: "ECHO: Test"
```

### Success Criteria

- ✅ Two VirtIO devices initialized
- ✅ Two separate IP addresses (10.0.2.15 and 192.168.100.10)
- ✅ Data flows: External → NIC1 → Filter → NIC2 → Internal
- ✅ Networks physically isolated (separate QEMU networks)

### Implementation Estimate

**Time**: 3-4 hours

**Complexity**: Medium (need to split driver, configure two VirtIO devices)

---

## Stage 4: Transparent Proxy (Connection Recreation)

**Goal**: Create **NEW internal connection** instead of simple forwarding

**Prerequisites**: Stage 3 complete

### Key Difference

**Stage 3** (Simple forwarding):
- External client → NIC1 (Connection 1)
- Filter reads from Connection 1
- Filter writes to Connection 2 (reuses same TCP state)

**Stage 4** (True proxy):
- External client → NIC1 (Connection 1 - terminated by EthDriverRx)
- Filter processes data
- EthDriverTx creates NEW Connection 2 to internal server
- Two completely independent TCP connections

### New Logic in EthDriverTx

```c
// Instead of simple forwarding, create NEW connection

struct tcp_pcb *internal_server_pcb = NULL;

void connect_to_internal_server(void) {
    // Create new TCP connection to internal server
    internal_server_pcb = tcp_new();

    ip_addr_t server_ip;
    IP4_ADDR(&server_ip, 192, 168, 100, 15);  // Internal server

    err_t err = tcp_connect(internal_server_pcb, &server_ip, 5678,
                           internal_connected_callback);
    if (err != ERR_OK) {
        printf("Failed to connect to internal server\n");
    }
}

void forward_to_internal_server(void) {
    tx_ready_wait();  // Wait for filter to provide data

    // Read sanitized data from filter
    char *data = (char *)tx_packet_buffer;
    size_t len = strlen(data);

    // Send via NEW internal connection
    tcp_write(internal_server_pcb, data, len, TCP_WRITE_FLAG_COPY);
    tcp_output(internal_server_pcb);

    tx_sent_emit();  // Signal filter
}
```

### Success Criteria

- ✅ External connection independent of internal connection
- ✅ Can close external connection while keeping internal open
- ✅ Connection state tracked separately
- ✅ True network proxy behavior

### Implementation Estimate

**Time**: 2-3 hours

**Complexity**: Medium (lwIP connection management)

---

## Stage 5: ICS Protocol Parsing (Final Goal)

**Goal**: Parse and filter **ICS protocols** (Modbus, DNP3, EtherNet/IP)

**Prerequisites**: Stage 4 complete

### New Features

**Filter Component becomes ICS Firewall**:

```c
// Parse Modbus TCP protocol
typedef struct {
    uint16_t transaction_id;
    uint16_t protocol_id;
    uint16_t length;
    uint8_t unit_id;
    uint8_t function_code;
    // ... data
} modbus_tcp_header_t;

bool parse_and_filter_modbus(uint8_t *payload, size_t len) {
    modbus_tcp_header_t *mbap = (modbus_tcp_header_t *)payload;

    // Validate MBAP header
    if (ntohs(mbap->protocol_id) != 0) {
        printf("BLOCKED: Invalid Modbus protocol ID\n");
        return false;
    }

    // Check function code
    uint8_t fc = mbap->function_code;

    // ALLOW: Read operations
    if (fc == 0x01 || fc == 0x02 || fc == 0x03 || fc == 0x04) {
        printf("ALLOWED: Modbus read function 0x%02x\n", fc);
        return true;
    }

    // BLOCK: Write operations (security policy)
    if (fc == 0x05 || fc == 0x06 || fc == 0x0F || fc == 0x10) {
        printf("BLOCKED: Modbus write function 0x%02x\n", fc);
        return false;
    }

    printf("BLOCKED: Unknown Modbus function 0x%02x\n", fc);
    return false;
}
```

### Success Criteria

- ✅ Modbus TCP packets parsed correctly
- ✅ Read commands allowed
- ✅ Write commands blocked
- ✅ Invalid packets rejected
- ✅ Logging of all filter decisions

### Implementation Estimate

**Time**: 4-6 hours (protocol parsing + testing)

**Complexity**: High (need ICS protocol knowledge)

---

## Overall Timeline

| Stage | Time | Cumulative |
|-------|------|------------|
| Stage 1: Single NIC Echo | 2-3 hours | 2-3 hours |
| Stage 2: Sanitization | 1 hour | 3-4 hours |
| Stage 3: Dual NIC | 3-4 hours | 6-8 hours |
| Stage 4: Transparent Proxy | 2-3 hours | 8-11 hours |
| Stage 5: ICS Protocol Parsing | 4-6 hours | 12-17 hours |

**Total**: 12-17 hours across 5 stages

---

## Recommended Approach

### Week 1: Foundation
- **Day 1-2**: Stage 1 (Single NIC Echo) - **MOST CRITICAL**
  - Proves dataport works for network packets
  - Validates zero-copy architecture
  - Simple enough to debug easily

- **Day 3**: Stage 2 (Sanitization)
  - Demonstrates security filtering
  - Easy to add once Stage 1 works

### Week 2: Network Segmentation
- **Day 4-5**: Stage 3 (Dual NIC)
  - Enables IT/OT network separation
  - More complex but builds on Stage 1

- **Day 6-7**: Stage 4 (Transparent Proxy)
  - True proxy behavior
  - Prepares for real-world deployment

### Week 3: ICS Protocols (Optional)
- **Day 8-10**: Stage 5 (ICS Parsing)
  - Modbus, DNP3, EtherNet/IP
  - Research contribution

---

## Success Metrics Per Stage

### Stage 1
- **Technical**: Dataport packet transfer works
- **Research**: Proof of concept for zero-copy networking

### Stage 2
- **Technical**: Security filtering in isolated component
- **Research**: Component-based security validation

### Stage 3
- **Technical**: Dual-NIC network segmentation
- **Research**: IT/OT network separation on seL4

### Stage 4
- **Technical**: Independent connection management
- **Research**: Transparent proxy on verified microkernel

### Stage 5
- **Technical**: ICS protocol deep packet inspection
- **Research**: Formally verified ICS firewall

---

## Current Status

**vm_ethernet_echo project**:
- ✅ Directory structure created
- ✅ CAmkES assembly designed (vm_ethernet_echo.camkes)
- ✅ EthernetDriver copied from vm_freertos_net
- ✅ Architecture documented

**Ready for**: Stage 1 implementation

**Next immediate steps**:
1. Create simple echo_component.c (50 lines)
2. Modify ethernet_driver.c to forward via dataport (20 lines)
3. Create CMakeLists.txt files
4. Build and test Stage 1

---

## Decision Point

**You asked**: Does this make sense?

**Answer**: YES! ✅ This is the correct approach.

**Why**:
- ✅ Each stage is independently testable
- ✅ Complexity increases gradually
- ✅ Early stages provide immediate value (proof of concept)
- ✅ Can stop at any stage if time-limited
- ✅ Each stage contributes research value

**Recommendation**: **Start with Stage 1 NOW**

Stage 1 alone is sufficient to:
- Prove dataport-based packet transfer works
- Demonstrate component isolation
- Publish as "Zero-Copy Network Processing on seL4"

Later stages are **enhancements**, not requirements.

---

**Ready to implement Stage 1?** I can start coding the simple echo_component.c and modify the ethernet_driver.c right now!
