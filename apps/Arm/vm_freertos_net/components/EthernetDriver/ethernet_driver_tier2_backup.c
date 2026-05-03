/*
 * EthernetDriver Component - Tier 1: Virtio Device Discovery Proof of Concept
 *
 * This is a simplified CAmkES port of sDDF's virtio ethernet driver
 * (from sDDF/drivers/network/virtio/ethernet.c)
 *
 * PURPOSE: Prove that CAmkES can discover and initialize the same
 *          virtio-net device that sDDF uses, without any sDDF queues.
 *
 * SUCCESS CRITERIA:
 * - Detect virtio MMIO device at 0xa003000
 * - Verify virtio version 2 (modern virtio 1.0+)
 * - Confirm device ID is network (0x01)
 * - Successfully complete device initialization handshake
 * - Read MAC address from device config
 * - Initialize RX and TX virtqueues
 */

#include <camkes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <utils/util.h>

/* ========================================================================
 * VIRTIO CONSTANTS (from sDDF sddf/virtio/virtio.h)
 * ======================================================================== */

#define VIRTIO_VERSION 0x2  /* Modern virtio 1.0+ */

/* Virtio Device IDs */
#define VIRTIO_DEVICE_ID_NET 0x1

/* Virtio MMIO Register Offsets (4.2.2.1 in spec) */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_MMIO_CONFIG              0x100

/* Virtio Device Status Bits (2.1) */
#define VIRTIO_DEVICE_STATUS_ACKNOWLEDGE    (1 << 0)
#define VIRTIO_DEVICE_STATUS_DRIVER         (1 << 1)
#define VIRTIO_DEVICE_STATUS_DRIVER_OK      (1 << 2)
#define VIRTIO_DEVICE_STATUS_FEATURES_OK    (1 << 3)
#define VIRTIO_DEVICE_STATUS_FAILED         (1 << 7)

/* Virtio Feature Bits */
#define VIRTIO_F_VERSION_1              (1ULL << 32)
#define VIRTIO_NET_F_MAC                (1ULL << 5)
#define VIRTIO_NET_F_STATUS             (1ULL << 16)

/* Virtio Queue Numbers */
#define VIRTIO_NET_RX_QUEUE 0
#define VIRTIO_NET_TX_QUEUE 1

#define VIRTIO_MMIO_IRQ_VQUEUE  (1 << 0)
#define VIRTIO_MMIO_IRQ_CONFIG  (1 << 1)

/* Virtqueue Descriptor Flags (from VirtIO spec) */
#define VIRTQ_DESC_F_NEXT       1  /* Descriptor continues via next field */
#define VIRTQ_DESC_F_WRITE      2  /* Buffer is write-only (device writes) */
#define VIRTQ_DESC_F_INDIRECT   4  /* Buffer contains list of descriptors */

/* Magic value for virtio MMIO transport */
#define VIRTIO_MMIO_MAGIC 0x74726976  /* "virt" in little-endian */

/* ========================================================================
 * VIRTIO MMIO REGISTER STRUCTURE
 * ======================================================================== */

