# lincstation-leds
A daemon to use the LincStation N2 LEDs on Linux other than Unraid.

> [!NOTE]
> This is a fork of [elias-gvin/lincstation_leds](https://github.com/elias-gvin/lincstation_leds), which in and of itself is a fork of [fazalmajid/lincstation_leds](https://github.com/fazalmajid/lincstation_leds). If you would like to know more about the origin of this project or don't like my approach to the LEDs, please check out their repos as well.

I mostly used the original code as a base, but most of it has changed. Do note that I'm not very familiar in C, so if there are any issues (or memory leaks, I know that C is prone to that) please let me know in the [issues](https://github.com/duisterethomas/lincstation-leds/issues).

> [!IMPORTANT]
> I'm developing this fork for and testing this fork on [Proxmox VE](https://www.proxmox.com/en/products/proxmox-virtual-environment/overview), so keep in mind that the installation instructions might not work on other linux distros. 

## LED color meanings
- Disk LEDs
    - White = A disk is present in the corresponding slot
    - Blinking white = There is read and/or write activity on the disk
    - Red = The result of the S.M.A.R.T. health check is `FAILED`
    - Off = No disk is detected in the corresponding slot
- Network LED
    - White = The LincStation is connected to the internet
    - Red = The LincStation is connected to the local network, but doesn't have internet access
    - Off = The LincStation is not connected to a network

## Building and installation
You need to run all of these commands on your LincStation. In Proxmox you can do this either by logging into the web ui, selecting the node and clicking on shell, or by connecting to it via ssh.

> [!IMPORTANT]
> You will have to run most of these commands as the `root` user if you're installing this on Proxmox!

### Prerequisites
1. Install the required packages
   
   You will need to have `git`, `make`, `gcc`, `i2c-tools`, `i2c-tools-dev` and `smartmontools` installed.
   ```bash
   apt-get install git make gcc i2c-tools libi2c-dev smartmontools
   ```

2. Ensure the I2C modules are loaded
   ```bash
   modprobe i2c-dev
   modprobe i2c-i801  # or appropriate I2C driver for your hardware
   modprobe i2c-core
   ```

3. Clone this repository and enter the folder
   ```bash
   git clone https://github.com/duisterethomas/lincstation-leds.git
   cd lincstation-leds
   ```

### Testing
You can test it before installing by running:

```bash
make
env LEDS_DEBUG=true ./lincstation_leds
```

### Installing the Service
1. Build and install
   ```bash
   make
   make install
   ```

   This will:
   - Compile the binary
   - Install it to `/usr/local/bin/lincstation_leds`
   - Install the systemd service file to `/etc/systemd/system/lincstation-leds.service`
   - Enable the service to start at boot

2. Start the service
   ```bash
   systemctl start lincstation-leds.service
   ```

3. Verify it's running
   ```bash
   systemctl status lincstation-leds.service
   ```

### Uninstalling
To remove the service and binary:
```bash
make uninstall
```

This will:
- Stop and disable the service
- Remove the binary and service file
- Reload systemd configuration

## Configuring
There isn't really a config (yet), so to configure this you'll have to change values in the code. All of the values mentioned here are near the top of the `lincstation_leds.c` file. Don't forget to build and install again after you're done configuring using:
```bash
make
make install
```

### Update intervals
By default, the disk LEDs will update once per second and the network LED once per 60 disk LEDs updates (e.g. 60 seconds). you can change this by modifying these values in the code:
```c
// Disk activity sample interval in microseconds
#define DISK_SAMPLE_INTERVAL 1000000 // 1 second

// Amount of disk activity samples before network check
#define NETWORK_SAMPLE_INTERVAL 60
```
Just be aware that it will consume more CPU with shorter values. 

### Disk names
If the disk names are different on your system, you can modify the following values in the code:
```c
// Disk device names
#define HDD0_NAME "sda"
#define HDD1_NAME "sdb"
#define NVME0_NAME "nvme0n1"
#define NVME1_NAME "nvme1n1"
#define NVME2_NAME "nvme2n1"
#define NVME3_NAME "nvme3n1"
```
