#include <fcntl.h>
#include <i2c/smbus.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// I2C device address and bus
#define I2C_DEVICE_ADDR 0x26
#define MAX_I2C_BUS 20

// LED control registers
#define LED_ON_REG_0 0xA0
#define LED_OFF_REG_0 0xB0
#define LED_ON_REG_1 0xA1
#define LED_OFF_REG_1 0xB1

// Blink control registers
#define HDD0_BLINK_REG 0x52
#define HDD1_BLINK_REG 0x54
#define NETWORK_BLINK_REG 0x56
#define NVME0_BLINK_REG 0x58
#define NVME1_BLINK_REG 0x5A
#define NVME2_BLINK_REG 0x5C
#define NVME3_BLINK_REG 0x5E

// LED bit masks
#define HDD0_WHITE 0x04
#define HDD0_RED 0x08
#define HDD1_WHITE 0x10
#define HDD1_RED 0x20
#define NETWORK_WHITE 0x40
#define NETWORK_RED 0x80
#define NVME0_WHITE 0x01
#define NVME0_RED 0x02
#define NVME1_WHITE 0x04
#define NVME1_RED 0x08
#define NVME2_WHITE 0x10
#define NVME2_RED 0x20
#define NVME3_WHITE 0x40
#define NVME3_RED 0x80

// Activity sample interval in microseconds
#define ACTIVITY_SAMPLE_INTERVAL 1000000 // 1 second

// Cleanup retry settings
#define CLEANUP_MAX_RETRIES 3
#define CLEANUP_RETRY_DELAY 100000 // 100ms in microseconds

typedef struct {
    char device_name[32];
    unsigned long long prev_reads;
    unsigned long long prev_writes;
} disk_stats_t;

typedef struct {
    char interface_name[32];
    unsigned long long prev_rx_bytes;
    unsigned long long prev_tx_bytes;
    int is_active;
} network_stats_t;

// Global variables
static int i2c_fd = -1;
static int i2c_bus = -1;
static volatile int running = 1;
static int debug = 0;

// Function prototypes
int find_i2c_bus(void);
int init_i2c(void);
void cleanup_i2c(void);
int write_i2c_register(int reg, int value);
void set_led_state(int reg, int mask, int state);
void set_blink_state(int reg, int state);
int get_disk_status(disk_stats_t *disk);
int get_disk_health(const char* disk_name);
void update_network_led(network_stats_t *network);
int read_network_stats(network_stats_t *network);
void signal_handler(int signal);
void turn_off_all_leds(void);

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    if (debug) printf("\nReceived signal %d, shutting down...\n", signal);
    running = 0;
}

// Find the I2C bus that has the LED controller
int find_i2c_bus(void) {
    char filename[32];
    int fd;
    unsigned long funcs;

    for (int bus = 0; bus < MAX_I2C_BUS; bus++) {
        snprintf(filename, sizeof(filename), "/dev/i2c-%d", bus);
        fd = open(filename, O_RDWR);
        if (fd < 0) continue;

        // Verify this bus supports SMBus commands
        if (ioctl(fd, I2C_FUNCS, &funcs) < 0) {
            close(fd);
            continue;
        }

        // Check for basic SMBus byte data support
        if (!(funcs & I2C_FUNC_SMBUS_BYTE_DATA)) {
            close(fd);
            continue;
        }

        // Check if the device is not in use
        if (ioctl(fd, I2C_SLAVE, I2C_DEVICE_ADDR) < 0) {
            close(fd);
            continue;
        }

        // Try to read from the device to verify it exists
        if (i2c_smbus_read_byte_data(fd, 0x26) >= 0) {
            close(fd);
            if (debug) printf("Confirmed LED controller on SMBus %d\n", bus);
            return bus;
        }

        close(fd);
    }

    fprintf(stderr, "LED controller not found on any I2C bus\n");
    return -1;
}

// Initialize I2C communication
int init_i2c(void) {
    char filename[32];

    i2c_bus = find_i2c_bus();
    if (i2c_bus < 0) {
        return -1;
    }

    snprintf(filename, sizeof(filename), "/dev/i2c-%d", i2c_bus);
    i2c_fd = open(filename, O_RDWR);

    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        return -1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, I2C_DEVICE_ADDR) < 0) {
        perror("Failed to set I2C slave address");
        cleanup_i2c();
        return -1;
    }

    if (debug) printf("I2C initialized successfully on bus %d\n", i2c_bus);
    return 0;
}

