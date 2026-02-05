# irTemp — IR Temperature Sensor over Zigbee

ESP32-H2 (M5Stack Nano H2) firmware that reads temperature from an MLX90614 infrared sensor and reports it to Home Assistant over Zigbee.

Exposes both object (IR) and ambient temperature as separate endpoints, with a configurable update rate.

## How it works

1. Reads the MLX90614 over I2C (SMBus protocol)
2. Reports object temperature on endpoint 1, ambient temperature on endpoint 2
3. Update interval is configurable from 50ms to 600 seconds (default: 30s)
4. Settings persist across reboots via NVS
5. Identify command triggers a double LED flash

## Hardware

- **Board:** M5Stack Nano H2 (ESP32-H2)
- **Sensor:** MLX90614 IR temperature sensor ([M5Stack NCIR Unit](https://shop.m5stack.com/products/ncir-sensor-unit))
- **I2C:** SDA = GPIO 2, SCL = GPIO 1, address 0x5A, 100kHz
- **LED:** GPIO 4 (identify flash)

## Zigbee endpoints

| Endpoint | Cluster | Purpose |
|----------|---------|---------|
| 1 | Temperature Measurement (0x0402) | Object (IR) temperature |
| 1 | Analog Input (0x000F) | Update rate setting (seconds, writable) |
| 1 | Identify (0x0003) | LED flash for identification |
| 1 | Basic (0x0000) | Device info (model: IRTEMP_SENS) |
| 2 | Temperature Measurement (0x0402) | Ambient temperature |
| 2 | Basic (0x0000) | Device info (model: IRTEMP_AMBNT) |

## Temperature ranges

| Measurement | Min | Max |
|-------------|-----|-----|
| Object (IR) | -70.00 C | 327.67 C |
| Ambient | -40.00 C | 125.00 C |

## Configuration

| Setting | Default | Range |
|---------|---------|-------|
| Update rate | 30 s | 0.05 to 600 s |

## Home Assistant quirk

Copy `ha_quirks/irtemp.py` to your HA config:

```
config/custom_zha_quirks/irtemp.py
```

Enable in `configuration.yaml`:

```yaml
zha:
  custom_quirks_path: /config/custom_zha_quirks/
```

The quirk exposes an **Update Rate** number entity (0.05 to 600 seconds, step 0.05s).

Temperature sensors on both endpoints are detected automatically by ZHA as standard temperature entities.

## Build & flash

Requires [ESP-IDF v5.3.3](https://github.com/espressif/esp-idf) and [just](https://github.com/casey/just).

```bash
# First time setup
just setup-idf
just set-target

# Build and flash (edit `port` in Justfile for your COM port)
just flash

# Monitor serial output
just monitor

# Or both at once
just run
```

Other commands:

```bash
just build          # build only
just erase          # erase flash
just factory-reset  # erase + reflash
just check          # verify toolchain setup
just ports          # list Windows COM ports
```

Flashing goes through Windows esptool (WSL -> PowerShell) since USB serial is on the Windows side.

## Project structure

```
irTemp/
├── Justfile                 # Build recipes
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   └── main/
│       ├── main.c           # All firmware code
│       ├── CMakeLists.txt
│       └── idf_component.yml
└── ha_quirks/
    └── irtemp.py            # ZHA quirk for update rate entity
```
