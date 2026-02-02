#include <arpa/inet.h>
#include <fcntl.h>
#include <i2c/smbus.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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

// Disk device names
#define HDD0_NAME "sda"
#define HDD1_NAME "sdb"
#define NVME0_NAME "nvme0n1"
#define NVME1_NAME "nvme1n1"
#define NVME2_NAME "nvme2n1"
#define NVME3_NAME "nvme3n1"

// Disk activity sample interval in microseconds
#define DISK_SAMPLE_INTERVAL 1000000 // 1 second

// Amount of disk activity samples before network check
#define NETWORK_SAMPLE_INTERVAL 60

// Cleanup delay in microseconds
#define CLEANUP_DELAY 500000 // 500ms

typedef struct {
    char device_name[32];
    unsigned long long prev_reads;
    unsigned long long prev_writes;
} disk_stats_t;

// Global variables
static int i2c_fd = -1;
static int i2c_bus = -1;
static volatile int running = 1;
static int debug = 0;

// Function prototypes
void signal_handler(int signal);
int find_i2c_bus(void);
int init_i2c(void);
int write_i2c_register(int reg, int value);
void set_led_state(int reg, int mask, int state);
void set_blink_state(int reg, int state);
void turn_off_all_leds(void);
int get_disk_status(disk_stats_t *disk);
int get_disk_health(const char* disk_name);
int is_connected_to_network();
int is_connected_to_internet();
void update_network_led();

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

    // Return if no bus has been found
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
        if (i2c_fd >= 0) close(i2c_fd);
        return -1;
    }

    if (debug) printf("I2C initialized successfully on bus %d\n", i2c_bus);
    return 0;
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

// Turn LED on or off
void set_led_state(int reg, int mask, int state) {
    if (state) {
        write_i2c_register(reg, mask);
    } else {
        write_i2c_register(reg + 0x10, mask); // OFF register is +0x10 from ON register
    }
}

// Enable or disable LED blinking
void set_blink_state(int reg, int state) {
    if (state) {
        write_i2c_register(reg, 0x01);
    } else {
        write_i2c_register(reg, 0x00);
    }
}

void turn_off_all_leds(void) {
    if (debug) printf("Turning off all LEDs and disabling blinking...\n");

    // --- Turn off HDD and Network LEDs ---
    write_i2c_register(LED_OFF_REG_0, HDD0_WHITE | HDD0_RED);
    write_i2c_register(LED_OFF_REG_0, HDD1_WHITE | HDD1_RED);
    write_i2c_register(LED_OFF_REG_0, NETWORK_WHITE | NETWORK_RED);

    // --- Turn off NVMe LEDs ---
    write_i2c_register(LED_OFF_REG_1, NVME0_WHITE | NVME0_RED);
    write_i2c_register(LED_OFF_REG_1, NVME1_WHITE | NVME1_RED);
    write_i2c_register(LED_OFF_REG_1, NVME2_WHITE | NVME2_RED);
    write_i2c_register(LED_OFF_REG_1, NVME3_WHITE | NVME3_RED);

    // --- Disable blinking on all LEDs ---
    write_i2c_register(HDD0_BLINK_REG, 0x00);
    write_i2c_register(HDD1_BLINK_REG, 0x00);
    write_i2c_register(NETWORK_BLINK_REG, 0x00);
    write_i2c_register(NVME0_BLINK_REG, 0x00);
    write_i2c_register(NVME1_BLINK_REG, 0x00);
    write_i2c_register(NVME2_BLINK_REG, 0x00);
    write_i2c_register(NVME3_BLINK_REG, 0x00);
}

// Check if a disk is connected and if there is activity on that disk
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

// Check the SMART health summary
int get_disk_health(const char* disk_name) {
    char cmd[128];
    char buffer[256];
    // -H checks the "health summary" (PASSED or FAILED)
    snprintf(cmd, sizeof(cmd), "smartctl -H /dev/%s", disk_name);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        printf("Failed to check S.M.A.R.T. health test result\n");
        return -1;
    }

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

// Check if the host is connected to a local network
int is_connected_to_network() {
    struct ifaddrs *ifaddr, *ifa;
    int connected = 0;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        // Check if interface is UP and not a loopback (lo)
        if ((ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            connected = 1;
            break;
        }
    }
    freeifaddrs(ifaddr);
    return connected;
}

// Check if the host has internet access
int is_connected_to_internet() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(53); // DNS port
    inet_pton(AF_INET, "8.8.8.8", &serv_addr.sin_addr);

    // Try connecting with a timeout
    int result = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    close(sock);

    return (result == 0); // 1 if success, 0 if fail
}

void update_network_led() {
    // Turn LED off if not connected to a network
    if (!is_connected_to_network()) {
        if (debug) printf("Network: not connected\n");
        set_led_state(LED_ON_REG_0, NETWORK_WHITE, 0);
        set_led_state(LED_ON_REG_0, NETWORK_RED, 0);
        return;
    }

    // Set LED to white if connected to internet
    if (is_connected_to_internet()) {
        if (debug) printf("Network: connected\n");
        set_led_state(LED_ON_REG_0, NETWORK_WHITE, 1);
        set_led_state(LED_ON_REG_0, NETWORK_RED, 0);
        return;
    }

    // Set LED to red if not connected to internet
    if (debug) printf("Network: no internet\n");
    set_led_state(LED_ON_REG_0, NETWORK_WHITE, 0);
    set_led_state(LED_ON_REG_0, NETWORK_RED, 1);
}

