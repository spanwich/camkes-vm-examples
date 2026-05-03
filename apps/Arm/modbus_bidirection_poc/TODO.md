# TODO: VirtIO_Net1_Driver Architectural Refactoring

**Status**: Working baseline at v2.212-phase3 (rolled back from v2.216)
**Primary Issues**: pbuf pool exhaustion, obscured connection lifecycle, 6000+ line monolith
**Goal**: Achieve stable 10+ minute runtime without crashes or resource leaks

---

## Critical Issues (Must Fix)

### 1. Port Matching Logic Bug (Line 846)

**File**: `components/VirtIO_Net1_Driver/virtio_net1_driver.c:846`

**Current Code**:
```c
if (connection_table[i].lwip_ephemeral_port == src_port &&
    connection_table[i].src_port == dest_port) {
```

**Problem**:
- Semantic confusion between RX direction (SCADA→PLC) and TX direction (PLC→SCADA)
- In TX path: `src_port` = lwIP ephemeral, `dest_port` = SCADA port
- Metadata stores: `src_port` = SCADA original, `dst_port` = 502
- Current logic tries to match `connection_table[i].src_port` with `dest_port` (should work but causes issues)
- Changing to `dst_port` breaks communication immediately

**Tasks**:
- [ ] Trace packet flow in both directions to understand port semantics
- [ ] Add logging to show what ports are being matched in TX path
- [ ] Verify metadata is stored correctly during connection establishment
- [ ] Check timing: is metadata available when TX path runs?
- [ ] Consider renaming fields for clarity:
  - `scada_port` instead of `src_port`
  - `plc_port` (always 502) instead of `dst_port`
  - `lwip_ephemeral_port` (already clear)

**Related Files**:
- TX path logic: lines 800-900
- Metadata initialization: `connection_add()` function
- Port storage after `tcp_connect()`: line ~3630

---

### 2. Centralized Cleanup - Multiple Paths Set meta=NULL

**Problem**:
- v2.211 analysis found **16 different code paths** that modify metadata
- v2.212 reduced to 2 paths but still has pbuf leaks
- v2.216 attempted unified cleanup model but rolled back
- Many execution paths can still null metadata, causing race conditions with TX

**Locations that modify metadata** (from v2.211 analysis):
1. Line 1467-1468: `connection_cleanup_atomic()` - direct manipulation
2. Line 1560-1561: `process_cleanup_queue()` - ONLY intended path ✅
3. Line 1800-1801: `connection_remove()` - legacy echo server
4. Line 2956: `inbound_tcp_recv_callback(p=NULL)` - PLC FIN
5. Line 3348-3349: `inbound_tcp_err_callback()` - error callback
6. Line 3446-3447: `inbound_tcp_connected_callback()` - handshake fail
7. Line 3813-3814: Close notification handler
8. Multiple paths in `inbound_ready_handle()`

**Tasks**:
- [ ] **Audit v2.212 implementation**: Why did centralized cleanup fail?
  - Check if all 16 paths were converted to use intent flags
  - Verify `process_cleanup_queue()` is only place setting `active=false`
  - Look for race conditions between cleanup and TX path

- [ ] **Design enforcement mechanism**:
  - Make `active` and `pcb` fields private (accessor functions only)
  - Only `process_cleanup_queue()` can modify (compile-time enforcement)
  - All other code uses `enqueue_cleanup(session_id)` exclusively

- [ ] **Add validation assertions**:
  - Assert `active` is only modified in cleanup queue processor
  - Add stack trace logging when metadata is cleared
  - Detect double-cleanup attempts

**Related Documentation**:
- `research-docs/v2.211-metadata-nulling-analysis.md` - Complete execution path mapping
- `research-docs/v2.212-centralized-cleanup-implementation-complete.md` - What was attempted

---

### 3. Obscured Connection Lifecycle (`meta->active`)

**Problem**:
- No explicit state machine governing connection lifecycle
- `active` flag has unclear semantics - when is it set? when cleared?
- Lifecycle emerges from interactions between:
  - `metadata_close_pending` flag (v2.210 delayed cleanup)
  - `pcb_closed` flag (v2.212)
  - `cleanup_in_progress` flag (guard)
  - `close_notified` flag (deduplication)
  - `awaiting_response` flag
  - `response_received` flag
  - Various timeout checks
