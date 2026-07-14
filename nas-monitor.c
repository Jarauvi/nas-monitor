#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/hdreg.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#ifndef EVIOCGSW
#define EVIOCGSW(len) _IOR('E', 0x45, len)
#endif
#include <sys/select.h>
#include "MQTTClient.h"
#include <time.h>


#define CONFIG_PATH "/etc/nas-monitor.conf"
#define STATE_TOPIC "nas/monitor/state"
#define DISCOVERY_PREFIX "homeassistant/sensor/nas_monitor"
#define BUTTON_PREFIX "homeassistant/button/nas_monitor"
#define CMD_TOPIC_BASE "nas/monitor/command/"

static int debug_enabled = 0;

static void app_debug(const char *fmt, ...) {
    if (!debug_enabled) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static int run_command(const char *cmd) {
    app_debug("command: %s", cmd);
    int rc = system(cmd);
    app_debug("command return code: %d", rc);
    return rc;
}

static int get_ev_sw_state(const char *event_device, int sw_code) {
    int fd = open(event_device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    // EVIOCGSW uses: ioctl(fd, EVIOCGSW(sizeof(state)), &state)
    // where state is a struct input_sw_state { __u16 sw; __s16 value; }.
    struct input_sw_state {
        __u16 sw;
        __s16 value;
    } state;

    memset(&state, 0, sizeof(state));
    state.sw = (unsigned short)sw_code;

    if (ioctl(fd, EVIOCGSW(sizeof(state)), &state) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return (int)state.value;
}

static void do_shutdown_now(void) {
    (void)sync();
    sleep(2);
    (void)system("/sbin/shutdown -h now");
}

#define MAX_LEDS 8
#define MAX_SPINDOWN_DRIVES 8

typedef enum {
    LED_PURPOSE_DISABLED = 0,
    LED_PURPOSE_STANDBY = 1,
    LED_PURPOSE_TEMP_LIMIT = 2,
    LED_PURPOSE_DISK_ALMOST_FULL = 3
} LedPurpose;

static const char *led_purpose_name(LedPurpose purpose) {
    switch (purpose) {
        case LED_PURPOSE_STANDBY:
            return "standby";
        case LED_PURPOSE_TEMP_LIMIT:
            return "temp_limit";
        case LED_PURPOSE_DISK_ALMOST_FULL:
            return "disk_almost_full";
        default:
            return "disabled";
    }
}

typedef struct {
    int enabled;
    char led_name[128];
    LedPurpose purpose;

    char disk_device[64];

    // Purpose: TEMP_LIMIT
    int temp_threshold_c;

    // Purpose: DISK_ALMOST_FULL
    int disk_almost_full_pct;

    int on_value;
    int off_value;

    int poll_interval_sec;
} LedConfig;

typedef struct {
    char mqtt_address[256];
    char client_id[128];
    char username[128];
    char password[128];
    char storage_mount[256];
    char temp_sensor_path[256];
    char pwm_enable_path[256];
    char pwm_file_path[256];
    char led_sysfs_base_path[256];
    int sda_spindown_timeout;
    int sdb_spindown_timeout;
    int spindown_drive_count;
    char spindown_drive[MAX_SPINDOWN_DRIVES][64];
    int spindown_timeout[MAX_SPINDOWN_DRIVES];

    // Optional modules
    int mqtt_enabled;
    int ha_discovery_enabled;
    int fan_control_enabled;

    // Spindown behavior enable (actual hdparm standby cycle)
    int spindown_enabled;

    // MQTT / Home Assistant button visibility enable (per requirements)
    int mqtt_btn_mode_rw_enabled;
    int mqtt_btn_mode_ro_enabled;
    int mqtt_btn_reboot_enabled;
    int mqtt_btn_shutdown_enabled;
    int mqtt_btn_sleep_enabled;
    // Single enable/disable to show ALL spindown buttons
    int mqtt_btn_spindown_enabled;

    // LED module
    int led_control_enabled;
    int leds_count;
    LedConfig leds[MAX_LEDS];
    int debug_enabled;

    // Power switch monitor module
    int power_switch_monitor_enabled;
    char power_switch_input_device[128];
    int power_sw_code;
    int auto_sw_code;
    int off_shutdown_delay_sec;

    // Fan module configuration
    int fan_start_temp;

    int fan_max_temp;
    int fan_crit_temp;
    int fan_min_pwm;
    int fan_max_pwm;
} NASConfig;

typedef struct {
    float cpu_usage;
    float mem_usage;
    float disk_usage;
    float disk_free_gb;
    float cpu_temp;
    float load_1m;
    long uptime;
    int sda_active;
    int sdb_active;
    char sda_smart[64];
    char sdb_smart[64];
    char fs_mode[8];
    int fan_pwm; // New metric
} NASMetrics;

typedef struct {
    char device_path[64];
    unsigned long last_io_sectors;
    time_t last_active_time;
    int initialized;
    int is_spun_down; // Prevent sending redundant standby commands
} SoftwareSpindownTracker;

// Array to hold the state of tracked drives
static SoftwareSpindownTracker spindown_trackers[MAX_SPINDOWN_DRIVES];

// Ring buffer to keep track of running temperatures
#define TEMP_SAMPLES 5
static int temp_history[TEMP_SAMPLES] = {0};
static int temp_index = 0;
static int temp_count = 0;
static int last_led_brightness[MAX_LEDS] = { -1 };

void trim(char *str) {
    int len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' || str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }
}

int load_config(const char *filename, NASConfig *config) {
    /* Set safe defaults. */
    strncpy(config->temp_sensor_path, "/sys/class/hwmon/hwmon1/temp1_input", sizeof(config->temp_sensor_path));
    strncpy(config->pwm_enable_path, "/sys/class/hwmon/hwmon0/pwm1_enable", sizeof(config->pwm_enable_path));
    strncpy(config->pwm_file_path, "/sys/class/hwmon/hwmon0/pwm1", sizeof(config->pwm_file_path));
    strncpy(config->led_sysfs_base_path, "/sys/class/leds", sizeof(config->led_sysfs_base_path));
    config->sda_spindown_timeout = 0;
    config->sdb_spindown_timeout = 0;
    config->spindown_drive_count = 0;
    for (int i = 0; i < MAX_SPINDOWN_DRIVES; i++) {
        config->spindown_drive[i][0] = '\0';
        config->spindown_timeout[i] = 0;
    }

    // Modules (defaults keep current behavior as close as possible)
    config->mqtt_enabled = 1;
    config->ha_discovery_enabled = 1;
    config->fan_control_enabled = 0;
    config->spindown_enabled = 1;

    // MQTT button visibility defaults (keep previous behavior)
    config->mqtt_btn_mode_rw_enabled = 1;
    config->mqtt_btn_mode_ro_enabled = 1;
    config->mqtt_btn_reboot_enabled = 1;
    config->mqtt_btn_shutdown_enabled = 1;
    config->mqtt_btn_sleep_enabled = 1;

    // Single enable/disable to show ALL spindown buttons
    config->mqtt_btn_spindown_enabled = 1;


    // LED module defaults
    config->led_control_enabled = 0;
    config->leds_count = MAX_LEDS;
    config->debug_enabled = 0;
    for (int i = 0; i < MAX_LEDS; i++) {

        config->leds[i].enabled = 0;
        config->leds[i].purpose = LED_PURPOSE_DISABLED;
        config->leds[i].led_name[0] = '\0';
        config->leds[i].disk_device[0] = '\0';
        config->leds[i].temp_threshold_c = 80;
        config->leds[i].disk_almost_full_pct = 90;
        config->leds[i].on_value = 1;
        config->leds[i].off_value = 0;
        config->leds[i].poll_interval_sec = 5;
    }

    config->fan_start_temp = 45;
    config->fan_max_temp = 65;
    config->fan_crit_temp = 80;
    config->fan_min_pwm = 0;
    config->fan_max_pwm = 255;

    // Power switch monitor module defaults (disabled)
    config->power_switch_monitor_enabled = 0;
    strncpy(config->power_switch_input_device, "/dev/input/event0", sizeof(config->power_switch_input_device));
    config->power_sw_code = 0;
    config->auto_sw_code = 1;
    config->off_shutdown_delay_sec = 2;


    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");

        if (key && value) {
            trim(key);
            trim(value);

            if (strcmp(key, "mqtt_address") == 0) strncpy(config->mqtt_address, value, sizeof(config->mqtt_address));
            else if (strcmp(key, "client_id") == 0) strncpy(config->client_id, value, sizeof(config->client_id));
            else if (strcmp(key, "username") == 0) strncpy(config->username, value, sizeof(config->username));
            else if (strcmp(key, "password") == 0) strncpy(config->password, value, sizeof(config->password));
            else if (strcmp(key, "storage_mount") == 0) strncpy(config->storage_mount, value, sizeof(config->storage_mount));
            else if (strcmp(key, "temp_sensor_path") == 0) strncpy(config->temp_sensor_path, value, sizeof(config->temp_sensor_path));
            else if (strcmp(key, "pwm_enable_path") == 0) strncpy(config->pwm_enable_path, value, sizeof(config->pwm_enable_path));
            else if (strcmp(key, "pwm_file_path") == 0) strncpy(config->pwm_file_path, value, sizeof(config->pwm_file_path));
            else if (strcmp(key, "leds_sysfs_base_path") == 0) strncpy(config->led_sysfs_base_path, value, sizeof(config->led_sysfs_base_path));
            else if (strcmp(key, "sda_spindown_timeout") == 0) config->sda_spindown_timeout = atoi(value);
            else if (strcmp(key, "sdb_spindown_timeout") == 0) config->sdb_spindown_timeout = atoi(value);
            else if (strcmp(key, "spindown_drive_count") == 0) config->spindown_drive_count = atoi(value);
            else if (strncmp(key, "spindown_drive_", 15) == 0) {
                int idx = atoi(key + 15);
                if (idx >= 1 && idx <= MAX_SPINDOWN_DRIVES) {
                    strncpy(config->spindown_drive[idx - 1], value, sizeof(config->spindown_drive[0]));
                }
            } else if (strncmp(key, "spindown_timeout_", 17) == 0) {
                int idx = atoi(key + 17);
                if (idx >= 1 && idx <= MAX_SPINDOWN_DRIVES) {
                    config->spindown_timeout[idx - 1] = atoi(value);
                }
            } else if (strcmp(key, "mqtt_enabled") == 0) config->mqtt_enabled = atoi(value);
            else if (strcmp(key, "ha_discovery_enabled") == 0) config->ha_discovery_enabled = atoi(value);
            else if (strcmp(key, "fan_control_enabled") == 0) config->fan_control_enabled = atoi(value);
            else if (strcmp(key, "spindown_enabled") == 0) config->spindown_enabled = atoi(value);

            // MQTT button visibility enable flags
            else if (strcmp(key, "mqtt_btn_mode_rw_enabled") == 0) config->mqtt_btn_mode_rw_enabled = atoi(value);
            else if (strcmp(key, "mqtt_btn_mode_ro_enabled") == 0) config->mqtt_btn_mode_ro_enabled = atoi(value);
            else if (strcmp(key, "mqtt_btn_reboot_enabled") == 0) config->mqtt_btn_reboot_enabled = atoi(value);
            else if (strcmp(key, "mqtt_btn_shutdown_enabled") == 0) config->mqtt_btn_shutdown_enabled = atoi(value);
            else if (strcmp(key, "mqtt_btn_sleep_enabled") == 0) config->mqtt_btn_sleep_enabled = atoi(value);
            else if (strcmp(key, "mqtt_btn_spindown_enabled") == 0) config->mqtt_btn_spindown_enabled = atoi(value);

            else if (strcmp(key, "led_control_enabled") == 0) config->led_control_enabled = atoi(value);

            else if (strcmp(key, "leds_count") == 0) config->leds_count = atoi(value);

            // Per-LED config (led_name_1..led_name_N, purpose_1..)
            else if (strncmp(key, "led_name_", 9) == 0) {
                int idx = atoi(key + 9);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    LedConfig *l = &config->leds[idx - 1];
                    strncpy(l->led_name, value, sizeof(l->led_name));
                    l->enabled = (l->led_name[0] != '\0');
                }
            } else if (strncmp(key, "led_purpose_", 13) == 0) {
                int idx = atoi(key + 13);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    LedConfig *l = &config->leds[idx - 1];
                    if (strcmp(value, "standby") == 0) l->purpose = LED_PURPOSE_STANDBY;
                    else if (strcmp(value, "temp_limit") == 0) l->purpose = LED_PURPOSE_TEMP_LIMIT;
                    else if (strcmp(value, "disk_almost_full") == 0) l->purpose = LED_PURPOSE_DISK_ALMOST_FULL;
                    else l->purpose = LED_PURPOSE_DISABLED;
                }
            } else if (strncmp(key, "led_disk_device_", 17) == 0) {
                int idx = atoi(key + 17);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    LedConfig *l = &config->leds[idx - 1];
                    strncpy(l->disk_device, value, sizeof(l->disk_device));
                }
            } else if (strncmp(key, "led_temp_threshold_c_", 23) == 0) {
                int idx = atoi(key + 23);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    config->leds[idx - 1].temp_threshold_c = atoi(value);
                }
            } else if (strncmp(key, "led_disk_almost_full_pct_", 27) == 0) {
                int idx = atoi(key + 27);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    config->leds[idx - 1].disk_almost_full_pct = atoi(value);
                }
            } else if (strncmp(key, "led_on_value_", 14) == 0) {
                int idx = atoi(key + 14);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    config->leds[idx - 1].on_value = atoi(value);
                }
            } else if (strncmp(key, "led_off_value_", 15) == 0) {
                int idx = atoi(key + 15);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    config->leds[idx - 1].off_value = atoi(value);
                }
            } else if (strncmp(key, "led_poll_interval_sec_", 24) == 0) {
                int idx = atoi(key + 24);
                if (idx >= 1 && idx <= MAX_LEDS) {
                    config->leds[idx - 1].poll_interval_sec = atoi(value);
                }
            }

            else if (strcmp(key, "debug_enabled") == 0) config->debug_enabled = atoi(value);
            else if (strcmp(key, "fan_start_temp") == 0) config->fan_start_temp = atoi(value);
            else if (strcmp(key, "fan_max_temp") == 0) config->fan_max_temp = atoi(value);
            else if (strcmp(key, "fan_crit_temp") == 0) config->fan_crit_temp = atoi(value);
            else if (strcmp(key, "fan_min_pwm") == 0) config->fan_min_pwm = atoi(value);
            else if (strcmp(key, "fan_max_pwm") == 0) config->fan_max_pwm = atoi(value);

            // Power switch monitor module
            else if (strcmp(key, "power_switch_monitor_enabled") == 0) config->power_switch_monitor_enabled = atoi(value);
            else if (strcmp(key, "power_switch_input_device") == 0) strncpy(config->power_switch_input_device, value, sizeof(config->power_switch_input_device));
            else if (strcmp(key, "power_sw_code") == 0) config->power_sw_code = atoi(value);
            else if (strcmp(key, "auto_sw_code") == 0) config->auto_sw_code = atoi(value);
            else if (strcmp(key, "off_shutdown_delay_sec") == 0) config->off_shutdown_delay_sec = atoi(value);

        }

    }

    if (config->spindown_drive_count <= 0) {
        int idx = 0;
        if (config->sda_spindown_timeout > 0 && idx < MAX_SPINDOWN_DRIVES) {
            strncpy(config->spindown_drive[idx], "/dev/sda", sizeof(config->spindown_drive[0]));
            config->spindown_timeout[idx] = config->sda_spindown_timeout;
            idx++;
        }
        if (config->sdb_spindown_timeout > 0 && idx < MAX_SPINDOWN_DRIVES) {
            strncpy(config->spindown_drive[idx], "/dev/sdb", sizeof(config->spindown_drive[0]));
            config->spindown_timeout[idx] = config->sdb_spindown_timeout;
            idx++;
        }
        config->spindown_drive_count = idx;
    } else if (config->spindown_drive_count > MAX_SPINDOWN_DRIVES) {
        config->spindown_drive_count = MAX_SPINDOWN_DRIVES;
    }

    fclose(fp);
    return 1;
}