// Cleanup I2C resources
void cleanup_i2c(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

// Write to I2C register using SMBus
int write_i2c_register(int reg, int value) {
    __s32 result = i2c_smbus_write_byte_data(i2c_fd, reg, value);

    if (result < 0) {
        perror("Failed to write to I2C device via SMBus");
        return -1;
    }

    return 0;
}

// Set LED state (on/off)
void set_led_state(int reg, int mask, int state) {
    if (state) {
        write_i2c_register(reg, mask);
    } else {
        write_i2c_register(reg + 0x10, mask);    // OFF register is +0x10 from ON register
    }
}

// Set blink state (on/off)
void set_blink_state(int reg, int state) {
    if (state) {
        write_i2c_register(reg, 0x01);
    } else {
        write_i2c_register(reg, 0x00);
    }
}

// Turn off all LEDs
void turn_off_all_leds(void) {
    if (debug) printf("Turning off all LEDs and disabling blinking...\n");

    // --- Turn off HDD and Network LEDs ---
    write_i2c_register(LED_OFF_REG_0, HDD0_WHITE | HDD0_RED);
    write_i2c_register(LED_OFF_REG_0, HDD1_WHITE | HDD1_RED);
    write_i2c_register(LED_OFF_REG_0, NETWORK_WHITE | NETWORK_RED);

    // --- Disable blinking for HDD and network ---
    write_i2c_register(HDD0_BLINK_REG, 0x00);
    write_i2c_register(HDD1_BLINK_REG, 0x00);
    write_i2c_register(NETWORK_BLINK_REG, 0x00);

    // --- Turn off NVMe LEDs ---
    write_i2c_register(LED_OFF_REG_1, NVME0_WHITE | NVME0_RED);
    write_i2c_register(LED_OFF_REG_1, NVME1_WHITE | NVME1_RED);
    write_i2c_register(LED_OFF_REG_1, NVME2_WHITE | NVME2_RED);
    write_i2c_register(LED_OFF_REG_1, NVME3_WHITE | NVME3_RED);

    // --- Disable blinking for NVMe slots ---
    write_i2c_register(NVME0_BLINK_REG, 0x00);
    write_i2c_register(NVME1_BLINK_REG, 0x00);
    write_i2c_register(NVME2_BLINK_REG, 0x00);
    write_i2c_register(NVME3_BLINK_REG, 0x00);
}

// Check the state of a disk
int get_disk_status(disk_stats_t *disk) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/stat", disk->device_name);

    FILE* f = fopen(path, "r");
    if (!f) return -1; // Disk name not found or permission denied

    unsigned long long r, w;
    // Field 1: read I/Os, Field 5: write I/Os
    // %*llu is used to skip the fields in between (fields 2, 3, and 4)
    if (fscanf(f, "%llu %*llu %*llu %*llu %llu", &r, &w) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);

    // Activity is detected if current counters are greater than previous counters
    int activity = (r > disk->prev_reads || w > disk->prev_writes);

    // Update trackers for the next call
    disk->prev_reads = r;
    disk->prev_writes = w;

    return activity;
}

int get_disk_health(const char* disk_name) {
    char cmd[128];
    char buffer[256];
    // -H checks the "health summary" (PASSED or FAILED)
    snprintf(cmd, sizeof(cmd), "smartctl -H /dev/%s", disk_name);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return -1;

    int passed = 0;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        if (strstr(buffer, "PASSED")) {
            passed = 1;
            break;
        }
    }
    pclose(pipe);
    return passed;
}

// Read network statistics from /proc/net/dev
int read_network_stats(network_stats_t *network) {
    FILE *fp;
    char line[256];
    char interface[32];
    unsigned long long rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo, rx_frame, rx_compressed, rx_multicast;
    unsigned long long tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo, tx_colls, tx_carrier, tx_compressed;

    fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        perror("Failed to open /proc/net/dev");
        return -1;
    }

    // Skip header lines
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    network->is_active = 0;

    while (fgets(line, sizeof(line), fp)) {
        int parsed = sscanf(line, "%31[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            interface, &rx_bytes, &rx_packets, &rx_errs, &rx_drop, &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
                            &tx_bytes, &tx_packets, &tx_errs, &tx_drop, &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed);

        if (parsed >= 17) {
            // Skip loopback interface
            if (strncmp(interface, "lo", 2) == 0) {
                continue;
            }

            // Check for network activity
            if (rx_bytes != network->prev_rx_bytes || tx_bytes != network->prev_tx_bytes) {
                network->is_active = 1;
                strncpy(network->interface_name, interface, sizeof(network->interface_name) - 1);
                network->interface_name[sizeof(network->interface_name) - 1] = '\0';
            }

            network->prev_rx_bytes = rx_bytes;
            network->prev_tx_bytes = tx_bytes;
        }
    }

    fclose(fp);
    return 0;
}

// Update network LED based on activity
void update_network_led(network_stats_t *network) {
    // Turn off both colors first
    set_led_state(LED_ON_REG_0, NETWORK_WHITE, 0);
    set_led_state(LED_ON_REG_0, NETWORK_RED, 0);

    if (network->is_active) {
        // Network activity - white LED
        set_led_state(LED_ON_REG_0, NETWORK_WHITE, 1);
        if (debug) printf("Network: active on %s\n", network->interface_name);
    } else {
        if (debug) printf("Network: idle\n");
    }
}

