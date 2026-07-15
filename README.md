# 🛠️ nas-monitor

[![Buildroot-native](https://img.shields.io/badge/Buildroot-native-blue.svg)](https://buildroot.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

`nas-monitor` is a lightweight, Buildroot-native monitoring and control daemon designed specifically for DIY NAS-style systems. It gathers essential system metrics, manages fan PWM curves, monitors drive standby/spindown states, controls status LEDs via sysfs, integrates with Home Assistant via MQTT, and responds dynamically to physical power-switch events.

This is my attempt to create all-in-one package for handling functionalities for my Buffalo CS-WV setup. This project started only for lowering the power consumption and noise by switching off disks when they are not needed and keeping the fan noise at minimum. But idea led to another and now there is a bunch of useful features implemented.

There is nothing device specific, only dependency is paho-mqtt-c package. Optionally, if smartd and smartctl are present the status of the checks and the latest result are sent in mqtt as well. 

<div align="center">
  <img src="https://github.com/Jarauvi/nas-monitor/blob/main/images/controls.png?raw=true" width="128" height="128">
  <img src="https://github.com/Jarauvi/nas-monitor/blob/main/images/sensors.png?raw=true" width="128" height="128">
</div>

## ✨ Features

* **📊 Metrics & Integration:** Publish runtime metrics to MQTT with automatic Home Assistant MQTT Discovery.
* **📈 System Health:** Real-time monitoring of CPU, memory, disk utilization, and filesystem status.
* **💾 Storage Care:** Smart drive power/standby detection and automated, configurable disk spindown.
* **💨 Active Cooling:** Intelligent PWM fan control dynamically mapped to CPU/SoC temperatures.
* **🚨 Hardware Status Indicators:** Direct LED control using standard Linux `sysfs` brightness nodes.
* **🔌 Power Automation:** Physical power-switch monitoring via Linux input-event devices (`/dev/input/event*`) with safe software shutdown sequences.

---

## 📦 Buildroot Integration

Follow these steps to integrate `nas-monitor` into your custom Buildroot tree:

### 1. Clone to packages

Clone or move the repo to buildroot-xxxx.xx.x/package/

### 2. Register the Package

Add the package to your Buildroot package index by editing your local package config file (buildroot-xxxx.xx.x/package/Config.in).

Under System tools title add:

```text
source "package/nas-monitor/Config.in"
```

### 3. Configure and Compile

Run your configuration utility, enable the package, and initiate the build:

```bash
# Open the configuration menu
make menuconfig

# Navigate to and select:
# Target packages -> System tools -> [*] nas-monitor

# Compile the whole image to include the package with dependencies
make

# ...Or compile only the daemon package for moving to the existing system (still needs existing paho-mqtt-c binaries)
make nas-monitor


```

> ℹ️ **Note:** The compilation process installs the target binary at `/usr/bin/nas-monitor` and deploys a default template configuration file to `/etc/nas-monitor.conf`.

---

## ⚙️ Configuration

The daemon manages its behavior entirely through the `/etc/nas-monitor.conf` configuration file.

### Runtime Debugging

* **Interactive Mode:** When run directly in a shell, the daemon automatically prints verbose tracing output directly to `stdout`.
* **Daemon Mode:** To force verbose debugging output when running in the background, set:

```ini
debug_enabled=1
```

*Even in debug mode, critical failures and core errors are continuously mirrored to the system logging utility if that is present (`syslog`).*

### Configuration Blueprint (`/etc/nas-monitor.conf`)

Look at section **Hardware Discovery Helper** below for finding out device parameters

```ini
# ==============================================================================
# MQTT & Home Assistant Discovery
# ==============================================================================
mqtt_enabled=1
ha_discovery_enabled=1
mqtt_address=tcp://192.168.1.234:5678
client_id=Linkstation
username=mqtt_user
password=secret
storage_mount=/mnt/storage

# ----------------------------------------------------------------------------
# Home Assistant MQTT buttons visibility (optional)
# - Individual buttons can be enabled/disabled.
# - Spindown buttons are generated for every configured drive.
#   Use mqtt_btn_spindown_enabled to show/hide all of them at once.
# ----------------------------------------------------------------------------
# Per-button enable/disable for non-spindown buttons
# rw and ro buttons for toggling file system between read-write/read-only
# make sure you have defined all the necessary paths to be written to tmpfs before activating
mqtt_btn_mode_rw_enabled=1
mqtt_btn_mode_ro_enabled=1
mqtt_btn_reboot_enabled=1
mqtt_btn_shutdown_enabled=1
mqtt_btn_sleep_enabled=1

# Single enable/disable to show ALL spindown buttons
mqtt_btn_spindown_enabled=1

# ==============================================================================
# Drive Spindown Management
# When drive is idle for certain time, spindown the disk
# Good for replacing hdparm -S in the devices that do not support it
# ==============================================================================
spindown_enabled=1
spindown_drive_count=2
spindown_drive_1=/dev/sda
spindown_timeout_1=600
spindown_drive_2=/dev/sdb
spindown_timeout_2=600

# ==============================================================================
# PWM Fan Curve Config
# Control fan PWM based on temperature
# ==============================================================================
fan_control_enabled=1
temp_sensor_path=/sys/class/hwmon/hwmon1/temp1_input
pwm_enable_path=/sys/class/hwmon/hwmon0/pwm1_enable
pwm_file_path=/sys/class/hwmon/hwmon0/pwm1
fan_start_temp=45
fan_max_temp=65
fan_crit_temp=80
fan_min_pwm=0
fan_max_pwm=255

# ==============================================================================
# Physical Power Switch Monitor
# Detects power switch state and shuts down device when switched off
# ==============================================================================
power_switch_monitor_enabled=1
power_switch_input_device=/dev/input/event0
power_sw_code=0
auto_sw_code=1
off_shutdown_delay_sec=2

# ===============================================================================
# LED control
# Supported purposes: standby, temp_limit, disk_almost_full
# standby = if configured disk has been stopped to standby mode
# temp_limit = if certain temperature threshold is reached
# disk_almost_full = if certain block capacity is over x % 
# ===============================================================================

led_control_enabled=0
leds_count=1

# LED #1 example: standby indication
# The standby logic uses on_value when the drive is in standby and off_value when active.
led_name_1=linkstation:blue:function
led_purpose_1=standby
led_disk_device_1=/dev/sda
led_on_value_1=0
led_off_value_1=1
led_poll_interval_sec_1=5

# LED #2 example: temperature threshold
led_name_2=linkstation:amber:info
led_purpose_2=temp_limit
led_temp_threshold_c_2=80
led_on_value_2=1
led_off_value_2=0

# LED #3 example: storage usage threshold
led_name_3=linkstation:red:alarm
led_purpose_3=disk_almost_full
led_disk_almost_full_pct_3=90
led_on_value_3=1
led_off_value_3=0
```

---

## 🔍 Hardware Discovery Helper

Because hardware paths may vary, use these target shell snippets to easily locate the paths for your `/etc/nas-monitor.conf` setup:

### 1. Locate Temperature Sensors

Find hardware monitoring `hwmon` paths or fallback to standard thermal zone paths:

```bash
# Method A: Search standard hwmon nodes
find /sys/class/hwmon -type f \( -name 'temp*_input' -o -name 'temp*_raw' \) 2>/dev/null

# Method B: Fallback query on SoC thermal zones
for zone in /sys/class/thermal/thermal_zone*; do
  echo "--- $zone ($(cat $zone/type 2>/dev/null))"
  cat "$zone/temp" 2>/dev/null
  echo
done
```

### 2. Locate PWM Fan Outputs

Search broadly for PWM or system cooling controllers:

```bash
find /sys -type f \( -name 'pwm*' -o -name '*pwm*' -o -name '*fan*' -o -name '*cool*' \) 2>/dev/null | head -200
```

### 3. Identify Available System LEDs

Check the registered onboard LED system control directories:

```bash
ls -la /sys/class/leds
```

### 4. Isolate the Power Button Event Interface

Locate and test your chassis' push/toggle buttons:

```bash
# List all registered input event devices
ls -la /dev/input/event*

# Test a specific input device (press your button to see raw console output)
cat /dev/input/event0
```

### 5. Disk Devices for Spindown

List your block devices:

```bash
ls /dev/sd?
```

*Use these exact paths for your `spindown_drive_X` values.*

---

## 📡 MQTT & Home Assistant Integration

When `mqtt_enabled=1` is configured, the daemon outputs runtime metrics to the following MQTT topic:

```text
nas/monitor/state
```

If `ha_discovery_enabled=1` is active, discovery JSON payloads are automatically pushed to the default home assistant autodiscovery prefix:

```text
homeassistant/sensor/nas_monitor/...
```

---

## ⚠️ Requirements & Limitations

| Aspect | Target Condition | Fallback / Behavior |
| :--- | :--- | :--- |
| **Drive Diagnostics** | `smartctl` (from `smartmontools`) | If missing, the daemon reports `"SMART unavailable"` |
| **Hardware Control** | Linux Kernel `sysfs` drivers active | Features safely auto-disable if driver nodes are missing |
| **Portability** | Multi-platform design | Hardware-dependent features require target hardware to expose standard sysfs interfaces |
| **I/O Capabilities** | Custom Hardware / GPIO platforms | Relies on default Linux input subsystem event mapping |