int main(int argc, char *argv[]) {
    // Initializing variables
    disk_stats_t disks[6] = {
        {HDD0_NAME, 0, 0},
        {HDD1_NAME, 0, 0},
        {NVME0_NAME, 0, 0},
        {NVME1_NAME, 0, 0},
        {NVME2_NAME, 0, 0},
        {NVME3_NAME, 0, 0}
    };
    int disk_check_count = NETWORK_SAMPLE_INTERVAL;

    // Check for debug environment variable
    debug = getenv("LEDS_DEBUG") && strcmp(getenv("LEDS_DEBUG"), "true") == 0;
    if (debug) {
        printf("LED Disk & Network Activity Monitor\n");
        printf("Press Ctrl+C to exit\n\n");
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize I2C
    if (init_i2c() < 0) {
        fprintf(stderr, "Failed to initialize I2C\n");
        return 1;
    }

    // Turn off all LEDs initially
    turn_off_all_leds();

    if (debug) printf("Starting monitoring loop...\n\n");

    // Main monitoring loop
    while (running) {
        // Update disk LEDs
        for (int i = 0; i < 6; i++) {
            int disk_status, led_reg, white_mask, red_mask, blink_reg;

            // Map disk to appropriate LED
            if (strcmp(disks[i].device_name, HDD0_NAME) == 0) {
                led_reg = LED_ON_REG_0;
                white_mask = HDD0_WHITE;
                red_mask = HDD0_RED;
                blink_reg = HDD0_BLINK_REG;
            } else if (strcmp(disks[i].device_name, HDD1_NAME) == 0) {
                led_reg = LED_ON_REG_0;
                white_mask = HDD1_WHITE;
                red_mask = HDD1_RED;
                blink_reg = HDD1_BLINK_REG;
            } else if (strcmp(disks[i].device_name, NVME0_NAME) == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME0_WHITE;
                red_mask = NVME0_RED;
                blink_reg = NVME0_BLINK_REG;
            } else if (strcmp(disks[i].device_name, NVME1_NAME) == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME1_WHITE;
                red_mask = NVME1_RED;
                blink_reg = NVME1_BLINK_REG;
            } else if (strcmp(disks[i].device_name, NVME2_NAME) == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME2_WHITE;
                red_mask = NVME2_RED;
                blink_reg = NVME2_BLINK_REG;
            } else if (strcmp(disks[i].device_name, NVME3_NAME) == 0) {
                led_reg = LED_ON_REG_1;
                white_mask = NVME3_WHITE;
                red_mask = NVME3_RED;
                blink_reg = NVME3_BLINK_REG;
            } else {
                continue; // Unknown disk
            }

            // Get the status of the disk
            disk_status = get_disk_status(&disks[i]);

            // Turn led and blink off if disk not connected
            if (disk_status == -1) {
                if (debug) printf("Disk: %s not connected\n", disks[i].device_name);
                set_led_state(led_reg, white_mask, 0);
                set_led_state(led_reg, red_mask, 0);
                set_blink_state(blink_reg, 0);
                continue;
            }

            // Make LED red if disk SMART check fails
            if (get_disk_health(disks[i].device_name) == 0) {
                if (debug) printf("Disk: %s S.M.A.R.T. health test result: FAILED\n", disks[i].device_name);
                set_led_state(led_reg, white_mask, 0);
                set_led_state(led_reg, red_mask, 1);
                set_blink_state(blink_reg, 0);
                continue;
            }

            // Make LED blink if there is disk activity
            if (disk_status == 1) {
                if (debug) printf("Disk: %s activity\n", disks[i].device_name);
                set_blink_state(blink_reg, 1);
            } else {
                if (debug) printf("Disk: %s connected\n", disks[i].device_name);
                set_blink_state(blink_reg, 0);
            }

            // Make LED white because the disk is healty
            set_led_state(led_reg, white_mask, 1);
            set_led_state(led_reg, red_mask, 0);
        }

        // Update network LED
        if (disk_check_count >= NETWORK_SAMPLE_INTERVAL) {
            disk_check_count = 0;
            update_network_led();
        }
        disk_check_count ++;

        if (debug) printf("---\n");

        // Wait before next iteration
        usleep(DISK_SAMPLE_INTERVAL);
    }

    // Cleanup
    if (debug) printf("Starting cleanup...\n");

    // Wait a bit before turning off all LEDs
    usleep(CLEANUP_DELAY);

    if (debug) printf("Turning off LEDs...\n");
    turn_off_all_leds();

    if (debug) printf("Closing I2C bus...\n");
    if (i2c_fd >= 0) close(i2c_fd);

    if (debug) printf("Goodbye!\n");
    return 0;
}
