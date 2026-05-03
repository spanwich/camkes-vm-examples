#!/usr/bin/env python3
"""
test_packet_sender.py - Simple packet sender for testing Tier 2 echo server

This script sends raw Ethernet frames to test the echo server.
Works with QEMU socket networking backend.

Usage:
    # In terminal 1: Run QEMU with socket backend
    ./test_echo.sh
    # Select option 2 (socket backend)

    # In terminal 2: Send test packets
    python3 test_packet_sender.py
"""

import socket
import struct
import sys
import time

def create_ethernet_frame(dst_mac, src_mac, ethertype, payload):
    """Create a simple Ethernet frame"""
    # Convert MAC addresses from string to bytes
    dst = bytes.fromhex(dst_mac.replace(':', ''))
    src = bytes.fromhex(src_mac.replace(':', ''))

    # Ethertype (2 bytes)
    etype = struct.pack('!H', ethertype)

    # Combine into frame
    frame = dst + src + etype + payload

    return frame

def send_test_packet_socket(host='localhost', port=1234):
    """Send test packet via QEMU socket backend"""

    print("Connecting to QEMU socket backend...")
    print(f"  Host: {host}")
    print(f"  Port: {port}")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("✓ Connected!")
        print()

        # Create test frame
        dst_mac = "52:54:00:12:34:56"  # QEMU virtio-net MAC
        src_mac = "00:11:22:33:44:55"  # Test source MAC
        ethertype = 0x0800  # IPv4 (though we're not using real IP)
        payload = b"Hello from test script! This is a test packet."

        frame = create_ethernet_frame(dst_mac, src_mac, ethertype, payload)

        print(f"Sending test frame:")
        print(f"  Destination MAC: {dst_mac}")
        print(f"  Source MAC:      {src_mac}")
        print(f"  Ethertype:       0x{ethertype:04x}")
        print(f"  Payload:         {payload.decode('ascii')}")
        print(f"  Frame size:      {len(frame)} bytes")
        print()

        # Send the frame
        sock.send(frame)
        print("✓ Packet sent!")
        print()

        # Try to receive echo (with timeout)
        print("Waiting for echo response (5 second timeout)...")
        sock.settimeout(5.0)

        try:
            response = sock.recv(2048)
            print(f"✓ Received echo: {len(response)} bytes")
            print()

            # Parse response
            if len(response) >= 14:
                resp_dst = ':'.join(f'{b:02x}' for b in response[0:6])
                resp_src = ':'.join(f'{b:02x}' for b in response[6:12])
                resp_type = struct.unpack('!H', response[12:14])[0]
                resp_payload = response[14:]

                print(f"Echo response details:")
                print(f"  Destination MAC: {resp_dst}")
                print(f"  Source MAC:      {resp_src}")
                print(f"  Ethertype:       0x{resp_type:04x}")
                print(f"  Payload:         {resp_payload[:50]}")  # First 50 bytes
                print()

                # Verify it's an echo
                if response == frame:
                    print("✓✓✓ SUCCESS! Packet was echoed correctly!")
                else:
                    print("⚠ Packet was echoed but content differs")
            else:
                print(f"⚠ Response too short: {len(response)} bytes")

        except socket.timeout:
            print("✗ No echo received (timeout)")
            print()
            print("This could mean:")
            print("  - Echo server hasn't processed the packet yet")
            print("  - Packet didn't reach the virtio-net device")
            print("  - Interrupts aren't working")
            print("  - Check QEMU output for RX/TX messages")

        sock.close()

    except ConnectionRefusedError:
        print("✗ Connection refused!")
        print()
        print("Make sure QEMU is running with socket backend:")
        print("  ./test_echo.sh")
        print("  Select option 2")
        return 1

    except Exception as e:
        print(f"✗ Error: {e}")
        return 1

    return 0

def send_test_packet_tap(interface='tap0'):
    """Send test packet via TAP interface (requires root)"""

    try:
        import fcntl
        import os

        print(f"Opening TAP interface: {interface}")

        # This requires root and proper TAP setup
        TUNSETIFF = 0x400454ca
        IFF_TAP = 0x0002
        IFF_NO_PI = 0x1000

        tun = open('/dev/net/tun', 'r+b', buffering=0)
        ifr = struct.pack('16sH', interface.encode(), IFF_TAP | IFF_NO_PI)
        fcntl.ioctl(tun, TUNSETIFF, ifr)

        print(f"✓ Opened {interface}")
        print()

        # Create test frame
        dst_mac = "52:54:00:12:34:56"  # QEMU virtio-net MAC
        src_mac = "00:11:22:33:44:55"  # Test source MAC
        ethertype = 0x0800  # IPv4
        payload = b"Hello from TAP interface!"

        frame = create_ethernet_frame(dst_mac, src_mac, ethertype, payload)

        print(f"Sending test frame via {interface}:")
        print(f"  Frame size: {len(frame)} bytes")
        print()

        os.write(tun.fileno(), frame)
        print("✓ Packet sent!")

        # Try to read echo
        print("Waiting for echo...")
        tun_fd = tun.fileno()

        import select
        ready = select.select([tun_fd], [], [], 5.0)

        if ready[0]:
            response = os.read(tun_fd, 2048)
            print(f"✓ Received echo: {len(response)} bytes")

            if response == frame:
                print("✓✓✓ SUCCESS! Packet was echoed correctly!")
            else:
                print("⚠ Packet was echoed but content differs")
        else:
            print("✗ No echo received (timeout)")

        tun.close()

    except PermissionError:
        print("✗ Permission denied!")
        print("TAP interface access requires root privileges.")
        print("Run with: sudo python3 test_packet_sender.py tap")
        return 1

    except Exception as e:
        print(f"✗ Error: {e}")
        return 1

    return 0

def main():
    print("╔══════════════════════════════════════════════════════════╗")
    print("║    Tier 2 Echo Server - Packet Sender                   ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()

    if len(sys.argv) > 1 and sys.argv[1] == 'tap':
        print("Mode: TAP interface")
        print()
        return send_test_packet_tap()
    else:
        print("Mode: Socket backend")
        print()
        return send_test_packet_socket()

if __name__ == '__main__':
    sys.exit(main())
