# System Dataflow Diagram

## MODBUS Bidirectional Cross-Domain Solution

**Version:** v2.240+ (Stable)
**Last Updated:** 2025-12-01

---

## High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    MODBUS Bidirectional Gateway                          │
│                  (seL4 Microkernel + CAmkES Framework)                   │
└──────────────────────────────────────────────────────────────────────────┘

External Network (Untrusted)          Internal Network (Trusted)
192.168.96.0/24                        192.168.95.0/24
     │                                          │
     │                                          │
┌────▼─────────────┐                  ┌────────▼────────┐
│   SCADA System   │                  │   PLC Device    │
│  192.168.96.5    │                  │  192.168.95.2   │
│   Port: Random   │                  │   Port: 502     │
└────┬─────────────┘                  └────────┬────────┘
     │                                          │
     │ TCP                                      │ TCP
     │                                          │
┌────▼──────────────────────────────────────────▼────────┐
│              seL4 Virtualization Platform               │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │         VirtIO_Net0_Driver (External)           │   │
│  │         IP: 192.168.96.10 (DHCP)                │   │
│  │         TCP Server Port: 502                    │   │
│  │         Role: External Network Interface        │   │
│  │         - lwIP TCP/IP Stack                     │   │
│  │         - VirtIO-net Device Driver              │   │
│  │         - Connection Management (150 slots)     │   │
│  │         - Session ID Assignment                 │   │
│  └──────────┬──────────────────┬───────────────────┘   │
│             │                  │                        │
│             │ Inbound          │ Outbound               │
│             │ Dataport         │ Dataport               │
│             │                  │                        │
│  ┌──────────▼──────────┐  ┌───▼────────────────────┐   │
│  │   ICS_Inbound       │  │   ICS_Outbound         │   │
│  │   (Validator)       │  │   (Validator)          │   │
│  │   - EverParse       │  │   - EverParse          │   │
│  │   - Policy Rules    │  │   - Policy Rules       │   │
│  │   - Metadata Fwd    │  │   - Metadata Fwd       │   │
│  └──────────┬──────────┘  └───▲────────────────────┘   │
│             │                  │                        │
│             │ Inbound          │ Outbound               │
│             │ Dataport         │ Dataport               │
│             │                  │                        │
│  ┌──────────▼──────────────────┴───────────────────┐   │
│  │         VirtIO_Net1_Driver (Internal)           │   │
│  │         IP: 192.168.95.10 (DHCP)                │   │
│  │         TCP Client (Dynamic Ports)              │   │
│  │         Role: Internal Network Interface        │   │
│  │         - lwIP TCP/IP Stack                     │   │
│  │         - VirtIO-net Device Driver              │   │
│  │         - Connection Pool (150 slots)           │   │
│  │         - Session Mapping                       │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

---

## Detailed Dataflow: Request Path (INBOUND)

**Direction:** External Network → Internal Network
**Use Case:** SCADA polling PLC for register values

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Step 1: SCADA Initiates Connection                                     │
└─────────────────────────────────────────────────────────────────────────┘

SCADA (192.168.96.5:45123)  ──TCP SYN──>  VirtIO_Net0 (192.168.96.10:502)
                                          │
                                          ▼
                                    lwIP TCP Server
                                    tcp_accept() callback
                                    │
                                    ▼
                              Allocate connection_metadata
                              - session_id = 1234
                              - original_src_ip = 192.168.96.5
                              - original_dest_ip = (192.168.95.2)
                              - src_port = 45123
                              - dest_port = 502

┌─────────────────────────────────────────────────────────────────────────┐
│ Step 2: SCADA Sends MODBUS Request                                     │
└─────────────────────────────────────────────────────────────────────────┘