/* Read the configured temperature sensor value from sysfs. */
int get_raw_sensor_temp(const NASConfig *cfg) {
    FILE *fp = fopen(cfg->temp_sensor_path, "r");
    if (!fp) return -1;
    int raw_temp = 0;
    if (fscanf(fp, "%d", &raw_temp) != 1) raw_temp = -1;
    fclose(fp);
    return (raw_temp == -1) ? -1 : (raw_temp / 1000);
}

/* Keep a short rolling average of temperature samples. */
int get_averaged_temperature(int new_sample) {
    if (new_sample == -1) return 999; // Sensor fault indicator

    temp_history[temp_index] = new_sample;
    temp_index = (temp_index + 1) % TEMP_SAMPLES;
    if (temp_count < TEMP_SAMPLES) {
        temp_count++;
    }

    int sum = 0;
    for (int i = 0; i < temp_count; i++) {
        sum += temp_history[i];
    }
    return sum / temp_count;
}

int write_pwm(const NASConfig *cfg, int pwm_val) {
    FILE *fp = fopen(cfg->pwm_file_path, "w");
    if (!fp) return 0;
    fprintf(fp, "%d", pwm_val);
    fclose(fp);
    return 1;
}

int read_pwm(const NASConfig *cfg) {
    FILE *fp = fopen(cfg->pwm_file_path, "r");
    if (!fp) return -1;

    int pwm_val = -1;
    if (fscanf(fp, "%d", &pwm_val) != 1) {
        pwm_val = -1;
    }
    fclose(fp);
    return pwm_val;
}