- Impossible to reason about correctness without tracing all code paths

**Ideal Lifecycle** (what should happen):
```
1. SCADA connects → active=true (connection_add)
2. SCADA sends request → awaiting_response=true
3. PLC sends response → response_received=true
4. Net0 receives response → awaiting_response=false
5. Connection closes (either side) → active=false (via cleanup queue ONLY)
```

**Actual Lifecycle** (emergent behavior):
- Multiple paths race to set `active=false`
- Conditions scattered across callbacks, handlers, cleanup functions
- No documentation of valid state transitions
- No invariant checking

**Tasks**:
- [ ] **Document current state machine**:
  - List all states (active, awaiting_response, metadata_close_pending, etc.)
  - Map all transitions between states
  - Identify which code paths trigger transitions
  - Find invalid/unreachable states

- [ ] **Design explicit state machine**:
  ```
  enum connection_state {
      CONN_ESTABLISHING,    // tcp_connect() called, waiting for ack
      CONN_IDLE,            // Connected, no request pending
      CONN_REQUEST_SENT,    // Request forwarded to PLC
      CONN_AWAITING_RESPONSE, // Waiting for PLC response
      CONN_RESPONSE_READY,  // Response received, forwarding to SCADA
      CONN_CLOSING,         // Close initiated, waiting for cleanup
      CONN_CLOSED           // Fully closed, slot can be reused
  };
  ```

- [ ] **Implement state machine**:
  - Replace boolean flags with single `state` enum
  - Add state transition function with validation
  - Assert valid transitions only (catch bugs early)
  - Log all state changes for debugging

- [ ] **Define cleanup conditions clearly**:
  - `active=false` should only happen in `CONN_CLOSED` state
  - Cleanup queue should transition `CONN_CLOSING → CONN_CLOSED`
  - All other paths transition to `CONN_CLOSING` and enqueue cleanup

**Related Files**:
- Metadata structure: line ~84-120
- Connection establishment: `connection_add()`
- State transitions scattered throughout 6000-line file (needs consolidation)

---

## Architectural Refactoring (Should Do)

### 4. Modularize 6000-Line Monolith

**Current State**:
- `virtio_net1_driver.c`: 6091 lines (beyond human cognitive capacity)
- All functionality in single file:
  - VirtIO device driver
  - lwIP stack integration
  - Connection management
  - Cleanup logic
  - TX/RX packet processing
  - Control queue communication
  - Debugging infrastructure

**Optimization Concerns** (analysis):
- ✅ LTO (Link-Time Optimization) eliminates cross-file optimization penalty
- ✅ `static inline` in headers preserves inlining for hot paths
- ✅ Instruction cache locality: negligible impact (VM/lwIP overhead dominates)
- ✅ Build time: incremental builds make this irrelevant
- **Verdict**: No meaningful optimization loss from splitting

**Proposed Structure**:
```
components/VirtIO_Net1_Driver/
├── virtio_net1_driver.c          (~500 lines)
│   ├── CAmkES component entry points
│   ├── Main loop
│   ├── Initialization
│   └── Glue code
│
├── connection_mgmt.c/.h          (~800 lines)
│   ├── Metadata table management
│   ├── Connection lookup functions
│   ├── Explicit state machine implementation
│   └── State transition validation
│
├── cleanup_queue.c/.h            (~600 lines)
│   ├── Cleanup queue operations (enqueue/dequeue)
│   ├── process_cleanup_queue() - ONLY place setting active=false
│   ├── Deferred cleanup conditions
│   └── Cleanup statistics
│
├── lwip_callbacks.c/.h           (~1000 lines)
│   ├── tcp_recv callback
│   ├── tcp_sent callback
│   ├── tcp_err callback
│   ├── tcp_connected callback
│   └── tcp_poll callback
│
├── tx_path.c/.h                  (~800 lines)
│   ├── Packet transmission logic
│   ├── IP address restoration (FIX for Issue #1)
│   ├── Checksum recalculation
│   └── TX error handling
│
├── rx_path.c/.h                  (~800 lines)
│   ├── Packet reception from PLC
│   ├── Forwarding to Net0
│   └── RX error handling
│
├── control_queues.c/.h           (~600 lines)
│   ├── Net0 ↔ Net1 communication
│   ├── Request queue processing
│   ├── Error notification queue
│   └── Close notification queue
│
├── virtio_device.c/.h            (~500 lines)
│   ├── VirtIO-net device initialization
│   ├── Ring buffer management
│   ├── DMA operations
│   └── Device-specific logic
│
└── debug_levels.h                (existing)
```

