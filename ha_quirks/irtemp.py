"""ZHA Quirk for irTemp ESP32-H2 IR Temperature Sensor.

Place this file in your Home Assistant config directory:
  config/custom_zha_quirks/irtemp.py

Then add to configuration.yaml:
  zha:
    custom_quirks_path: /config/custom_zha_quirks/

Restart Home Assistant and reconfigure the device.

This device exposes:
  - Endpoint 1: Object temperature (IR measurement) + Update rate setting
  - Endpoint 2: Ambient temperature

Temperature sensors use standard ZCL Temperature Measurement cluster
and are auto-detected. Rename them in HA UI if needed:
  - Endpoint 1 = Object (IR) temperature
  - Endpoint 2 = Ambient temperature

This quirk exposes the update rate setting as a configurable number entity.
"""

from zigpy.quirks.v2 import NumberDeviceClass, QuirkBuilder
from zigpy.zcl.clusters.general import AnalogInput


# irTemp: IR Temperature Sensor with MLX90614
(
    QuirkBuilder("ESPRESSIF", "IRTEMP_SENS")
    # Update Rate (Analog Input cluster) - value in seconds, supports decimals
    .number(
        AnalogInput.AttributeDefs.present_value.name,
        AnalogInput.cluster_id,
        endpoint_id=1,
        min_value=0.05,
        max_value=600,
        step=0.05,
        unit="s",
        device_class=NumberDeviceClass.DURATION,
        translation_key="update_rate",
        fallback_name="Update Rate",
    )
    .add_to_registry()
)