int init_fan_hardware(const NASConfig *cfg) {
    FILE *fp = fopen(cfg->pwm_enable_path, "w");
    if (!fp) {
        syslog(LOG_ERR, "fan-control: Failed to enable manual PWM control.");
        return 0;
    }
    fprintf(fp, "1");
    fclose(fp);
    
    // Initializing fan speed to minimum configured value
    write_pwm(cfg, 0);
    app_debug("fan-control: Hardware manual control enabled. Fan started at 0 PWM.");
    return 1;
}

int smooth_step(int current, int target) {
    int diff = target - current;
    int chg = (diff * 1) / 10; // SMOOTH_NUM / SMOOTH_DEN (1/10)

    if (chg == 0 && diff != 0) {
        chg = (diff > 0) ? 1 : -1;
    }
    return current + chg;
}

/* Calculate and apply the next fan PWM value from the current temperature. */
int run_fan_control_cycle(const NASConfig *cfg, int last_pwm) {
    int raw_temp = get_raw_sensor_temp(cfg);
    int cur_temp = get_averaged_temperature(raw_temp);
    int target_pwm = cfg->fan_min_pwm;

    // 1. Critical Overtemperature handling
    if (cur_temp >= cfg->fan_crit_temp && cur_temp < 999) {
        syslog(LOG_EMERG, "fan-control: CRITICAL temperature %d C reached. Emergency shutdown.", cur_temp);
        write_pwm(cfg, cfg->fan_max_pwm);
        sync();
        (void)system("poweroff -f");
        exit(EXIT_FAILURE);
    }

    // 2. Hardware Error / Missing Sensor -> fallback to full speed safety
    if (cur_temp >= 999) {
        target_pwm = cfg->fan_max_pwm;
    } 
    // 3. Below threshold -> keep off/min duty
    else if (cur_temp <= cfg->fan_start_temp) {
        target_pwm = cfg->fan_min_pwm;
    } 
    // 4. Exceeded maximum thresholds -> full duty
    else if (cur_temp >= cfg->fan_max_temp) {
        target_pwm = cfg->fan_max_pwm;
    } 
    // 5. Dynamic scale duty ramp
    else {
        int temp_range = cfg->fan_max_temp - cfg->fan_start_temp;
        int pwm_range = cfg->fan_max_pwm - cfg->fan_min_pwm;
        int temp_offset = cur_temp - cfg->fan_start_temp;
        target_pwm = cfg->fan_min_pwm + (temp_offset * pwm_range / temp_range);
    }

    int next_pwm = smooth_step(last_pwm, target_pwm);
    write_pwm(cfg, next_pwm);

    if (next_pwm != last_pwm) {
        app_debug("fan-control: Temperature %d C -> PWM %d", cur_temp, next_pwm);
    }
    return next_pwm;
}