int main(int argc, char *argv[]) {
    disk_stats_t disks[6] = {
        {"sda", 0, 0},
        {"sdb", 0, 0},
        {"nvme0n1", 0, 0},
        {"nvme1n1", 0, 0},
        {"nvme2n1", 0, 0},
        {"nvme3n1", 0, 0}
    };

    network_stats_t network = {"", 0, 0, 0};

    debug = getenv("LEDS_DEBUG") && strcmp(getenv("LEDS_DEBUG"), "true") == 0;

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (debug) {
        printf("LED Disk & Network Activity Monitor\n");
        printf("Press Ctrl+C to exit\n\n");
    }

    // Initialize I2C
    if (init_i2c() < 0) {
        fprintf(stderr, "Failed to initialize I2C\n");
        return 1;
    }

    // Turn off all LEDs initially
    turn_off_all_leds();

    // Initialize disk stats (first read to establish baseline)
    read_network_stats(&network);

    if (debug) printf("Starting monitoring loop...\n\n");

    // Main monitoring loop
    while (running) {
        // Read current network stats
        if (read_network_stats(&network) < 0) {
            fprintf(stderr, "Failed to read network stats\n");
            continue;
        }

        // Update disk LEDs
        for (int i = 0; i < 6; i++) {
            int disk_status, led_reg, white_mask, red_mask, blink_reg;

            // Get the status of sda
            disk_status = get_disk_status(&disks[i]);

            // Map disk to appropriate LED
            if (strcmp(disks[i].device_name, "sda") == 0) {
                led_reg = LED_ON_REG_0;
                white_mask = HDD0_WHITE;
                red_mask = HDD0_RED;
                blink_reg = HDD0_BLINK_REG;
            } else if (strcmp(disks[i].device_name, "sdb") == 0) {
                led_reg = LED_ON_REG_0;
                white_mask = HDD1_WHITE;
                red_mask = HDD1_RED;
                blink_reg = HDD1_BLINK_REG;
            } else if (strcmp(disks[i].device_name, "nvme0n1") == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME0_WHITE;
                red_mask = NVME0_RED;
                blink_reg = NVME0_BLINK_REG;
            } else if (strcmp(disks[i].device_name, "nvme1n1") == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME1_WHITE;
                red_mask = NVME1_RED;
                blink_reg = NVME1_BLINK_REG;
            } else if (strcmp(disks[i].device_name, "nvme2n1") == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME2_WHITE;
                red_mask = NVME2_RED;
                blink_reg = NVME2_BLINK_REG;
            } else if (strcmp(disks[i].device_name, "nvme3n1") == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME3_WHITE;
                red_mask = NVME3_RED;
                blink_reg = NVME3_BLINK_REG;
            } else {
                continue; // Unknown disk
            }

            // Turn led and blink off if disk not connected
            if (disk_status == -1) {
                if (debug) printf("Disk: %s not connected\n", disks[i].device_name);
                set_led_state(led_reg, white_mask, 0);
                set_led_state(led_reg, red_mask, 0);
                set_blink_state(blink_reg, 0);
                continue;
            }

            // Make led red if disk health issue
            if (!get_disk_health(disks[i].device_name)) {
                if (debug) printf("Disk: %s SMART failed\n", disks[i].device_name);
                set_led_state(led_reg, white_mask, 0);
                set_led_state(led_reg, red_mask, 1);
                set_blink_state(blink_reg, 0);
                continue;
            }

            // Make led blink if there is disk activity
            if (disk_status == 1) {
                if (debug) printf("Disk: %s activity\n", disks[i].device_name);
                set_blink_state(blink_reg, 1);
            } else {
                if (debug) printf("Disk: %s connected\n", disks[i].device_name);
                set_blink_state(blink_reg, 0);
            }

            // Make led white because the disk is healty
            set_led_state(led_reg, white_mask, 1);
            set_led_state(led_reg, red_mask, 0);
        }

        // Update network LED
        update_network_led(&network);

        if (debug) printf("---\n");

        // Wait before next iteration
        usleep(ACTIVITY_SAMPLE_INTERVAL);
    }

    // Cleanup
    if (debug) printf("Starting cleanup...\n");

    // Turn off all LEDs with retry
    int retry_count = 0;
    while (retry_count < CLEANUP_MAX_RETRIES) {
        if (debug) printf("Attempt %d/%d: Turning off LEDs...\n", retry_count + 1, CLEANUP_MAX_RETRIES);
        // Turn off all LEDs (this function already handles blinking)
        turn_off_all_leds();
        // Small delay to ensure I2C commands complete
        usleep(CLEANUP_RETRY_DELAY);
        retry_count++;
    }
    if (debug) printf("Cleanup completed after %d attempts.\n", retry_count);

    cleanup_i2c();

    if (debug) printf("LED monitor stopped.\n");
    return 0;
}