typedef volatile struct {
    uint32_t MagicValue;            /* 0x000 */
    uint32_t Version;               /* 0x004 */
    uint32_t DeviceID;              /* 0x008 */
    uint32_t VendorID;              /* 0x00c */
    uint32_t DeviceFeatures;        /* 0x010 */
    uint32_t DeviceFeaturesSel;     /* 0x014 */
    uint32_t _reserved0[2];         /* 0x018-0x01c */
    uint32_t DriverFeatures;        /* 0x020 */
    uint32_t DriverFeaturesSel;     /* 0x024 */
    uint32_t _reserved1[2];         /* 0x028-0x02c */
    uint32_t QueueSel;              /* 0x030 */
    uint32_t QueueNumMax;           /* 0x034 */
    uint32_t QueueNum;              /* 0x038 */
    uint32_t _reserved2[2];         /* 0x03c-0x040 */
    uint32_t QueueReady;            /* 0x044 */
    uint32_t _reserved3[2];         /* 0x048-0x04c */
    uint32_t QueueNotify;           /* 0x050 */
    uint32_t _reserved4[3];         /* 0x054-0x05c */
    uint32_t InterruptStatus;       /* 0x060 */
    uint32_t InterruptACK;          /* 0x064 */
    uint32_t _reserved5[2];         /* 0x068-0x06c */
    uint32_t Status;                /* 0x070 */
    uint32_t _reserved6[3];         /* 0x074-0x07c */
    uint32_t QueueDescLow;          /* 0x080 */
    uint32_t QueueDescHigh;         /* 0x084 */
    uint32_t _reserved7[2];         /* 0x088-0x08c */
    uint32_t QueueDriverLow;        /* 0x090 (was QueueAvailLow) */
    uint32_t QueueDriverHigh;       /* 0x094 (was QueueAvailHigh) */
    uint32_t _reserved8[2];         /* 0x098-0x09c */
    uint32_t QueueDeviceLow;        /* 0x0a0 (was QueueUsedLow) */
    uint32_t QueueDeviceHigh;       /* 0x0a4 (was QueueUsedHigh) */
    uint32_t _reserved9[21];        /* 0x0a8-0x0f8 */
    uint32_t ConfigGeneration;      /* 0x0fc */
    uint32_t Config[0];             /* 0x100+ */
} virtio_mmio_regs_t;

/* Virtio Net Configuration Space (5.1.4) */
typedef struct {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed)) virtio_net_config_t;

/* Virtqueue Descriptor */
struct virtq_desc {
    uint64_t addr;   /* Physical address */
    uint32_t len;    /* Length */
    uint16_t flags;  /* Flags */
    uint16_t next;   /* Next descriptor index */
} __attribute__((packed));

/* Virtqueue Available Ring */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

/* Virtqueue Used Ring Element */
struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

/* Virtqueue Used Ring */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

/* Virtqueue Structure */
struct virtq {
    uint32_t num;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
};

/* ========================================================================
 * COMPONENT STATE
 * ======================================================================== */

#define COMPONENT_NAME "EthernetDriver"

#define RX_COUNT 512
#define TX_COUNT 512

/* Virtio MMIO registers (mapped from dataport) */
static volatile virtio_mmio_regs_t *regs;

/* Virtqueue structures */
static struct virtq rx_virtq;
static struct virtq tx_virtq;

/* Hardware ring buffer for virtqueues */
static uintptr_t hw_ring_buffer_vaddr;
static uintptr_t hw_ring_buffer_paddr;

/* Packet buffers (Tier 2) */
#define PACKET_BUFFER_SIZE 2048  /* Standard MTU + headers */
#define MAX_PACKETS 32           /* Number of packet buffers */

static uint8_t packet_buffers[MAX_PACKETS][PACKET_BUFFER_SIZE] __attribute__((aligned(64)));
static bool rx_buffer_used[MAX_PACKETS];  /* Track which RX buffers are in use */

/* Packet statistics */
static uint32_t packets_received = 0;
static uint32_t packets_sent = 0;
static uint32_t packets_dropped = 0;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static int get_free_rx_buffer(void);
static void refill_rx_queue(void);
static bool send_packet(uint8_t *data, uint32_t len);
static void process_rx_packets(void);

/* ========================================================================
 * UTILITY FUNCTIONS
 * ======================================================================== */

static void print_separator(void)
{
    printf("════════════════════════════════════════════════════════════\n");
}

static void print_box_top(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
}