static long long get_disk_total_sectors(const char *device_path) {
    const char *dev_name = device_path;
    if (strncmp(dev_name, "/dev/", 5) == 0) {
        dev_name += 5; // e.g., "/dev/sda" -> "sda"
    }

    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) return -1;

    char line[256];
    unsigned int major, minor;
    char name[32];
    unsigned long reads_completed, reads_merged, sectors_read, time_reading;
    unsigned long writes_completed, writes_merged, sectors_written;
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        int parsed = sscanf(line, "%u %u %31s %lu %lu %lu %lu %lu %lu %lu",
                            &major, &minor, name,
                            &reads_completed, &reads_merged, &sectors_read, &time_reading,
                            &writes_completed, &writes_merged, &sectors_written);
        
        if (parsed >= 10 && strcmp(name, dev_name) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);

    if (!found) return -1;

    return (long long)(sectors_read + sectors_written);
}

void handle_software_spindown_cycle(const NASConfig *cfg) {
    if (!cfg->spindown_enabled) return;

    time_t now = time(NULL);

    for (int i = 0; i < cfg->spindown_drive_count; i++) {
        const char *drive = cfg->spindown_drive[i];
        int timeout_sec = cfg->spindown_timeout[i];

        if (timeout_sec <= 0) continue; // Spindown disabled for this drive

        SoftwareSpindownTracker *tracker = &spindown_trackers[i];

        // 1. Initialize tracker on first run
        if (!tracker->initialized) {
            strncpy(tracker->device_path, drive, sizeof(tracker->device_path) - 1);
            long long sectors = get_disk_total_sectors(drive);
            if (sectors >= 0) {
                tracker->last_io_sectors = (unsigned long)sectors;
                tracker->last_active_time = now;
                tracker->is_spun_down = 0;
                tracker->initialized = 1;
                app_debug("Spindown tracker initialized for %s. Timeout: %d sec.", drive, timeout_sec);
            }
            continue;
        }

        // 2. Read current sectors
        long long current_sectors = get_disk_total_sectors(drive);
        if (current_sectors < 0) {
            // Error reading stats, skip this cycle to avoid false triggers
            continue;
        }

        // 3. Check for I/O activity
        if ((unsigned long)current_sectors != tracker->last_io_sectors) {
            // Activity detected! Reset the idle timer.
            tracker->last_io_sectors = (unsigned long)current_sectors;
            tracker->last_active_time = now;

            if (tracker->is_spun_down) {
                app_debug("Activity detected on %s! Drive woke up naturally.", drive);
                tracker->is_spun_down = 0;
            }
        } else {
            // No activity. Check if the idle period exceeds the timeout.
            int idle_duration = (int)(now - tracker->last_active_time);

            if (idle_duration >= timeout_sec) {
                if (!tracker->is_spun_down) {
                    syslog(LOG_INFO, "Drive %s idle for %d seconds. Sending standby command.", drive, idle_duration);
                    app_debug("Drive %s idle for %d seconds. Sending standby command.", drive, idle_duration);
                    
                    // Issue immediate standby/spin-down command safely
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd), "/sbin/hdparm -y %s", drive);
                    run_command(cmd);

                    tracker->is_spun_down = 1;
                }
            } else {
                // Optional debug logging to track countdown
                if (!tracker->is_spun_down && cfg->debug_enabled) {
                    app_debug("Drive %s is quiet. Spindown in %d seconds.", drive, timeout_sec - idle_duration);
                }
            }
        }
    }
}

