This should be a NanoH2 with device (https://shop.m5stack.com/products/ncir-sensor-unit) MLX90614 IR temp sensor.
It should use zigbee to connect ot my local homeassistant.
It should expose a temperature sensor entity in homeassistant, and expose a setting value that decides the update rate of the temp in seconds. with min of 1 and default of 30, up to 600 seconds.
I think homeassistant also has a identify function for zigbee devices, I'd like that to flash the blue led twice for a second each. (this part I've not yet ever impl, so it still needs research!)