static void print_box_bottom(void)
{
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

static void print_mac_address(const uint8_t *mac)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ========================================================================
 * VIRTIO DEVICE DISCOVERY & INITIALIZATION
 * ======================================================================== */

/**
 * virtio_check_magic - Verify virtio MMIO magic value
 *
 * Returns: true if magic value is correct ("virt")
 */
static bool virtio_check_magic(void)
{
    uint32_t magic = regs->MagicValue;

    printf("%s: Checking virtio magic value...\n", COMPONENT_NAME);
    printf("%s:   regs pointer: %p\n", COMPONENT_NAME, (void*)regs);

    /* Debug: Dump first 16 bytes as raw uint32_t values */
    uint32_t *raw = (uint32_t*)regs;
    printf("%s:   Raw MMIO dump:\n", COMPONENT_NAME);
    printf("%s:     [0x00] = 0x%08x (MagicValue)\n", COMPONENT_NAME, raw[0]);
    printf("%s:     [0x04] = 0x%08x (Version)\n", COMPONENT_NAME, raw[1]);
    printf("%s:     [0x08] = 0x%08x (DeviceID - 1=net, 2=blk, 0=none)\n", COMPONENT_NAME, raw[2]);
    printf("%s:     [0x0c] = 0x%08x (VendorID)\n", COMPONENT_NAME, raw[3]);

    /* QEMU quirk: The -device virtio-net-device may be at a different slot */
    /* Let's check the next few slots if this one is empty (DeviceID=0) */
    if (raw[2] == 0 && raw[0] == VIRTIO_MMIO_MAGIC) {
        printf("%s:   ⚠ Slot 0 is empty, checking next slots...\n", COMPONENT_NAME);
        for (int slot = 1; slot < 8; slot++) {
            /* Each slot is 0x200 bytes apart, but we mapped 4KB so we can only check within our page */
            if (slot * 0x200 >= 0x1000) break;

            uint32_t *slot_base = (uint32_t*)((uintptr_t)regs + (slot * 0x200));
            uint32_t slot_magic = slot_base[0];
            uint32_t slot_devid = slot_base[2];

            printf("%s:     Slot %d (@+0x%x): Magic=0x%08x, DeviceID=%d\n",
                   COMPONENT_NAME, slot, slot * 0x200, slot_magic, slot_devid);

            if (slot_magic == VIRTIO_MMIO_MAGIC && slot_devid == VIRTIO_DEVICE_ID_NET) {
                printf("%s:   ✓ Found network device in slot %d!\n", COMPONENT_NAME, slot);
                printf("%s:   NOTE: CAmkES mapped slot 0, but device is in slot %d\n", COMPONENT_NAME, slot);
                printf("%s:   You need to update vm_ethernet_test.camkes to map the correct slot.\n", COMPONENT_NAME);
                break;
            }
        }
    }

    printf("%s:   Expected: 0x%08x (\"virt\")\n", COMPONENT_NAME, VIRTIO_MMIO_MAGIC);
    printf("%s:   Read:     0x%08x\n", COMPONENT_NAME, magic);

    if (magic == VIRTIO_MMIO_MAGIC) {
        printf("%s:   ✓ Magic value correct!\n", COMPONENT_NAME);
        return true;
    } else {
        printf("%s:   ✗ MAGIC VALUE MISMATCH!\n", COMPONENT_NAME);
        return false;
    }
}

/**
 * virtio_check_version - Verify virtio version
 *
 * Returns: true if version is 2 (modern virtio 1.0+)
 */
static bool virtio_check_version(void)
{
    uint32_t version = regs->Version;

    printf("%s: Checking virtio version...\n", COMPONENT_NAME);
    printf("%s:   Expected: 0x%x (modern virtio 1.0+)\n", COMPONENT_NAME, VIRTIO_VERSION);
    printf("%s:   Read:     0x%x\n", COMPONENT_NAME, version);

    if (version == VIRTIO_VERSION) {
        printf("%s:   ✓ Version correct!\n", COMPONENT_NAME);
        return true;
    } else {
        printf("%s:   ✗ VERSION MISMATCH!\n", COMPONENT_NAME);
        printf("%s:   This likely means QEMU was started without:\n", COMPONENT_NAME);
        printf("%s:   -global virtio-mmio.force-legacy=false\n", COMPONENT_NAME);
        return false;
    }
}

/**
 * virtio_check_device_id - Verify device is a network device
 *
 * Returns: true if device ID is 0x1 (network)
 */
static bool virtio_check_device_id(void)
{
    uint32_t device_id = regs->DeviceID;
    uint32_t vendor_id = regs->VendorID;

    printf("%s: Checking device type...\n", COMPONENT_NAME);
    printf("%s:   Device ID: 0x%x\n", COMPONENT_NAME, device_id);
    printf("%s:   Vendor ID: 0x%x (QEMU)\n", COMPONENT_NAME, vendor_id);

    if (device_id == VIRTIO_DEVICE_ID_NET) {
        printf("%s:   ✓ Device is virtio-net!\n", COMPONENT_NAME);
        return true;
    } else {
        printf("%s:   ✗ NOT A NETWORK DEVICE!\n", COMPONENT_NAME);
        return false;
    }
}

/**
 * virtio_read_features - Read device features
 */
static void virtio_read_features(void)
{
    printf("%s: Reading device features...\n", COMPONENT_NAME);

    /* Read lower 32 bits */
    regs->DeviceFeaturesSel = 0;
    uint32_t features_low = regs->DeviceFeatures;

    /* Read upper 32 bits */
    regs->DeviceFeaturesSel = 1;
    uint32_t features_high = regs->DeviceFeatures;

    uint64_t features = features_low | ((uint64_t)features_high << 32);

    printf("%s:   Features: 0x%016llx\n", COMPONENT_NAME, (unsigned long long)features);
    printf("%s:   - VIRTIO_F_VERSION_1: %s\n", COMPONENT_NAME,
           (features & VIRTIO_F_VERSION_1) ? "YES" : "NO");
    printf("%s:   - VIRTIO_NET_F_MAC:   %s\n", COMPONENT_NAME,
           (features & VIRTIO_NET_F_MAC) ? "YES" : "NO");
    printf("%s:   - VIRTIO_NET_F_STATUS: %s\n", COMPONENT_NAME,
           (features & VIRTIO_NET_F_STATUS) ? "YES" : "NO");
}

/**
 * virtio_negotiate_features - Negotiate driver features
 */
static bool virtio_negotiate_features(void)
{
    printf("%s: Negotiating features...\n", COMPONENT_NAME);

    /* We want MAC address and version 1 support */
    regs->DriverFeaturesSel = 0;
    regs->DriverFeatures = VIRTIO_NET_F_MAC;

    regs->DriverFeaturesSel = 1;
    regs->DriverFeatures = VIRTIO_F_VERSION_1;

    /* Signal features OK */
    regs->Status = VIRTIO_DEVICE_STATUS_FEATURES_OK;

    /* Device should accept our features */
    if (regs->Status & VIRTIO_DEVICE_STATUS_FEATURES_OK) {
        printf("%s:   ✓ Feature negotiation successful!\n", COMPONENT_NAME);
        return true;
    } else {
        printf("%s:   ✗ FEATURE NEGOTIATION FAILED!\n", COMPONENT_NAME);
        return false;
    }
}

/**
 * virtio_read_mac_address - Read MAC address from device config
 */
static void virtio_read_mac_address(void)
{
    volatile virtio_net_config_t *config = (virtio_net_config_t *)regs->Config;

    printf("%s: Reading MAC address from device...\n", COMPONENT_NAME);
    printf("%s:   MAC: ", COMPONENT_NAME);
    print_mac_address(config->mac);
    printf("\n");

    if (config->status & 0x1) {
        printf("%s:   Status: Link UP\n", COMPONENT_NAME);
    } else {
        printf("%s:   Status: Link DOWN\n", COMPONENT_NAME);
    }
}

/**
 * virtio_setup_virtqueue - Setup a single virtqueue
 *
 * This initializes the descriptor table, available ring, and used ring
 * for either RX or TX virtqueue.
 */
static bool virtio_setup_virtqueue(uint32_t queue_id, struct virtq *vq, uint32_t queue_size)
{
    printf("%s: Setting up virtqueue %u...\n", COMPONENT_NAME, queue_id);

    /* Select the queue */
    regs->QueueSel = queue_id;

    /* Check max queue size */
    uint32_t max_size = regs->QueueNumMax;
    printf("%s:   Max queue size: %u\n", COMPONENT_NAME, max_size);

    if (queue_size > max_size) {
        printf("%s:   ✗ Requested size %u > max %u!\n", COMPONENT_NAME, queue_size, max_size);
        return false;
    }

    /* Set queue size */
    regs->QueueNum = queue_size;
    vq->num = queue_size;

    /* Calculate offsets within hw_ring_buffer */
    /* Layout: [RX desc|RX avail|RX used|TX desc|TX avail|TX used] */

    size_t desc_size = 16 * queue_size;      /* Each descriptor is 16 bytes */
    size_t avail_size = 6 + 2 * queue_size;  /* Header + ring entries */
    size_t used_size = 6 + 8 * queue_size;   /* Header + ring entries */

    size_t offset = 0;
    if (queue_id == VIRTIO_NET_TX_QUEUE) {
        /* TX queue comes after RX queue */
        size_t rx_desc = 16 * RX_COUNT;
        size_t rx_avail = ALIGN_UP(6 + 2 * RX_COUNT, 2);
        size_t rx_used = ALIGN_UP(6 + 8 * RX_COUNT, 4);
        offset = ALIGN_UP(rx_desc + rx_avail + rx_used, 16);
    }

    size_t desc_off = offset;
    size_t avail_off = ALIGN_UP(desc_off + desc_size, 2);
    size_t used_off = ALIGN_UP(avail_off + avail_size, 4);

    /* Setup virtual addresses */
    vq->desc = (struct virtq_desc *)(hw_ring_buffer_vaddr + desc_off);
    vq->avail = (struct virtq_avail *)(hw_ring_buffer_vaddr + avail_off);
    vq->used = (struct virtq_used *)(hw_ring_buffer_vaddr + used_off);

    /* Setup physical addresses (tell device where rings are) */
    uint64_t desc_paddr = hw_ring_buffer_paddr + desc_off;
    uint64_t avail_paddr = hw_ring_buffer_paddr + avail_off;
    uint64_t used_paddr = hw_ring_buffer_paddr + used_off;

    regs->QueueDescLow = (uint32_t)(desc_paddr & 0xFFFFFFFF);
    regs->QueueDescHigh = (uint32_t)(desc_paddr >> 32);
    regs->QueueDriverLow = (uint32_t)(avail_paddr & 0xFFFFFFFF);
    regs->QueueDriverHigh = (uint32_t)(avail_paddr >> 32);
    regs->QueueDeviceLow = (uint32_t)(used_paddr & 0xFFFFFFFF);
    regs->QueueDeviceHigh = (uint32_t)(used_paddr >> 32);

    printf("%s:   Descriptor ring: vaddr=0x%lx, paddr=0x%lx\n",
           COMPONENT_NAME, (unsigned long)vq->desc, (unsigned long)desc_paddr);
    printf("%s:   Available ring:  vaddr=0x%lx, paddr=0x%lx\n",
           COMPONENT_NAME, (unsigned long)vq->avail, (unsigned long)avail_paddr);
    printf("%s:   Used ring:       vaddr=0x%lx, paddr=0x%lx\n",
           COMPONENT_NAME, (unsigned long)vq->used, (unsigned long)used_paddr);

    /* Enable the queue */
    regs->QueueReady = 1;

    if (regs->QueueReady == 1) {
        printf("%s:   ✓ Virtqueue %u ready!\n", COMPONENT_NAME, queue_id);
        return true;
    } else {
        printf("%s:   ✗ Failed to enable virtqueue %u!\n", COMPONENT_NAME, queue_id);
        return false;
    }
}

/**
 * virtio_device_init - Complete virtio device initialization sequence
 *
 * This follows the virtio spec section 3.1.1 (Device Initialization)
 */
static bool virtio_device_init(void)
{
    printf("\n");
    print_box_top();
    printf("║     VirtIO Device Initialization Sequence               ║\n");
    print_box_bottom();
    printf("\n");

    /* Step 1: Check device is present and valid */
    printf("%s: [Step 1/8] Device Detection\n", COMPONENT_NAME);
    print_separator();

    if (!virtio_check_magic()) {
        return false;
    }

    if (!virtio_check_version()) {
        return false;
    }

    if (!virtio_check_device_id()) {
        return false;
    }

    printf("\n");

    /* Step 2: Reset device */
    printf("%s: [Step 2/8] Resetting device\n", COMPONENT_NAME);
    print_separator();
    regs->Status = 0;
    printf("%s:   ✓ Device reset complete\n", COMPONENT_NAME);
    printf("\n");

    /* Step 3: Set ACKNOWLEDGE bit */
    printf("%s: [Step 3/8] Acknowledging device\n", COMPONENT_NAME);
    print_separator();
    regs->Status = VIRTIO_DEVICE_STATUS_ACKNOWLEDGE;
    printf("%s:   ✓ Device acknowledged\n", COMPONENT_NAME);
    printf("\n");

    /* Step 4: Set DRIVER bit */
    printf("%s: [Step 4/8] Setting DRIVER bit\n", COMPONENT_NAME);
    print_separator();
    regs->Status = VIRTIO_DEVICE_STATUS_DRIVER;
    printf("%s:   ✓ Driver status set\n", COMPONENT_NAME);
    printf("\n");

    /* Step 5: Read and negotiate features */
    printf("%s: [Step 5/8] Feature Negotiation\n", COMPONENT_NAME);
    print_separator();
    virtio_read_features();
    if (!virtio_negotiate_features()) {
        return false;
    }
    printf("\n");

    /* Step 6: Read device configuration */
    printf("%s: [Step 6/8] Reading Device Configuration\n", COMPONENT_NAME);
    print_separator();
    virtio_read_mac_address();
    printf("\n");

    /* Step 7: Setup virtqueues */
    printf("%s: [Step 7/8] Setting up Virtqueues\n", COMPONENT_NAME);
    print_separator();

    if (!virtio_setup_virtqueue(VIRTIO_NET_RX_QUEUE, &rx_virtq, RX_COUNT)) {
        return false;
    }

    if (!virtio_setup_virtqueue(VIRTIO_NET_TX_QUEUE, &tx_virtq, TX_COUNT)) {
        return false;
    }
    printf("\n");

    /* Step 8: Set DRIVER_OK */
    printf("%s: [Step 8/8] Finalizing Initialization\n", COMPONENT_NAME);
    print_separator();
    regs->Status = VIRTIO_DEVICE_STATUS_DRIVER_OK;
    printf("%s:   ✓ Device initialization complete!\n", COMPONENT_NAME);
    printf("%s:   ✓ Device is now LIVE and ready for I/O\n", COMPONENT_NAME);
    printf("\n");

    return true;
}

/* ========================================================================
 * INTERRUPT HANDLING (Future)
 * ======================================================================== */

/**
 * virtio_irq_handle - Handle virtio interrupt (placeholder)
 */
void virtio_irq_handle(void)
{
    uint32_t irq_status = regs->InterruptStatus;

    if (irq_status & VIRTIO_MMIO_IRQ_VQUEUE) {
        /* Process RX queue - echo packets back */
        process_rx_packets();
        regs->InterruptACK = VIRTIO_MMIO_IRQ_VQUEUE;
    }

    if (irq_status & VIRTIO_MMIO_IRQ_CONFIG) {
        printf("%s: ⚡ IRQ: Configuration changed\n", COMPONENT_NAME);
        regs->InterruptACK = VIRTIO_MMIO_IRQ_CONFIG;
    }

    virtio_irq_acknowledge();
}

/* ========================================================================
 * TIER 2: PACKET RX/TX IMPLEMENTATION
 * ======================================================================== */

/**
 * get_free_rx_buffer - Find a free RX buffer slot
 * Returns: buffer index, or -1 if none available
 */
static int get_free_rx_buffer(void)
{
    for (int i = 0; i < MAX_PACKETS; i++) {
        if (!rx_buffer_used[i]) {
            rx_buffer_used[i] = true;
            return i;
        }
    }
    return -1;
}

/**
 * refill_rx_queue - Add RX buffers to the receive queue
 */
static void refill_rx_queue(void)
{
    struct virtq *vq = &rx_virtq;

    /* Fill RX queue with available buffers */
    for (int i = 0; i < MAX_PACKETS; i++) {
        if (rx_buffer_used[i]) continue;  /* Already in use */

        int buf_idx = get_free_rx_buffer();
        if (buf_idx < 0) break;

        /* Find next free descriptor */
        uint16_t desc_idx = i;  /* Simplified: use buffer index as descriptor index */
        if (desc_idx >= vq->num) break;

        /* Setup descriptor for RX buffer */
        vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)&packet_buffers[buf_idx][0];
        vq->desc[desc_idx].len = PACKET_BUFFER_SIZE;
        vq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;  /* Device writes to this buffer */
        vq->desc[desc_idx].next = 0;

        /* Add to available ring */
        uint16_t avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = desc_idx;
        __sync_synchronize();  /* Memory barrier */
        vq->avail->idx++;
    }

    /* Notify device of new RX buffers */
    regs->QueueNotify = VIRTIO_NET_RX_QUEUE;
}