**Benefits**:
1. **Addresses Issue #1**: TX path isolated in `tx_path.c` - easier to reason about port semantics
2. **Addresses Issue #2**: `cleanup_queue.c` as single source of truth - violations are obvious
3. **Addresses Issue #3**: `connection_mgmt.c` contains explicit state machine - lifecycle is documented
4. **Debugging**: Can add module-specific logging levels
5. **Testing**: Can unit test individual modules
6. **Code review**: Smaller files in git diffs
7. **Onboarding**: New developers can understand one module at a time

**Tasks**:
- [ ] **Phase 1: Extract headers** (non-invasive)
  - Create header files with function declarations
  - No code movement yet
  - Verify compilation still works

- [ ] **Phase 2: Move state machine** (high value)
  - Extract connection_mgmt.c with explicit state machine
  - This helps Issue #3 immediately

- [ ] **Phase 3: Move cleanup logic** (critical for Issue #2)
  - Extract cleanup_queue.c
  - Make metadata modification private
  - Enforce single cleanup authority

- [ ] **Phase 4: Move TX/RX paths** (helps Issue #1)
  - Extract tx_path.c and rx_path.c
  - Separate packet direction semantics
  - Clarify port matching logic

- [ ] **Phase 5: Move remaining modules**
  - lwip_callbacks.c
  - control_queues.c
  - virtio_device.c

- [ ] **Phase 6: Update build system**
  - Modify CMakeLists.txt to compile all modules
  - Enable LTO: `-flto` flag
  - Profile hot paths, mark as `static inline` if needed

**Risks**:
- Build system complexity (CAmkES components)
- Need to preserve CAmkES component boundary
- Migration testing required for each phase

**Rollback Plan**:
- Git branch for each phase
- Keep v2.212 as stable baseline
- Each phase independently testable

---

## Testing & Validation

### 5. Comprehensive Test Suite

**Current Testing**:
- Manual GRFICS deployment testing
- Log analysis for errors
- No automated regression tests
- No unit tests

**Needed Tests**:

#### Unit Tests (per module after split):
- [ ] `connection_mgmt`: State machine transitions
- [ ] `cleanup_queue`: Enqueue/dequeue correctness, deduplication
- [ ] `tx_path`: Port matching logic with mock metadata
- [ ] `rx_path`: Packet parsing
- [ ] `control_queues`: Queue overflow handling

#### Integration Tests:
- [ ] SCADA-initiated close (baseline: should work)
- [ ] PLC-initiated error (currently broken - orphan PCBs)
- [ ] PLC timeout error
- [ ] PLC sends RST
- [ ] PLC sends FIN prematurely
- [ ] Burst of rapid PLC errors (stress test)
- [ ] 1000+ transactions without pbuf leak
- [ ] Connection pool exhaustion recovery

#### Metrics to Monitor:
- [ ] PBUF pool usage (should stay < 10% during normal operation)
- [ ] Connection count (should return to 0 after all close)
- [ ] Orphan PCB count (should stay at 0)
- [ ] TX error count (should be 0 with correct implementation)
- [ ] Cleanup queue depth (should drain to 0 between bursts)

**Related Files**:
- Test infrastructure: TBD
- GRFICS deployment script: `run-grfics-deployment.sh`
- Log analysis tools: TBD

---

## Research & Documentation

### 6. Understanding v2.212-216 Failures

**Goal**: Learn why previous fixes didn't work before attempting new ones

**Tasks**:
- [ ] **Analyze v2.212 logs**:
  - Why did centralized cleanup still have pbuf leaks?
  - Were all 16 metadata modification sites converted?
  - Did deferred cleanup conditions work correctly?
  - What was the queue depth during failures?

- [ ] **Analyze v2.213-214 changes**:
  - What was different from v2.212?
  - Why was further iteration needed?

- [ ] **Analyze v2.216 unified cleanup model**:
  - Why did Net0-controlled lifecycle fail?
  - Was the "orphan PCB" problem actually solved?
  - What broke during rollback?

- [ ] **Document rollback decision**:
  - What specific symptoms triggered rollback to v2.212?
  - What logs/evidence exist?
  - Save as `research-docs/v2.216-rollback-analysis.md`

### 7. Port Matching Root Cause Analysis

**Goal**: Understand why 100% of TX errors have meta=NULL (v2.210 logs)

**Tasks**:
- [ ] **Trace connection establishment**:
  - Log metadata table after each `connection_add()`
  - Verify ports are stored correctly
  - Check lwip_ephemeral_port is captured after `tcp_connect()`

- [ ] **Trace TX path lookup**:
  - Log all metadata entries during TX path search
  - Show which entry should match but doesn't
  - Identify mismatch: port values? active flag? timing?

- [ ] **Timing analysis**:
  - Is metadata deleted before TX completes?
  - Measure time between connection close and last TX packet
  - Check if cleanup happens during FIN-ACK exchange

- [ ] **Document findings**:
  - Save as `research-docs/tx-metadata-lookup-failure-root-cause.md`
  - Include empirical evidence (logs, traces)
  - Propose fix with justification

---

## Long-Term Improvements

### 8. Simplify Architecture (Post-Refactoring)

**After modularization is complete:**

- [ ] **Remove deprecated code**:
  - Echo server functions (not used for ICS traffic)
  - Legacy cleanup paths (after centralization proven)
  - Old debug levels (v2.207 deprecated old system)

- [ ] **Simplify state flags**:
  - Replace 6+ boolean flags with single state enum
  - Remove redundant guards (e.g., `cleanup_in_progress` after state machine)

- [ ] **Optimize hot paths** (if needed):
  - Profile with `perf` or instrumentation
  - Identify actual bottlenecks (likely not where expected)
  - Apply `static inline` or compiler hints only where proven beneficial

- [ ] **Documentation**:
  - Update README.md with new architecture
  - Add module-level documentation
  - Document state machine with diagrams
  - Update CLAUDE.md with lessons learned

---

## Decision Log

### Why v2.212 Instead of v2.216?

**Decision**: Rolled back from v2.216 to v2.212-phase3
**Date**: 2025-10-31 (inferred from commits)
**Reason**: [TODO: Document specific failure symptoms]

**v2.212 Status**:
- ✅ Working baseline (10+ minutes runtime in past)
- ❌ Still has pbuf pool exhaustion issue
- ❌ Still has "TX: No metadata" errors
- ✅ Better than v2.213-216 (which had worse issues)

**v2.216 Status**:
- 🎯 Goal: Unified cleanup model (Net0 controls lifecycle)
- ❌ Implementation had issues (orphan PCBs?)
- 📋 Plan documented in `research-docs/v2.216-unified-cleanup-model.md`
- ❓ Should be revisited AFTER architectural refactoring

### Why Refactor Before Fixing Bugs?

**Observation**: Your 4 questions are all symptoms of architectural problems:
1. Port matching confusion ← Unclear packet direction semantics
2. Scattered cleanup ← No enforced single authority
3. Obscured lifecycle ← No explicit state machine
4. 6000-line file ← Historical accretion without design

**Principle**: "You can't fix what you can't understand"

**Strategy**:
1. Refactor for clarity FIRST (split into modules, explicit state machine)
2. THEN fix bugs with clear architecture
3. Prevents "whack-a-mole" bug fixing (v2.40 → v2.216 history)

**Risk**: Refactoring might introduce new bugs
**Mitigation**:
- Each phase independently tested
- v2.212 baseline preserved as rollback point
- Git branch for each module extraction

---

## Priority Order

### Phase 1: Understanding (Weeks 1-2)
1. ✅ Document current issues (this TODO list)
2. Research & Documentation (Tasks #6, #7)
3. Design state machine (Task #3)
4. Design module structure (Task #4)

### Phase 2: Foundation (Weeks 3-4)
1. Extract headers (Task #4, Phase 1)
2. Implement explicit state machine (Task #3)
3. Extract connection_mgmt.c (Task #4, Phase 2)
4. Add state transition logging

### Phase 3: Cleanup Enforcement (Weeks 5-6)
1. Extract cleanup_queue.c (Task #4, Phase 3)
2. Make metadata fields private (Task #2)
3. Audit all 16 modification sites (Task #2)
4. Enforce single cleanup authority (Task #2)

### Phase 4: TX/RX Separation (Weeks 7-8)
1. Extract tx_path.c and rx_path.c (Task #4, Phase 4)
2. Fix port matching logic (Task #1)
3. Clarify packet direction semantics (Task #1)
4. Add TX/RX path logging

### Phase 5: Validation (Weeks 9-10)
1. Implement test suite (Task #5)
2. Run 1000+ transaction tests
3. Verify no pbuf leaks
4. Verify no TX errors
5. Verify cleanup works correctly

### Phase 6: Completion (Weeks 11-12)
1. Extract remaining modules (Task #4, Phases 5-6)
2. Enable LTO optimization (Task #4)
3. Clean up deprecated code (Task #8)
4. Update documentation (Task #8)
5. Production deployment testing

---

## Success Criteria

### Must Have (for v2.217+):
- [ ] Zero "TX: No metadata" errors in 1000+ transaction test
- [ ] PBUF pool usage stays < 10% during normal operation
- [ ] Connection count returns to 0 after all connections close
- [ ] Zero orphan PCBs in Net0
- [ ] 10+ minute runtime without crashes
- [ ] All cleanup goes through single authority (`process_cleanup_queue`)

### Should Have:
- [ ] Explicit state machine with documented transitions
- [ ] Code split into <1000 line modules
- [ ] Automated test suite
- [ ] Clear architecture documentation

### Nice to Have:
- [ ] Unit tests for each module
- [ ] Performance profiling and optimization
- [ ] Remove all deprecated code
- [ ] Clean git history with logical commits

---

## Notes & Observations

### From Discussion (2025-11-01):

**Point 1 (Line 846 port logic)**:
- Changing `src_port` to `dst_port` breaks communication immediately
- Current logic should work in theory but has 100% TX error rate (v2.210)
- Root cause unknown - needs empirical investigation (Task #7)

**Point 2 (Centralized cleanup)**:
- v2.212 attempted this: 87.5% reduction in modification sites
- Still failed with pbuf leaks
- Needs architecture refactoring, not incremental fixes

**Point 3 (Lifecycle obscurity)**:
- No explicit state machine currently
- Behavior emerges from 6+ interacting flags
- Impossible to reason about correctness
- **Critical priority** for Task #3

**Point 4 (6000-line file)**:
- Maintainability >> Optimization concerns
- LTO eliminates any performance penalty
- Should split strategically (proposed structure in Task #4)

### Key Insight:

> "The problem wasn't in individual components, but in having **two decision-makers** for cleanup. By reducing to **one decision-maker**, we eliminate coordination bugs."
>
> — From v2.216-unified-cleanup-model.md

This applies to ALL 4 issues:
- One authority for cleanup (Task #2)
- One state machine for lifecycle (Task #3)
- One semantic model for TX/RX (Task #1)
- One module per responsibility (Task #4)

**Design Principle**: Eliminate coordination bugs by eliminating coordination needs.

---

## References

- [CLAUDE.md](../../../../../../CLAUDE.md) - lwIP PCB management lessons
- [README.md](README.md) - Architecture overview
- [research-docs/v2.211-metadata-nulling-analysis.md](../../../../../../research-docs/v2.211-metadata-nulling-analysis.md) - Complete execution path mapping
- [research-docs/v2.212-centralized-cleanup-implementation-complete.md](../../../../../../research-docs/v2.212-centralized-cleanup-implementation-complete.md) - What v2.212 attempted
- [research-docs/v2.216-unified-cleanup-model.md](../../../../../../research-docs/v2.216-unified-cleanup-model.md) - Net0-controlled lifecycle design
- [research-docs/v2.210-pbuf-leak-evidence-analysis.md](../../../../../../research-docs/v2.210-pbuf-leak-evidence-analysis.md) - Empirical evidence of TX→pbuf leak causation

---

**Last Updated**: 2025-11-01
**Current Baseline**: v2.212-phase3
**Next Milestone**: Complete Phase 1 (Understanding) by end of Week 2
