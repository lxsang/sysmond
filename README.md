Jetson Nano system monitor.

Simple service that monitors system resource and shutdowns the system
when the battery is low

This service require the ADS1115 is connected to the
Nano and is handled by the ads1015 linux driver

The battery voltage value is available on user space as the content of

```sh
cat /sys/class/hwmon/hwmon2/device/in3_input
```

Statistic information that the daemon outputs:
- Battery
- CPU usage
- Memory usage
- CPU/GPU temperature
- Network trafic