/**
 * send_packet - Send a packet via TX queue
 */
static bool send_packet(uint8_t *data, uint32_t len)
{
    if (len > PACKET_BUFFER_SIZE) {
        printf("%s: Packet too large: %u bytes\n", COMPONENT_NAME, len);
        packets_dropped++;
        return false;
    }

    struct virtq *vq = &tx_virtq;

    /* Find next free TX descriptor (simplified: use round-robin) */
    static uint16_t next_tx_desc = 0;
    uint16_t desc_idx = next_tx_desc;
    next_tx_desc = (next_tx_desc + 1) % vq->num;

    /* Copy packet data to descriptor buffer */
    /* For simplicity, use packet_buffers for TX as well */
    int tx_buf_idx = (desc_idx + MAX_PACKETS/2) % MAX_PACKETS;
    memcpy(&packet_buffers[tx_buf_idx][0], data, len);

    /* Setup descriptor */
    vq->desc[desc_idx].addr = (uint64_t)(uintptr_t)&packet_buffers[tx_buf_idx][0];
    vq->desc[desc_idx].len = len;
    vq->desc[desc_idx].flags = 0;  /* Device reads from this buffer */
    vq->desc[desc_idx].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = desc_idx;
    __sync_synchronize();  /* Memory barrier */
    vq->avail->idx++;

    /* Notify device */
    regs->QueueNotify = VIRTIO_NET_TX_QUEUE;

    packets_sent++;
    return true;
}

