# irTemp Project Justfile
# ESP32-H2 Zigbee IR Temperature Sensor (MLX90614)

# Default recipe
default:
    @just --list

# ============================================================================
# Configuration
# ============================================================================

# Serial port (Windows COM port)
port := "COM10"

# ESP-IDF paths
idf_path := env_var_or_default("IDF_PATH", home_directory() + "/esp/esp-idf")
idf_version := "v5.3.3"

# Project paths
firmware_dir := "firmware"
build_dir := firmware_dir + "/build"

# Windows temp for flashing
win_temp := `cmd.exe /c "echo %TEMP%" 2>/dev/null | tr -d '\r'`

# ============================================================================
# Setup (run once)
# ============================================================================

# Install ESP-IDF and dependencies
setup-idf:
    #!/usr/bin/env bash
    set -euo pipefail

    echo "=== Installing ESP-IDF prerequisites ==="
    sudo apt-get update
    sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
        cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

    echo "=== Cloning ESP-IDF {{idf_version}} ==="
    mkdir -p ~/esp
    cd ~/esp
    if [ -d "esp-idf" ]; then
        echo "esp-idf already exists, updating..."
        cd esp-idf
        git fetch
        git checkout {{idf_version}}
        git submodule sync --recursive
        git submodule update --init --recursive --force
    else
        git clone https://github.com/espressif/esp-idf.git
        cd esp-idf
        git checkout {{idf_version}}
        git submodule update --init --recursive --force
    fi

    echo "=== Running ESP-IDF install script ==="
    ./install.sh esp32h2

    echo ""
    echo "=== Setup complete! ==="
    echo "Add this to your ~/.bashrc or run before each session:"
    echo "  source ~/esp/esp-idf/export.sh"

# ============================================================================
# Build
# ============================================================================

# Source IDF environment (helper)
[private]
_idf-env:
    #!/usr/bin/env bash
    if [ -z "${IDF_PATH:-}" ]; then
        source ~/esp/esp-idf/export.sh > /dev/null 2>&1
    fi

# Set target to ESP32-H2 (run once after init)
set-target: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py set-target esp32h2

# Build the firmware
build: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py build

# Clean build artifacts
clean: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py fullclean

# Open menuconfig
menuconfig: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py menuconfig

# ============================================================================
# Flash (via Windows - COM ports)
# ============================================================================

# Copy build artifacts to Windows temp
[private]
_copy-to-windows:
    #!/usr/bin/env bash
    win_temp=$(wslpath "{{win_temp}}")/irtemp
    mkdir -p "$win_temp"
    cp {{build_dir}}/irtemp.bin "$win_temp/" 2>/dev/null || cp {{build_dir}}/*.bin "$win_temp/"
    cp {{build_dir}}/bootloader/bootloader.bin "$win_temp/"
    cp {{build_dir}}/partition_table/partition-table.bin "$win_temp/"
    echo "Copied to: $win_temp"

# Flash firmware to device (builds first)
flash: build _copy-to-windows
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    win_temp_path="{{win_temp}}\\irtemp"

    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} --baud 460800 write_flash --force \
         --flash_mode dio --flash_freq 48m --flash_size 4MB \
         0x0 '$win_temp_path\\bootloader.bin' \
         0x8000 '$win_temp_path\\partition-table.bin' \
         0x10000 '$win_temp_path\\irtemp.bin'"

# Flash without rebuilding
flash-only: _copy-to-windows
    #!/usr/bin/env bash
    win_temp_path="{{win_temp}}\\irtemp"

    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} --baud 460800 write_flash --force \
         --flash_mode dio --flash_freq 48m --flash_size 4MB \
         0x0 '$win_temp_path\\bootloader.bin' \
         0x8000 '$win_temp_path\\partition-table.bin' \
         0x10000 '$win_temp_path\\irtemp.bin'"

# Erase flash completely
erase:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} erase_flash"

# ============================================================================
# Monitor
# ============================================================================

# Monitor serial output (via Windows) - Ctrl+C to quit
monitor:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m serial.tools.miniterm --exit-char 3 {{port}} 115200"

# Build, flash, and monitor in one go
run: flash monitor

# ============================================================================
# Utilities
# ============================================================================

# Check if ESP-IDF is set up correctly
check:
    #!/usr/bin/env bash
    echo "=== Checking ESP-IDF setup ==="
    if [ -d "{{idf_path}}" ]; then
        echo "✓ ESP-IDF found at {{idf_path}}"
        source {{idf_path}}/export.sh 2>/dev/null && echo "✓ ESP-IDF environment loads correctly" || echo "✗ Failed to source export.sh"
    else
        echo "✗ ESP-IDF not found at {{idf_path}}"
        echo "  Run: just setup-idf"
    fi

    echo ""
    echo "=== Checking project structure ==="
    if [ -d "{{firmware_dir}}" ]; then
        echo "✓ Firmware directory exists"
        [ -f "{{firmware_dir}}/CMakeLists.txt" ] && echo "✓ CMakeLists.txt exists" || echo "✗ CMakeLists.txt missing"
        [ -f "{{firmware_dir}}/main/main.c" ] && echo "✓ main.c exists" || echo "✗ main.c missing"
    else
        echo "✗ Firmware directory not found"
    fi

    echo ""
    echo "=== Checking Windows tools ==="
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command "python --version" 2>/dev/null && echo "✓ Windows Python available" || echo "✗ Windows Python not found"
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command "python -m esptool version" 2>/dev/null && echo "✓ esptool available" || echo "✗ esptool not installed (pip install esptool)"

# Show connected serial ports (Windows)
ports:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "Get-WmiObject Win32_SerialPort | Select-Object DeviceID, Caption, Description | Format-Table -AutoSize"

# Factory reset the Zigbee device (erase NVS)
factory-reset: erase flash
