# Linux System monitoring

A simple service that monitors and collects information on system resource such as battery, temperature, memory, CPU, and network usage.
The service can be used as backend for applications that need to consult system status.

![https://doc.iohub.dev/jarvis/asset//c_2/s_5/jarvis_monitoring.png](https://doc.iohub.dev/jarvis/asset//c_2/s_5/jarvis_monitoring.png)

*Example of web application that fetches data from `sysmond` and visualize it as real-time graphs*

`Sysmond` monitors resource available on the system via the user space **sysfs interface**.

The service logs all system information to application using a classic file-based interface, it can be configured to log system information to:
* Regular text file (such as log file)
* Name pipe (FIFO), the name pipe file should be previously created by application
* STDOUT
* UNIX socket domain, as with name pipe, the socket file must be previously created by application

Additionally, if configured correctly, the service can **monitor the system battery and automatically shutdown the system** when the battery is below a configured threshold.

`sysmond` is lightweight and can be used in embedded linux system, or single board computer such as Raspberry Pi or Jetson Nano.

## Build and install
The build process is done using autotool

```sh
libtoolize
aclocal
autoconf
automake --add-missing
./configure
# build
make
#install
make install
```

## Configuration

The default configuration file can be found in `/etc/sysmond.conf`.
Specific configuration file can be passed to the service using the `-f` option.

```sh
sysmond -f /path/to/your/sysmond.conf
```
### Battery monitoring configuration

The battery monitoring feature is helpful for battery-powered systems such as robotic systems.
On these systems, battery voltage reading is performed with the help of an ADC sensor (such as ADS1115).
As the service communicates with the system hardware via the **sysfs interface**, in order to provide input
to `sysmond`, these ADCs sensor should be accessible in user space via this interface. Usually, this is handled
by the device driver.

For example, an `ADS1115` linux driver will expose its ADC channel inputs in user space at
`/sys/class/hwmon/hwmon0/device/*`, if the battery is connected to the chanel 3 of the ADC sensor,
the battery voltage can be easily read with a simple `cat` command:

```sh
cat sys/class/hwmon/hwmon0/device/in3_input
# value in mV, example 4.1V
4120 
```
This kind of file need to be passed to `sysmond` configuration. The following configurations are availabe

```ini
# Max usable battery voltage
battery_max_voltage = 12600

# Min usable battery voltage
battery_min_voltage = 10000

# Below this voltage, the battery is unusable and is damaged
battery_cutoff_voltage = 9000

# if voltage divider is used, the R1+R2/ R2 ratio should be set to `battery_divide_ratio`, otherwise `1.0`
battery_divide_ratio = 3.36

# Battery input file. If this configuration is empty, the battery monitoring feature is disabled
battery_input = /sys/class/hwmon/hwmon2/device/in3_input

# When battery is low the system will be shutdown after n count down
power_off_count_down = 10

# the system will be shutdown if the battery voltage percent is below this value after `power_off_count_down` times
power_off_percent = 3
```

Based on these configuration, `sysmond` can approximate the battery voltage percent, it is also able to protect the battery by
powering off the system when the battery percent bellow the configured value.

### CPU, memory and storage usage configuration

```ini
# number of cpu cores to monitor, this value should be equal or less than the actual number of CPU cores in the system
# CPU usages are fetched from /proc/stat
cpu_core_number = 4

# memory usages are automatically fetch from /proc/meminfo, no configuration needed

# The mount point of the storage should be monitored
disk_mount_point = /
```

### Temperature configuration

```ini
# System temperature information can be found in /sys/devices/virtual/thermal/*
# CPU temperature
cpu_temperature_input=/sys/devices/virtual/thermal/thermal_zone1/temp

# GPU temperature
gpu_temperature_input=/sys/devices/virtual/thermal/thermal_zone2/temp
```

### Network monitoring configuration

```ini
# List of network interface to monitor
network_interfaces = wlan0,eth0
```

### Other configurations

```ini
# Sampling period in ms, example: 2Hz (2 samples per second) should be
sample_period = 500

# Output system info to file
# The output file may be: stdout, a regular file or name pipe, or a unix domain socket

# To print JSON data records to stdout use
data_file_out = stdout

# To send data via unix domain socket use
data_file_out = sock:/path/to/socket/file

# regular file or name pipe
data_file_out = /var/sysmond.log
```

## Output data format
System information is outputted in JSON format, example:

```json
{
	"stamp_sec": 1612363252,
	"stamp_usec": 890264,
	"battery": 0.000,
	"battery_percent": 0.000,
	"battery_max_voltage": 12600,
	"battery_min_voltage": 10000,
	"cpu_temp": 52582,
	"gpu_temp": 0,
	"cpu_usages": [1.500, 2.000, 0.000, 2.000, 2.000],
	"mem_total": 1891540,
	"mem_free": 62780,
	"mem_used": 729076,
	"mem_buff_cache": 1099684,
	"mem_available": 1360044,
	"mem_swap_total": 0,
	"mem_swap_free": 0,
	"disk_total": 877448515584,
	"disk_free": 876156772352,
	"net": [{
		"name": "eth0",
		"rx": 1529461109,
		"tx": 1704797921,
		"rx_rate": 132.000,
		"tx_rate": 1244.000
	}]
}
```

Note:
* Battery is in mV
* Temperature in is: Celsius\*1000
* Memory in KB
* Disk is in: bytes
* Network rate Kb/s
* CPU usages is in %, the first value in the list is the average CPU usages, second value is for cpu0, third value is for cpu1 and so on.

## Example configuration on Raspberry Pi

Recently i've used the Raspberry Pi 4 as my home server, `sysmond` is used to monitor the resource on this server, below is an example configuration:

```ini
# Battery monitoring is disabled
battery_max_voltage = 12600
battery_min_voltage = 10000
battery_cutoff_voltage = 9000
battery_divide_ratio = 3.36
# battery_input =

# daemon configuration
# time period between loop step in ms
sample_period = 500

#number of cpus to monitor
cpu_core_number = 4

# network interfaces to monitor
network_interfaces = eth0 
# e.g. wlan0,eth0

# disk mount point to monitor
disk_mount_point = /opt/cloud

# when battery is low
# the system will be shutdown after n count down
power_off_count_down = 10
# the system will bet shutdown if the battery voltage percent is below this value
power_off_percent = 3

cpu_temperature_input=/sys/devices/virtual/thermal/thermal_zone0/temp

#gpu_temperature_input=/sys/devices/virtual/thermal/thermal_zone2/temp

# output system info to file 
data_file_out = /var/fbf070ddea3ea90d07f456540b405d302554ec82
```

Since the raspberry is powered using "wall" power, and there is no ADC sensor available, the battery monitoring feature is disabled.
The service is output to a name pipe that is used by another application for visualization.
