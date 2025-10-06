# ICS One-Way Pipeline V3 - Build and Test Guide
## Phase 1 PoC: Linux VM + ICS Components Coexistence

---

## What This PoC Validates

✅ **Linux VM boots with VirtIO-Net driver**
✅ **ICS pipeline components run alongside VM**
✅ **External network injection via netcat to Linux VM**
✅ **Both systems coexist without interfering**

---

## Build Instructions

### Step 1: Clean Build
```bash
cd /home/iamfo470/phd/camkes-vm-examples
rm -rf build && mkdir build && cd build
```

### Step 2: Configure for ics_oneway_norm_v3
```bash
../init-build.sh -DCAMKES_VM_APP=ics_oneway_norm_v3 \
                 -DAARCH32=0 \
                 -DPLATFORM=qemu-arm-virt
```

### Step 3: Build
```bash
ninja
```

**Expected output:**
```
[100%] Built target rootserver_image
[100%] Built target elfloader
```

### Step 4: Run with VirtIO Network
```bash
# From build directory
cd /home/iamfo470/phd/camkes-vm-examples/ics_oneway_norm_v3

# Run with VirtIO network and port forwarding
./simulate --extra-qemu-args="-nic user,model=virtio,mac=52:54:00:12:34:56,hostfwd=tcp::8502-:502"
```

---

## Expected Startup Output

You should see both the **Linux VM** and **ICS components** starting:

```
seL4 Starting...
Booting all finished, dropped to user space
...
main@main.c:550 [Cond failed: vm]
VM_GENERAL_COMPOSITION_DEF: Starting VM0
Loading Linux: 'linux' from the FileServer
Loading Linux DTB: 'linux-dtb'
Loading initrd: 'linux-initrd'
...
[    0.000000] Booting Linux on physical CPU 0x0
[    0.000000] Linux version 5.x.x ...
...
Welcome to Buildroot
buildroot login: root
(automatic login)
#

[In parallel, you'll see:]
TestTrafficGen: Starting (ICS pipeline test component)
ExtFrontend: Initialization complete
ParserNorm: Validation engine started
PolicyEmit: Policy engine started (allow-all mode)
IntNicDrv: Message sink ready
TestTrafficGen: Sent test message #1 to ICS pipeline
ExtFrontend: Frame received...
```

---

## Test 1: Verify Linux VM VirtIO Network

### In Linux VM console:
```bash
# Check network interface exists
ifconfig -a

# You should see eth0 (VirtIO network interface)
# Configure it
ifconfig eth0 192.168.1.10 up

# Start simple listener on port 502 (Modbus TCP)
nc -l -p 502
```

### From host (another terminal):
```bash
# Send test packet to Linux VM
echo "HELLO_FROM_HOST" | nc localhost 8502
```

### Expected Result:
```
# In Linux VM console, you should see:
HELLO_FROM_HOST
```

**✅ SUCCESS: Linux VM receives VirtIO network packets!**

---

## Test 2: Verify ICS Pipeline Operation

Watch the QEMU console for TestTrafficGen messages:

```
TestTrafficGen: Sent test message #1 to ICS pipeline
ExtFrontend: Frame received (40 bytes)
ParserNorm: Message validated
PolicyEmit: ALLOW - Tag 0x0001
IntNicDrv: Message received and counted (Total: 1)

[5 seconds later...]
TestTrafficGen: Sent test message #2 to ICS pipeline
...
```

**✅ SUCCESS: ICS pipeline processes messages independently of Linux VM!**

---

## Test 3: Concurrent Operation

**Validate both systems run simultaneously:**

1. **Terminal 1:** Linux VM console - run `nc -l -p 502`
2. **Terminal 2:** Send packets - `echo "TEST" | nc localhost 8502` (every 2 seconds)
3. **QEMU console:** Watch ICS pipeline messages every 5 seconds

**Expected:** Both Linux VM receives packets AND ICS pipeline processes messages