int get_drive_power_state(const char *device_path) {
    int fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    unsigned char args[4] = {0xE5, 0, 0, 0};
    if (ioctl(fd, HDIO_DRIVE_CMD, &args) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    if (args[2] == 0xFF) return 1;
    if (args[2] == 0x00) return 0;
    return 1;
}

static int is_smartctl_available(void) {
    static int checked = 0;
    static int available = 0;
    if (checked) return available;
    checked = 1;

    FILE *fp = popen("command -v smartctl 2>/dev/null", "r");
    if (!fp) return 0;

    char path[256];
    if (fgets(path, sizeof(path), fp) != NULL) {
        available = 1;
    }
    pclose(fp);
    return available;
}

void get_drive_smart_state(const char *device_path, int is_active, char *output, size_t max_len) {
    if (is_active == 0) {
        strncpy(output, "Standby (Idle)", max_len);
        return;
    }

    if (!is_smartctl_available()) {
        strncpy(output, "SMART unavailable", max_len);
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "smartctl -c %s 2>/dev/null", device_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        strncpy(output, "SMART unavailable", max_len);
        return;
    }

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Self-test execution status")) {
            found = 1;
            char *pct_ptr = strstr(line, "% of test remains");
            if (pct_ptr) {
                char *start = pct_ptr;
                while (start > line && *(start - 1) != ' ') {
                    start--;
                }
                int remains = atoi(start);
                snprintf(output, max_len, "Running (%d%% left)", remains);
            } else if (strstr(line, "completed without error") || strstr(line, "was interrupted") || strstr(line, "was aborted")) {
                strncpy(output, "OK (Idle)", max_len);
            } else if (strstr(line, "failed")) {
                strncpy(output, "Fault (Failed)", max_len);
            } else {
                strncpy(output, "OK", max_len);
            }
            break;
        }
    }
    pclose(fp);

    if (!found) {
        strncpy(output, "SMART unavailable", max_len);
    }
}

void get_filesystem_mode(char *output, size_t max_len) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        strncpy(output, "unknown", max_len);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char dev[128], mount[128], type[64], options[128];
        if (sscanf(line, "%127s %127s %63s %127s", dev, mount, type, options) == 4) {
            if (strcmp(mount, "/") == 0) {
                if (strstr(options, "ro,") == options || strcmp(options, "ro") == 0 || strstr(options, ",ro")) {
                    strncpy(output, "ro", max_len);
                } else {
                    strncpy(output, "rw", max_len);
                }
                fclose(fp);
                return;
            }
        }
    }
    fclose(fp);
    strncpy(output, "unknown", max_len);
}

float get_cpu_temperature(const NASConfig *cfg) {
    int raw_temp = get_raw_sensor_temp(cfg);
    return (raw_temp == -1) ? 0.0 : (float)raw_temp;
}

float get_load_1m() {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return 0.0;
    float load = 0.0;
    if (fscanf(fp, "%f", &load) != 1) load = 0.0;
    fclose(fp);
    return load;
}

float get_cpu_usage() {
    long double a[4], b[4];
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0;
    int ret = fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &a[0], &a[1], &a[2], &a[3]);
    fclose(fp);
    if (ret < 4) return 0.0;

    // Use a tiny 500ms baseline sleep for CPU scaling assessment to keep monitor loops snappy
    usleep(500000);

    fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0;
    ret = fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &b[0], &b[1], &b[2], &b[3]);
    fclose(fp);
    if (ret < 4) return 0.0;

    long double total = (b[0]+b[1]+b[2]+b[3]) - (a[0]+a[1]+a[2]+a[3]);
    long double idle = b[3] - a[3];
    if (total == 0) return 0.0;
    return ((total - idle) / total) * 100.0;
}

float get_memory_usage() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0;

    char line[128];
    long long total_mem = 0;
    long long avail_mem = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %lld", &total_mem);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line, "MemAvailable: %lld", &avail_mem);
        }

        if (total_mem > 0 && avail_mem > 0) break;
    }
    fclose(fp);

    if (total_mem == 0) return 0.0;
    return ((float)(total_mem - avail_mem) / (float)total_mem) * 100.0;
}

void collect_metrics(NASConfig *cfg, NASMetrics *metrics, const char *mount_point, int current_pwm) {
    struct sysinfo si;
    struct statfs sf;

    metrics->cpu_usage = get_cpu_usage();
    metrics->cpu_temp = get_cpu_temperature(cfg);
    metrics->load_1m = get_load_1m();
    metrics->sda_active = get_drive_power_state("/dev/sda");
    metrics->sdb_active = get_drive_power_state("/dev/sdb");

    int pwm_value = read_pwm(cfg);
    metrics->fan_pwm = (pwm_value >= 0) ? pwm_value : current_pwm;

    get_drive_smart_state("/dev/sda", metrics->sda_active, metrics->sda_smart, sizeof(metrics->sda_smart));
    get_drive_smart_state("/dev/sdb", metrics->sdb_active, metrics->sdb_smart, sizeof(metrics->sdb_smart));
    get_filesystem_mode(metrics->fs_mode, sizeof(metrics->fs_mode));

    metrics->mem_usage = get_memory_usage();

    if (sysinfo(&si) == 0) {
        metrics->uptime = si.uptime;
    }

    if (statfs(mount_point, &sf) == 0) {
        long long total_bytes = (long long)sf.f_blocks * sf.f_bsize;
        long long free_bytes = (long long)sf.f_bavail * sf.f_bsize;
        if (total_bytes > 0) {
            metrics->disk_usage = ((float)(total_bytes - free_bytes) / (float)total_bytes) * 100.0;
            metrics->disk_free_gb = (float)free_bytes / (1024.0 * 1024.0 * 1024.0);
        }
    } else {
        metrics->disk_usage = 0.0;
        metrics->disk_free_gb = 0.0;
    }
}

void publish(MQTTClient client, char* topic, char* payload, int retain) {
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = payload;
    pubmsg.payloadlen = (int)strlen(payload);
    pubmsg.qos = 1;
    pubmsg.retained = retain;
    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, topic, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, 1000L);
}

const char* device_json = ",\"dev\":{\"ids\":[\"nas_monitor_01\"],\"name\":\"NAS Monitor\",\"mdl\":\"CS-WV Custom NAS\",\"mf\":\"Buildroot Linux\"}";