/**
 * process_rx_packets - Process received packets from RX queue
 *
 * For Tier 2 echo server: Simply echo packets back via TX queue
 */
static void process_rx_packets(void)
{
    struct virtq *vq = &rx_virtq;
    static uint16_t last_used_idx = 0;

    /* Check if there are any used buffers */
    if (vq->used->idx == last_used_idx) {
        return;  /* No new packets */
    }

    /* Process all new packets */
    while (last_used_idx != vq->used->idx) {
        uint16_t used_ring_idx = last_used_idx % vq->num;
        struct virtq_used_elem *used_elem = &vq->used->ring[used_ring_idx];

        uint16_t desc_idx = used_elem->id;
        uint32_t len = used_elem->len;

        /* Get the packet data */
        uint8_t *packet_data = (uint8_t *)(uintptr_t)vq->desc[desc_idx].addr;

        packets_received++;

        printf("%s: 📥 RX packet #%u: %u bytes\n", COMPONENT_NAME, packets_received, len);

        /* Echo: Send the packet back */
        if (send_packet(packet_data, len)) {
            printf("%s: 📤 TX echo #%u: %u bytes\n", COMPONENT_NAME, packets_sent, len);
        } else {
            printf("%s: ❌ Failed to echo packet\n", COMPONENT_NAME);
        }

        /* Mark buffer as free */
        int buf_idx = (desc_idx < MAX_PACKETS) ? desc_idx : 0;
        rx_buffer_used[buf_idx] = false;

        last_used_idx++;
    }

    /* Refill RX queue with fresh buffers */
    refill_rx_queue();
}