SCADA (192.168.96.5:45123)  ──MODBUS Read Holding Registers──>
                                          │
                                          ▼
                              VirtIO_Net0: tcp_echo_recv()
                              │
                              ▼
                        Extract FrameMetadata:
                        {
                          session_id: 1234
                          ethertype: 0x0800 (IPv4)
                          src_ip: 192.168.96.5
                          dst_ip: 192.168.96.10
                          ip_protocol: 6 (TCP)
                          src_port: 45123
                          dst_port: 502
                          payload_offset: 54
                          payload_length: 12 (MODBUS frame)
                          is_tcp: 1
                        }
                        │
                        ▼
                  Copy to InboundDataport:
                  ┌──────────────────────────────┐
                  │ request_msg:                 │
                  │   - FrameMetadata (above)    │
                  │   - payload: [MODBUS data]   │
                  │ close_queue: (empty)         │
                  └──────────────────────────────┘
                        │
                        ▼
                  inbound_ready_emit()
                        │
                        ▼
            ┌───────────────────────────┐
            │     ICS_Inbound           │
            │     in_ntfy_wait()        │
            └───────────────────────────┘
                        │
                        ▼
              process_message()
              - basic_bounds_check()
              - validate_message()
              - everparse_validate()
                        │
                        ▼
                  ✅ VALIDATED
                        │
                        ▼
            Forward to out_dp (Net1's inbound)
            memcpy(request_msg + close_queue)
            __sync_synchronize()
            out_ntfy_emit()
                        │
                        ▼
            ┌───────────────────────────┐
            │   VirtIO_Net1_Driver      │
            │   inbound_ready_handle()  │
            └───────────────────────────┘
                        │
                        ▼
              Lookup session_id=1234
              - Not found → Create new connection
                        │
                        ▼
              tcp_connect() to PLC
              192.168.95.2:502
              - Store Net0's metadata
              - Mark awaiting_response=true
                        │
                        ▼
              tcp_connected_callback()
              - Send MODBUS request to PLC
              - tcp_write(pcb, payload, 12)

┌─────────────────────────────────────────────────────────────────────────┐
│ Step 3: PLC Processes and Responds                                     │
└─────────────────────────────────────────────────────────────────────────┘

                   PLC (192.168.95.2:502)
                            │
                            ▼
                   Processes MODBUS request
                   Reads holding registers
                            │
                            ▼
                   MODBUS Response (8 bytes)
                            │
                            ▼
              VirtIO_Net1: tcp_plc_recv()
              - Extract response payload
              - Lookup connection by PCB
              - Mark response_received=true
                            │
                            ▼
           Copy to OutboundDataport:
           ┌──────────────────────────────┐
           │ response_msg:                │
           │   - session_id: 1234         │
           │   - src_ip: 192.168.95.2     │
           │   - payload: [MODBUS resp]   │
           │ error_queue: (empty)         │
           └──────────────────────────────┘
                            │
                            ▼
              outbound_ready_emit()
                            │
                            ▼
            ┌───────────────────────────┐
            │   ICS_Outbound            │
            │   in_ntfy_wait()          │
            └───────────────────────────┘
                            │
                            ▼
              process_message()
              - validate_message()
              - everparse_validate()
                            │
                            ▼
                  ✅ VALIDATED
                            │
                            ▼
           Forward to out_dp (Net0's outbound)
           memcpy(response_msg + error_queue)
           __sync_synchronize()
           out_ntfy_emit()
                            │
                            ▼
            ┌───────────────────────────┐
            │   VirtIO_Net0_Driver      │
            │   outbound_ready_handle() │
            └───────────────────────────┘
                            │
                            ▼
              Lookup session_id=1234
              - Found connection
              - Reconstruct TCP packet
                            │
                            ▼
              Create pbuf with MODBUS response
              ip_output() → netif_output()
                            │
                            ▼
                   VirtIO TX virtqueue
                            │
                            ▼
              SCADA (192.168.96.5:45123)
              ◀──MODBUS Response──
```

---

## Detailed Dataflow: Response Path (OUTBOUND)

**Direction:** Internal Network → External Network
**Use Case:** PLC spontaneous update (not implemented, shows architecture)

```
Currently, the system operates in REQUEST-RESPONSE mode only.
Future enhancements may support PLC-initiated messages.

Architecture supports bidirectional flow:
- Net1 can accept inbound connections from PLC
- Net0 can relay outbound data to SCADA
```

---

## Control Flow: Connection Lifecycle

### Connection State Sharing

```
┌──────────────────────────────────────────────────────────────────┐
│                   Connection State Table                         │
│              (Shared via seL4 Dataport - v2.117)                 │
└──────────────────────────────────────────────────────────────────┘

VirtIO_Net0_Driver                    VirtIO_Net1_Driver
   │                                         │
   │ Exposes: net0_conn_state                │ Exposes: net1_conn_state
   │ Reads:   net1_conn_state                │ Reads:   net0_conn_state
   │                                         │
   ▼                                         ▼
┌─────────────────────────┐         ┌─────────────────────────┐
│ struct connection_state │         │ struct connection_state │
│ {                       │         │ {                       │
│   session_id            │◀───────▶│   session_id            │
│   active                │   Read  │   active                │
│   awaiting_response     │   Only  │   awaiting_response     │
│   response_received     │         │   response_received     │
│ }                       │         │ }                       │
│ [256 entries]           │         │ [256 entries]           │
└─────────────────────────┘         └─────────────────────────┘

Purpose:
- Synchronize connection cleanup
- Prevent dangling references
- Coordinate error handling
```

### Close Notification Queue

```
┌──────────────────────────────────────────────────────────────────┐
│                     Close Queue (Net0 → Net1)                    │
│              Passed through ICS_Inbound validation               │
└──────────────────────────────────────────────────────────────────┘

Net0: SCADA closes connection
   │
   ▼
Net0: tcp_err_callback() / tcp_recv(p=NULL)
   │
   ▼
Net0: Enqueue to close_queue
   ┌────────────────────────────┐
   │ struct control_queue {     │
   │   session_id = 1234        │
   │   ctrl_type = CLOSE        │
   │   timestamp = sys_now()    │
   │ }                          │
   └────────────────────────────┘
   │
   ▼
Net0: Write to InboundDataport.close_queue
   │
   ▼
Net0: Set request_msg.payload_length = 0 (sentinel)
   │
   ▼
ICS_Inbound: Forward close_queue to Net1
   │
   ▼
Net1: Read InboundDataport.close_queue
   │
   ▼
Net1: Lookup session_id=1234
   │
   ▼
Net1: tcp_close() to PLC connection
   │
   ▼
Net1: Cleanup metadata
```

### Error Notification Queue

```
┌──────────────────────────────────────────────────────────────────┐
│                     Error Queue (Net1 → Net0)                    │
│             Passed through ICS_Outbound validation               │
└──────────────────────────────────────────────────────────────────┘

Net1: PLC connection error (timeout/RST)
   │
   ▼
Net1: tcp_err_callback()
   │
   ▼
Net1: Enqueue to error_queue
   ┌────────────────────────────┐
   │ struct control_queue {     │
   │   session_id = 1234        │
   │   ctrl_type = ERROR        │
   │   timestamp = sys_now()    │
   │ }                          │
   └────────────────────────────┘
   │
   ▼
Net1: Write to OutboundDataport.error_queue
   │
   ▼
Net1: Set response_msg.payload_length = 0 (sentinel)
   │
   ▼
ICS_Outbound: Forward error_queue to Net0
   │
   ▼
Net0: Read OutboundDataport.error_queue
   │
   ▼
Net0: Lookup session_id=1234
   │
   ▼
Net0: tcp_close() to SCADA connection
   │
   ▼
Net0: Cleanup metadata
```

---

## Memory Layout

### Dataport Buffers

```
InboundDataport (65536 bytes):
┌──────────────────────────────────────────────────┐
│ ICS_Message request_msg          (~60.0 KB)     │
│   - FrameMetadata                (52 bytes)     │
│   - payload_length               (2 bytes)      │
│   - payload[MAX_PAYLOAD_SIZE]    (60000 bytes)  │
├──────────────────────────────────────────────────┤
│ struct control_queue close_queue (~1.5 KB)      │
│   - head, tail                   (8 bytes)      │
│   - entries[64]                  (1536 bytes)   │
└──────────────────────────────────────────────────┘

OutboundDataport (65536 bytes):
┌──────────────────────────────────────────────────┐
│ ICS_Message response_msg         (~60.0 KB)     │
│   - FrameMetadata                (52 bytes)     │
│   - payload_length               (2 bytes)      │
│   - payload[MAX_PAYLOAD_SIZE]    (60000 bytes)  │
├──────────────────────────────────────────────────┤
│ struct control_queue error_queue (~1.5 KB)      │
│   - head, tail                   (8 bytes)      │
│   - entries[64]                  (1536 bytes)   │
└──────────────────────────────────────────────────┘

Connection State Buffers (8192 bytes each):
┌──────────────────────────────────────────────────┐
│ struct connection_state_table    (8 KB)         │
│   - entry_count                  (4 bytes)      │
│   - padding                      (4 bytes)      │
│   - entries[256]                 (5120 bytes)   │
│     * session_id                 (4 bytes)      │
│     * active                     (1 byte)       │
│     * awaiting_response          (1 byte)       │
│     * response_received          (1 byte)       │
│     * padding                    (13 bytes)     │
└──────────────────────────────────────────────────┘
```

---

## Component Interactions Summary

| Component            | Role                          | Connections                          |
|----------------------|-------------------------------|--------------------------------------|
| **VirtIO_Net0_Driver** | External network interface    | - Hardware: virtio_net0_hw<br>- Data: ICS_Inbound (TX), ICS_Outbound (RX)<br>- State: Shared with Net1 |
| **ICS_Inbound**        | External→Internal validator   | - Input: Net0<br>- Output: Net1      |
| **ICS_Outbound**       | Internal→External validator   | - Input: Net1<br>- Output: Net0      |
| **VirtIO_Net1_Driver** | Internal network interface    | - Hardware: virtio_net1_hw<br>- Data: ICS_Inbound (RX), ICS_Outbound (TX)<br>- State: Shared with Net0 |

---

## Security Properties

### Isolation Guarantees

1. **Component Isolation:**
   - Each component runs in separate seL4 protection domain
   - No direct memory access between components
   - All communication through seL4 capabilities

2. **Protocol Break:**
   - Net0 and Net1 run independent lwIP stacks
   - No direct packet forwarding
   - ICS components enforce validation barrier

3. **Metadata-Based Validation:**
   - ICS components operate on extracted metadata
   - No lwIP stack access in validators
   - Stateless validation logic

### Trust Boundaries

```
┌────────────────────────────────────────────────────────┐
│              Untrusted Domain (External)               │
│  - SCADA systems                                       │
│  - Engineering workstations                            │
│  - HMIs                                                │
└─────────────────┬──────────────────────────────────────┘
                  │
                  ▼
        ┌─────────────────────┐
        │  VirtIO_Net0_Driver │  ◀── First validation point
        │  (lwIP stack)       │      (TCP/IP parsing)
        └─────────────────────┘
                  │
                  ▼
        ┌─────────────────────┐
        │    ICS_Inbound      │  ◀── Second validation point
        │  (EverParse + Policy)│     (Protocol validation)
        └─────────────────────┘
                  │
                  ▼
┌─────────────────┴──────────────────────────────────────┐
│              Trusted Domain (Internal)                 │
│  - PLCs                                                │
│  - RTUs                                                │
│  - Field devices                                       │
└────────────────────────────────────────────────────────┘
```

---

## Performance Characteristics

### Connection Limits

- **Max Concurrent Connections:** 150 per network driver
- **Session ID Pool:** 32-bit space (4,294,967,295 unique sessions)
- **Connection Metadata Size:** ~100 bytes per connection
- **Total Metadata Memory:** ~15 KB per driver

### Buffer Sizes

- **MAX_PAYLOAD_SIZE:** 60,000 bytes
- **Dataport Size:** 65,536 bytes
- **Control Queue Depth:** 64 entries
- **Cleanup Queue Depth:** 512 entries

### Latency Estimates

- **Inbound Path:** SCADA → Net0 → ICS_In → Net1 → PLC
  - Network: ~1-5 ms
  - Validation: ~50-100 μs
  - Total: ~2-10 ms

- **Outbound Path:** PLC → Net1 → ICS_Out → Net0 → SCADA
  - Network: ~1-5 ms
  - Validation: ~50-100 μs
  - Total: ~2-10 ms

---

**Document Status:** Current (matches v2.240+ implementation)
**Maintenance:** Update when architecture changes
**Related Documents:**
- [SEQUENCE-DIAGRAM.md](SEQUENCE-DIAGRAM.md) - TCP connection lifecycle
- [SYSTEM-DESIGN.md](SYSTEM-DESIGN.md) - Detailed design specification
- [README.md](README.md) - System overview and usage