void send_discovery_configs(MQTTClient client, const NASConfig *cfg, int fan_enabled) {
    char topic[256];
    char payload[1024];

    // --- SENSORS DISCOVERY ---
    snprintf(topic, sizeof(topic), "%s_cpu/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS CPU Usage\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.cpu | round(1) }}\",\"unit_of_meas\":\"%%\",\"uniq_id\":\"nas_cpu_pct\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_temp/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS CPU Temperature\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.temp | round(1) }}\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"uniq_id\":\"nas_cpu_temp\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_load/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS CPU Load 1m\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.load | round(2) }}\",\"uniq_id\":\"nas_cpu_load1m\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_ram/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Memory Usage\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.ram | round(1) }}\",\"unit_of_meas\":\"%%\",\"uniq_id\":\"nas_ram_pct\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_disk/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Storage Allocation\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.disk | round(1) }}\",\"unit_of_meas\":\"%%\",\"uniq_id\":\"nas_disk_pct\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_disk_free/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Storage Free Space\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.disk_free | round(1) }}\",\"unit_of_meas\":\"GB\",\"dev_cla\":\"data_size\",\"uniq_id\":\"nas_disk_free_gb\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_uptime/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Uptime\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.uptime }}\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"uniq_id\":\"nas_uptime_seconds\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_fs_mode/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Root Filesystem Mode\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.fs_mode | upper }}\",\"icon\":\"mdi:folder-lock-open\",\"uniq_id\":\"nas_fs_mode_status\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    if (fan_enabled) {
        snprintf(topic, sizeof(topic), "%s_fan_pwm/config", DISCOVERY_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Fan Speed PWM\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.fan_pwm }}\",\"unit_of_meas\":\"PWM\",\"icon\":\"mdi:fan\",\"uniq_id\":\"nas_fan_pwm\"%s}", STATE_TOPIC, device_json);
        publish(client, topic, payload, 1);
    }

    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/nas_monitor_sda/config");
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Drive sda Status\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ 'ON' if value_json.sda_active == 1 else 'OFF' }}\",\"dev_cla\":\"power\",\"uniq_id\":\"nas_binary_sda\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/nas_monitor_sdb/config");
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Drive sdb Status\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ 'ON' if value_json.sdb_active == 1 else 'OFF' }}\",\"dev_cla\":\"power\",\"uniq_id\":\"nas_binary_sdb\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_sda_smart/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Drive sda SMART\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.sda_smart }}\",\"uniq_id\":\"nas_sda_smart_status\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    snprintf(topic, sizeof(topic), "%s_sdb_smart/config", DISCOVERY_PREFIX);
    snprintf(payload, sizeof(payload), "{\"name\":\"NAS Drive sdb SMART\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.sdb_smart }}\",\"uniq_id\":\"nas_sdb_smart_status\"%s}", STATE_TOPIC, device_json);
    publish(client, topic, payload, 1);

    // --- BUTTONS DISCOVERY ---
    if (cfg->mqtt_btn_mode_rw_enabled) {
        snprintf(topic, sizeof(topic), "%s_mode_rw/config", BUTTON_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Mode Read-Write\",\"cmd_t\":\"%smode_rw\",\"uniq_id\":\"nas_btn_mode_rw\"%s}", CMD_TOPIC_BASE, device_json);
        publish(client, topic, payload, 1);
    }

    if (cfg->mqtt_btn_mode_ro_enabled) {
        snprintf(topic, sizeof(topic), "%s_mode_ro/config", BUTTON_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Mode Read-Only\",\"cmd_t\":\"%smode_ro\",\"uniq_id\":\"nas_btn_mode_ro\"%s}", CMD_TOPIC_BASE, device_json);
        publish(client, topic, payload, 1);
    }

    if (cfg->mqtt_btn_reboot_enabled) {
        snprintf(topic, sizeof(topic), "%s_reboot/config", BUTTON_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Reboot System\",\"cmd_t\":\"%sreboot\",\"uniq_id\":\"nas_btn_reboot\",\"device_class\":\"restart\"%s}", CMD_TOPIC_BASE, device_json);
        publish(client, topic, payload, 1);
    }

    if (cfg->mqtt_btn_shutdown_enabled) {
        snprintf(topic, sizeof(topic), "%s_shutdown/config", BUTTON_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Shutdown System\",\"cmd_t\":\"%sshutdown\",\"uniq_id\":\"nas_btn_shutdown\"%s}", CMD_TOPIC_BASE, device_json);
        publish(client, topic, payload, 1);
    }

    if (cfg->mqtt_btn_sleep_enabled) {
        snprintf(topic, sizeof(topic), "%s_sleep/config", BUTTON_PREFIX);
        snprintf(payload, sizeof(payload), "{\"name\":\"NAS Suspend System\",\"cmd_t\":\"%ssleep\",\"uniq_id\":\"nas_btn_sleep\"%s}", CMD_TOPIC_BASE, device_json);
        publish(client, topic, payload, 1);
    }

    if (cfg->mqtt_btn_spindown_enabled) {
        for (int i = 0; i < cfg->spindown_drive_count; i++) {
            const char *drive = cfg->spindown_drive[i];
            int timeout_sec = cfg->spindown_timeout[i];
            if (timeout_sec <= 0) continue;
            if (!drive || drive[0] == '\0') continue;

            // Extract token from /dev/sda -> sda (also supports already-token strings)
            const char *token = drive;
            if (strncmp(token, "/dev/", 5) == 0) token += 5;

            snprintf(topic, sizeof(topic), "%s_%s_spin/config", BUTTON_PREFIX, token);
            snprintf(payload, sizeof(payload),
                     "{\"name\":\"NAS Spin Down %s\",\"cmd_t\":\"%s%s_spin\",\"uniq_id\":\"nas_btn_%s_spin\"%s}",
                     token, CMD_TOPIC_BASE, token, token, device_json);
            publish(client, topic, payload, 1);
        }
    }
}


static int read_drive_standby(const char *device_path) {
    return get_drive_power_state(device_path);
}

static int get_disk_almost_full_pct(const NASConfig *cfg, const char *mount_point) {
    (void)cfg;
    struct statfs sf;
    if (statfs(mount_point, &sf) != 0) return 0;

    long long total_bytes = (long long)sf.f_blocks * sf.f_bsize;
    long long free_bytes = (long long)sf.f_bavail * sf.f_bsize;
    if (total_bytes <= 0) return 0;

    long long used_bytes = total_bytes - free_bytes;
    int pct = (int)((used_bytes * 100) / total_bytes);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static int set_led_trigger_none(const NASConfig *cfg, const char *led_name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/trigger", cfg->led_sysfs_base_path, led_name);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        app_debug("led-control: trigger node unavailable for %s", led_name);
        return 0;
    }

    if (fprintf(fp, "none") < 0) {
        app_debug("led-control: failed to disable trigger for %s", led_name);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    app_debug("led-control: disabled trigger for %s", led_name);
    return 1;
}

static int write_led_brightness(const NASConfig *cfg, const char *led_name, int brightness) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/brightness", cfg->led_sysfs_base_path, led_name);

    (void)set_led_trigger_none(cfg, led_name);
    /* Ensure we always see this at runtime even if debug is off */
    fprintf(stderr, "WRITE_ENTER: %s -> %d\n", led_name, brightness);
    app_debug("led-control: writing %s -> brightness=%d", led_name, brightness);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        syslog(LOG_ERR, "led-control: Failed to open %s", path);
        app_debug("led-control: open failed for %s", path);
        return 0;
    }

    if (fprintf(fp, "%d\n", brightness) < 0) {
        syslog(LOG_ERR, "led-control: Failed to write brightness=%d to %s", brightness, path);
        app_debug("led-control: write failed for %s brightness=%d", path, brightness);
        fclose(fp);
        return 0;
    }

    if (fclose(fp) != 0) {
        syslog(LOG_ERR, "led-control: Failed to close %s", path);
        app_debug("led-control: close failed for %s", path);
        return 0;
    }

    app_debug("led-control: wrote %s -> brightness=%d", led_name, brightness);
    return 1;
}