/* ========================================================================
 * COMPONENT INITIALIZATION
 * ======================================================================== */

/**
 * run - Component entry point (CAmkES standard)
 */
int run(void)
{
    printf("\n");
    print_box_top();
    printf("║         EthernetDriver Component - Tier 2               ║\n");
    printf("║      VirtIO Echo Server - Packet RX/TX Test             ║\n");
    printf("║              (CAmkES Port of sDDF Driver)                ║\n");
    print_box_bottom();
    printf("\n");

    printf("%s: Component started\n", COMPONENT_NAME);
    printf("\n");

    /* Map hardware resources */
    printf("%s: Mapping hardware resources...\n", COMPONENT_NAME);
    print_separator();

    /* Get virtio MMIO registers from dataport */
    /* NOTE: QEMU assigns virtio-net to slot 31 at offset 0xe00 within the page */
    /* CAmkES maps the page-aligned address (0xa003000), so we add the offset */
    regs = (volatile virtio_mmio_regs_t *)((uintptr_t)virtio_mmio_regs + 0xe00);
    printf("%s:   VirtIO MMIO regs: vaddr=0x%lx (page base + 0xe00 offset)\n",
           COMPONENT_NAME, (unsigned long)regs);

    /* Allocate hardware ring buffer from component's heap */
    /* In a full implementation, this would be a proper DMA buffer */
    /* For Tier 1 proof of concept, we just allocate from heap */
    static uint8_t hw_buffer[0x10000] __attribute__((aligned(4096)));
    hw_ring_buffer_vaddr = (uintptr_t)hw_buffer;

    /* For physical address, we need to use virtual address for now */
    /* This works in QEMU because it doesn't enforce DMA constraints */
    /* TODO Tier 2: Use proper DMA allocation */
    hw_ring_buffer_paddr = hw_ring_buffer_vaddr;

    printf("%s:   HW ring buffer:   vaddr=0x%x, paddr=0x%x (allocated from heap)\n",
           COMPONENT_NAME, (unsigned int)hw_ring_buffer_vaddr, (unsigned int)hw_ring_buffer_paddr);

    /* Clear ring buffer */
    memset((void *)hw_ring_buffer_vaddr, 0, 0x10000);
    printf("%s:   ✓ Ring buffer cleared\n", COMPONENT_NAME);
    printf("\n");

    /* Initialize virtio device */
    bool success = virtio_device_init();

    if (success) {
        print_box_top();
        printf("║                                                          ║\n");
        printf("║              ✓✓✓ SUCCESS! ✓✓✓                          ║\n");
        printf("║                                                          ║\n");
        printf("║  VirtIO network device successfully initialized!        ║\n");
        printf("║                                                          ║\n");
        printf("║  Tier 2: Echo server ready                              ║\n");
        printf("║  All received packets will be echoed back               ║\n");
        printf("║                                                          ║\n");
        print_box_bottom();
        printf("\n");

        /* Initialize RX buffers and fill RX queue */
        printf("%s: Initializing packet buffers...\n", COMPONENT_NAME);
        memset(rx_buffer_used, 0, sizeof(rx_buffer_used));
        refill_rx_queue();
        printf("%s:   ✓ RX queue filled with %d buffers\n", COMPONENT_NAME, MAX_PACKETS);
        printf("\n");

        printf("%s: Echo server is LIVE! Waiting for packets...\n", COMPONENT_NAME);
        printf("%s: Statistics: RX=%u TX=%u Dropped=%u\n", COMPONENT_NAME,
               packets_received, packets_sent, packets_dropped);
        printf("\n");

        return 0;
    } else {
        print_box_top();
        printf("║                                                          ║\n");
        printf("║              ✗✗✗ FAILURE ✗✗✗                           ║\n");
        printf("║                                                          ║\n");
        printf("║  VirtIO device initialization failed!                   ║\n");
        printf("║                                                          ║\n");
        printf("║  Check:                                                  ║\n");
        printf("║  1. QEMU started with correct flags                     ║\n");
        printf("║  2. Memory mappings are correct                         ║\n");
        printf("║  3. IRQ routing is configured                           ║\n");
        printf("║                                                          ║\n");
        print_box_bottom();
        printf("\n");

        return 1;
    }
}