**✅ SUCCESS: VM and components coexist without interference!**

---

## Test 4: Advanced - Send Multiple Protocols

```bash
# Modbus TCP (port 502)
echo "MODBUS_TEST" | nc localhost 8502

# DNP3 (port 20000) - add hostfwd to QEMU args first
# EtherNet/IP (port 44818) - add hostfwd to QEMU args first
```

---

## Troubleshooting

### Issue 1: VM doesn't boot
**Symptom:** Kernel panic or no Linux boot messages

**Solution:**
```bash
# Check Linux images exist
ls -la /home/iamfo470/phd/camkes-vm-examples/projects/camkes-vm-linux/linux-artifacts/qemu-arm-virt/

# Should see:
# - linux (kernel)
# - linux-dtb (device tree)
# - rootfs.cpio.gz (filesystem)

# If missing, rebuild camkes-vm-linux
cd /home/iamfo470/phd/camkes-vm-examples
repo sync
cd build
ninja
```

### Issue 2: No eth0 in Linux VM
**Symptom:** `ifconfig -a` shows no eth0

**Solution:**
```bash
# Check QEMU args include VirtIO network
./simulate --extra-qemu-args="-nic user,model=virtio"

# Verify in Linux kernel boot messages:
dmesg | grep virtio
# Should show: virtio_net virtio0 eth0: ...
```

### Issue 3: netcat times out
**Symptom:** `nc localhost 8502` hangs

**Solution:**
```bash
# Check port forwarding in QEMU args
--extra-qemu-args="-nic user,model=virtio,hostfwd=tcp::8502-:502"

# Ensure Linux VM eth0 is configured
ifconfig eth0 192.168.1.10 up

# Ensure netcat is listening
nc -l -p 502
```

### Issue 4: ICS components don't start
**Symptom:** No TestTrafficGen messages

**Solution:**
```bash
# Check serial output for component errors
# Components should print initialization messages

# If missing, check CMakeLists.txt declarations
grep DeclareCAmkESComponent CMakeLists.txt
```

---

## Success Criteria Checklist

- [ ] Build completes without errors
- [ ] Linux VM boots to login prompt
- [ ] `ifconfig -a` shows eth0 interface
- [ ] `echo "TEST" | nc localhost 8502` reaches Linux VM
- [ ] TestTrafficGen sends messages every 5 seconds
- [ ] ICS pipeline components process messages
- [ ] Both systems run concurrently without crashes

---

## Next Steps (Phase 2)

Once Phase 1 PoC is validated:

1. **Add Cross-VM Dataport** (vm_echo_connector pattern)
   - Connect Linux VM → NetworkDriverDrv
   - Forward VirtIO packets to ICS pipeline

2. **Remove TestTrafficGen**
   - Replace with NetworkDriverDrv receiving from VM

3. **End-to-End Flow**
   ```
   netcat → QEMU VirtIO → Linux VM → Cross-VM Dataport →
   NetworkDriverDrv → ExtFrontend → ParserNorm → PolicyEmit → IntNicDrv
   ```

---

## Debug Mode

To see detailed VM boot messages:
```bash
./simulate --extra-qemu-args="-nic user,model=virtio,hostfwd=tcp::8502-:502 -serial mon:stdio"
```

To enable component debug output, edit component .c files and add:
```c
#define DEBUG_ENABLED 1
```

---

## Quick Reference Commands

**Build:**
```bash
cd /home/iamfo470/phd/camkes-vm-examples/build
ninja
```

**Run:**
```bash
cd /home/iamfo470/phd/camkes-vm-examples/ics_oneway_norm_v3
./simulate --extra-qemu-args="-nic user,model=virtio,hostfwd=tcp::8502-:502"
```

**Test:**
```bash
# In VM
nc -l -p 502

# From host
echo "TEST" | nc localhost 8502
```

---

**Version:** 1.0
**Date:** 2025-10-03
**Status:** Phase 1 PoC - Linux VM + ICS Components Coexistence