static int read_power_switch_states(const NASConfig *cfg, int *power_on, int *auto_mode) {
    unsigned long sw_state[(SW_MAX / (8 * sizeof(unsigned long))) + 1];
    int fd = open(cfg->power_switch_input_device, O_RDONLY);
    if (fd < 0) return -1;

    if (ioctl(fd, EVIOCGSW(sizeof(sw_state)), sw_state) >= 0) {
        *power_on = (sw_state[0] & (1UL << cfg->power_sw_code)) ? 1 : 0;
        *auto_mode = (sw_state[0] & (1UL << cfg->auto_sw_code)) ? 1 : 0;
        close(fd);
        return 0;
    }

    close(fd);
    return -1;
}

static void run_power_switch_monitor_cycle(const NASConfig *cfg) {
    if (!cfg->power_switch_monitor_enabled) return;

    static int sw_fd = -1;
    static int power_on = -1;
    static int auto_mode = -1;

    if (sw_fd < 0) {
        sw_fd = open(cfg->power_switch_input_device, O_RDONLY | O_NONBLOCK);
        if (sw_fd < 0) {
            syslog(LOG_ERR, "power-switch: failed to open %s: %s", cfg->power_switch_input_device, strerror(errno));
            return;
        }

        if (read_power_switch_states(cfg, &power_on, &auto_mode) < 0) {
            syslog(LOG_ERR, "power-switch: failed to read initial switch state from %s", cfg->power_switch_input_device);
            // keep the fd open; future reads may succeed
        } else {
            app_debug("power-switch: initial state power_on=%d auto_mode=%d", power_on, auto_mode);
        }
    }

    struct input_event ev;
    ssize_t rv;
    while ((rv = read(sw_fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type != EV_SW) continue;

        if (ev.code == cfg->power_sw_code) {
            power_on = ev.value;
            app_debug("power-switch: power switch code %d value %d", ev.code, ev.value);
        }

        if (ev.code == cfg->auto_sw_code) {
            auto_mode = ev.value;
            app_debug("power-switch: auto switch code %d value %d", ev.code, ev.value);
        }
    }

    if (rv < 0 && errno != EAGAIN) {
        syslog(LOG_ERR, "power-switch: read error on %s: %s", cfg->power_switch_input_device, strerror(errno));
        close(sw_fd);
        sw_fd = -1;
        return;
    }

    if (power_on == 0 && auto_mode == 0) {
        syslog(LOG_INFO, "power-switch: power_off and auto_off detected -> shutdown");
        sleep(cfg->off_shutdown_delay_sec);
        do_shutdown_now();
        exit(EXIT_FAILURE);
    }
}




static void run_led_control_cycle(const NASConfig *cfg, const NASMetrics *metrics) {

    if (!cfg->led_control_enabled) return;

    int processed = 0;
    for (int i = 0; i < MAX_LEDS; i++) {
        const LedConfig *l = &cfg->leds[i];

        if (!l->enabled) continue;
        processed++;
        app_debug("led-control: processing enabled LED %d: %s", processed, l->led_name);
        if (l->led_name[0] == '\0') continue;

        LedPurpose purpose = l->purpose;
        if (purpose == LED_PURPOSE_DISABLED) {
            app_debug("led-control: %s has no explicit purpose configured; using default standby behavior", l->led_name);
            purpose = LED_PURPOSE_STANDBY;
        }

        app_debug("led-control: evaluating %s (purpose=%s, disk=%s, threshold=%d, on_value=%d, off_value=%d)",
                  l->led_name,
                  led_purpose_name(purpose),
                  l->disk_device[0] ? l->disk_device : "/dev/sda",
                  purpose == LED_PURPOSE_TEMP_LIMIT ? l->temp_threshold_c : l->disk_almost_full_pct,
                  l->on_value,
                  l->off_value);

        int should_on = 0;
        switch (purpose) {
            case LED_PURPOSE_STANDBY: {
                const char *disk_path = l->disk_device[0] ? l->disk_device : "/dev/sda";
                int standby = read_drive_standby(disk_path);
                app_debug("led-control: %s standby test -> disk=%s standby=%d", l->led_name, disk_path, standby);
                should_on = (standby == 1);
                break;
            }
            case LED_PURPOSE_TEMP_LIMIT: {
                float temp = metrics->cpu_temp;
                app_debug("led-control: %s temp test -> cpu_temp=%.2f threshold=%d", l->led_name, temp, l->temp_threshold_c);
                should_on = (temp >= (float)l->temp_threshold_c);
                break;
            }
            case LED_PURPOSE_DISK_ALMOST_FULL: {
                int used_pct = get_disk_almost_full_pct(cfg, cfg->storage_mount);
                app_debug("led-control: %s disk test -> mount=%s used_pct=%d threshold=%d", l->led_name, cfg->storage_mount, used_pct, l->disk_almost_full_pct);
                should_on = (used_pct >= l->disk_almost_full_pct);
                break;
            }
            default:
                should_on = 0;
                break;
        }

        int val = should_on ? l->on_value : l->off_value;
        if (last_led_brightness[i] == val) {
            app_debug("led-control: %s brightness already %d; skipping redundant write", l->led_name, val);
            continue;
        }

        int wrote = write_led_brightness(cfg, l->led_name, val);
        if (wrote) {
            last_led_brightness[i] = val;
        }
        app_debug("led-control: %s purpose=%s should_on=%d value=%d wrote=%d", l->led_name, led_purpose_name(purpose), should_on, val, wrote);
    }
}

int message_arrived(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    (void)context;
    (void)topicLen;

    char* payload = (char*)message->payload;
    int len = message->payloadlen;


    char clean_payload[32] = {0};
    if (len < 31) {
        memcpy(clean_payload, payload, len);
    }

    app_debug("MQTT Command Received. Topic: %s, Payload: %s", topicName, clean_payload);

    if (strstr(topicName, "mode_rw")) {
        run_command("mount -o remount,rw / 2>/dev/null || btrfs property set / ro false 2>/dev/null");
    } else if (strstr(topicName, "mode_ro")) {
        run_command("mount -o remount,ro / 2>/dev/null || btrfs property set / ro true 2>/dev/null");
    } else if (strstr(topicName, "_spin")) {
        // Expected tokens in command name: <token>_spin where token is like sda/sdb
        // We support tokens defined in config spindown_drive[] to map them to real /dev paths.
        // Since we don't have cfg here, accept both /dev/sdX and sdX tokens.
        // If token is sdX -> use /dev/sdX.

        // Find token between last '/' and suffix '_spin'
        const char *last_slash = strrchr(topicName, '/');
        const char *spin_pos = strstr(topicName, "_spin");
        if (last_slash && spin_pos && spin_pos > last_slash) {
            char token[64] = {0};
            size_t tok_len = (size_t)(spin_pos - last_slash - 1);
            if (tok_len > 0 && tok_len < sizeof(token)) {
                memcpy(token, last_slash + 1, tok_len);
                token[tok_len] = '\0';

                char cmd[128];
                if (strncmp(token, "/dev/", 5) == 0) {
                    // token already a device path
                    snprintf(cmd, sizeof(cmd), "hdparm -y %s", token);
                } else {
                    snprintf(cmd, sizeof(cmd), "hdparm -y /dev/%s", token);
                }
                run_command(cmd);
            }
        }
    } else if (strstr(topicName, "reboot")) {
        run_command("reboot");
    } else if (strstr(topicName, "shutdown")) {
        run_command("poweroff");
    } else if (strstr(topicName, "sleep")) {
        run_command("systemctl suspend 2>/dev/null || echo mem > /sys/power/state");
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;  
}

int main() {
    openlog("nas-monitor", LOG_PID | LOG_CONS, LOG_USER);

    NASConfig config = {0};
    NASMetrics metrics = {0};

    if (!load_config(CONFIG_PATH, &config)) {

        syslog(LOG_ERR, "Configuration file %s missing or unreadable. Exiting.", CONFIG_PATH);
        closelog();
        return EXIT_FAILURE;
    }

    debug_enabled = isatty(fileno(stdout)) || config.debug_enabled;

    // Initialize fan hardware control path if enabled
    int last_pwm_val = 0;
    if (config.fan_control_enabled) {
        if (!init_fan_hardware(&config)) {
            syslog(LOG_ERR, "Failed to initialize fan controller. Disabling fan routine.");
            config.fan_control_enabled = 0;
        }
    }

    // MQTT module (optional)
    MQTTClient client;
    int mqtt_ready = 0;
    if (config.mqtt_enabled) {
        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;

        MQTTClient_create(&client, config.mqtt_address, config.client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
        conn_opts.keepAliveInterval = 60;
        conn_opts.cleansession = 1;

        if (strlen(config.username) > 0) {
            conn_opts.username = config.username;
            conn_opts.password = config.password;
        }

        MQTTClient_setCallbacks(client, NULL, NULL, message_arrived, NULL);

        if (MQTTClient_connect(client, &conn_opts) != MQTTCLIENT_SUCCESS) {
            syslog(LOG_ERR, "Failed to connect to MQTT broker at %s", config.mqtt_address);
            closelog();
            return EXIT_FAILURE;
        }

        if (config.ha_discovery_enabled) {
            int fan_sensor_available = config.fan_control_enabled || (read_pwm(&config) >= 0);
            send_discovery_configs(client, &config, fan_sensor_available);
        }
        MQTTClient_subscribe(client, "nas/monitor/command/#", 1);
        mqtt_ready = 1;
    } else {
        app_debug("MQTT disabled via config. Metrics publishing disabled.");
    }

    int spindown_idle_sec[MAX_SPINDOWN_DRIVES] = {0};
    const int loop_interval_sec = 30;

    while (1) {
        // 1. Optional Fan control cycle (runs every 30s)
        if (config.fan_control_enabled) {
            last_pwm_val = run_fan_control_cycle(&config, last_pwm_val);
        }


        collect_metrics(&config, &metrics, config.storage_mount, last_pwm_val);

        // 2a. Optional Power-switch monitoring (may shutdown)
        run_power_switch_monitor_cycle(&config);

        handle_software_spindown_cycle(&config);

        // 3. Optional LED control
        if (config.led_control_enabled) {
            // Simple implementation: refresh on every main-loop iteration.
            // Per-led poll interval can be added later if needed.
            run_led_control_cycle(&config, &metrics);
        }

        if (mqtt_ready) {
            char state_json[1600];
            snprintf(state_json, sizeof(state_json),

                    "{\"cpu\":%.2f,\"temp\":%.2f,\"load\":%.2f,\"ram\":%.2f,\"disk\":%.2f,\"disk_free\":%.2f,\"uptime\":%ld,\"sda_active\":%d,\"sdb_active\":%d,\"sda_smart\":\"%s\",\"sdb_smart\":\"%s\",\"fs_mode\":\"%s\",\"fan_pwm\":%d}",
                    metrics.cpu_usage, metrics.cpu_temp, metrics.load_1m, metrics.mem_usage, metrics.disk_usage, metrics.disk_free_gb,
                    metrics.uptime, metrics.sda_active, metrics.sdb_active, metrics.sda_smart, metrics.sdb_smart, metrics.fs_mode, metrics.fan_pwm);

            publish(client, STATE_TOPIC, state_json, 0);
        }

        sleep(29);
    }

    if (mqtt_ready) {
        MQTTClient_disconnect(client, 10000);
        MQTTClient_destroy(&client);
    }
    closelog();
    return EXIT_SUCCESS;
}
