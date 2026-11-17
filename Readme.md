# Elecrow ESP32-P4 Wi-Fi HaLow Example Firmware Build Tutorial

This tutorial covers the Elecrow ESP32-P4 + FGH100MABMD (Morse Micro MM6108) combination in this repository. It follows the general workflow from Seeed Studio's [Getting Started with Wi-Fi HaLow Module for XIAO](https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/), adapted for the P4 board profiles in this repository.

This document covers environment setup, example selection, build, flash, and verification. **It does not modify the existing example source code.** The S3 companion program is organized under `s3_example_code`; the existing `README.legacy-s3.md` and `main/src` directories are retained as reference material. The P4 examples do not use the S3 DVP, BLE, or UART provisioning implementation.

## 1. Identify the P4 silicon revision

This repository contains two independent P4 projects. Do not copy `sdkconfig`, `build`, or generated firmware from one directory into the other.

| Directory | ESP32-P4 silicon | HaLow chip | Relevant configuration |
| --- | --- | --- | --- |
| `halow_P4_V1.3` | rev1.3 (v1.x) | MM6108 | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_100=y` |
| `halow_P4_V3.2` | rev3.2 (v3.x) | MM6108 | `CONFIG_ESP32P4_REV_MIN_300=y` |

If the revision is unknown, open the serial monitor and check the ROM boot log for `chip revision`. The firmware must match the board: the V1.3 firmware is for rev1.x, and the V3.2 firmware is for rev3.x.

The current board profiles use these HaLow connections:

| Signal | P4 GPIO |
| --- | ---: |
| RESET_N | 9 |
| SPI SCK | 8 |
| SPI MOSI | 6 |
| SPI MISO | 7 (internal pull-up) |
| SPI CS | 27 |
| SPI IRQ | 28 |
| WAKE / BUSY | Not connected (`-1`) |

The example defaults currently select the `US` country code, 16 MB flash, MM6108, and `bcf_fgh100mabmd.bin`. These are defaults for the current board/module combination, not universal values. Confirm the HaLow pins, module model, BCF file, country code, network name, and password against the actual board, module, and gateway. Do not copy the example values blindly. The country code must match the deployment region and local radio regulations.

For different P4 wiring or a different HaLow module, use the actual schematic and module documentation to update the selected example's `sdkconfig.defaults.esp32p4`, including `CONFIG_MM_RESET_N`, `CONFIG_MM_SPI_*`, `CONFIG_MM_SPI_IRQ`, `CONFIG_MM_CHIP_*`, and `CONFIG_MM_BCF_FILE`, then reconfigure and rebuild. Each example directory has its own board-default file; modify only the example you intend to build. GPIOs must not conflict with other board peripherals, and the BCF must match the actual module/board. Set the network SSID and password in the example's `idf.py menuconfig`; set the country code with `CONFIG_HALOW_COUNTRY_CODE`.

## 2. Install the build environment

The P4 examples in this repository are built with **ESP-IDF v5.5.3**. Install that exact version and open **ESP-IDF PowerShell**. Verify the tools:

```powershell
idf.py --version
```

The output should report ESP-IDF v5.5.3. Before the first build, make sure Python, CMake, Ninja, and `idf.py` all come from the ESP-IDF v5.5.3 environment rather than another system installation. Other ESP-IDF versions are not validated by this tutorial.

Keep the repository path reasonably short. The commands below use `F:\wiki\~P4+halow-wifi` as an example; replace it with the actual path on your computer.

## 3. Build the minimal scan example

`scan` is the recommended first step for verifying the P4-to-HaLow connection. It initializes the module and scans for nearby HaLow APs without requiring a camera or SD card.

### P4 rev1.3

```powershell
cd F:\wiki\~P4+halow-wifi\halow_P4_V1.3\examples\scan
idf.py set-target esp32p4
idf.py build
```

### P4 rev3.2

```powershell
cd F:\wiki\~P4+halow-wifi\halow_P4_V3.2\examples\scan
idf.py set-target esp32p4
idf.py build
```

`set-target` loads `sdkconfig.defaults.esp32p4` from the current example directory. If the directory already contains an `sdkconfig` generated for another board or target, back up any local settings and run `idf.py set-target esp32p4` again to regenerate it.

After the build, the firmware is in the example's `build` directory. A successful build verifies compilation, linking, and image checksums; discovering an AP also depends on module power, SPI wiring, the antenna, and an available HaLow network.

## 4. Flash and view the log

Connect the P4 to the computer over USB, identify its serial port (for example, `COM9`), and run this in the same example directory:

```powershell
idf.py -p COM9 flash monitor
```

Exit the serial monitor with `Ctrl+]`. A normal boot should show the HaLow chip ID, firmware/BCF information, and scan results. When switching from another P4 project, use a complete `flash` so the bootloader, partition table, and application are all updated.

If the log contains `All hardware SPI startup probes failed`, do not treat that line alone as a final failure. Continue checking the chip ID, firmware version, RX alignment validation, and scan results. The shared HAL continues with its configured transport-diagnostic paths.

## 5. Build the STA connection example

`sta_connect` verifies WPA3-SAE association, DHCP, and UDP transmission. Change the SSID, password, and country code with `idf.py menuconfig`:

```powershell
cd F:\wiki\~P4+halow-wifi\halow_P4_V1.3\examples\sta_connect
# For V3.2, use the same path under halow_P4_V3.2
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
idf.py -p COM9 flash monitor
```

Open **HaLow sta_connect Example Configuration** and set the Wi-Fi SSID and password to the actual HaLow gateway configuration. The defaults are examples only. Successful connection logs include `Associated`, a `HaLow IP`, and the UDP transmission message. `UDP sent` only means the local send call succeeded; to verify reception, listen on UDP port 12345 at the gateway or destination host.

The [Elecrow ThinkNode-G4 Wi-Fi HaLow Gateway](https://www.elecrow.com/thinknode-g4-wi-fi-halow-gateway-support-wi-fi-wi-fi-halow-ethernet-connections-supports-ap-sta-mesh-etc.html) can be used as the companion gateway. First confirm its current HaLow mode, SSID, password, country/channel, and IP/DHCP settings, then enter the same network parameters in the P4 example. Gateway settings may differ by region or user configuration; use the actual device settings and local regulations.

## 6. Build the P4 camera web example

`web_camera_serve` is intended for a P4 board with an SC2336 MIPI-CSI camera, SD card, and HaLow module. It starts the camera and HTTP service after obtaining a HaLow IP address.

```powershell
cd F:\wiki\~P4+halow-wifi\halow_P4_V1.3\examples\web_camera_serve
# For V3.2, use the same path under halow_P4_V3.2
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
idf.py -p COM9 flash monitor
```

Before building, confirm in `idf.py menuconfig` that the connection parameters match the actual HaLow gateway. The startup log should show a HaLow IP, camera detection, `First JPEG ready`, and `HTTP ready`. Open the address printed in the log in a browser. HTTP port 80 provides the status/snapshot page; HTTP port 81 provides the MJPEG preview. Without an SD card, web preview and download can still work, but photo saving is unavailable.

The V3.2 version includes notes for P4 rev3.x ISP initialization; the V1.3 version uses the rev1.x profile. Do not overwrite the current project with an `sdkconfig` or `build` directory from the other revision.

## 7. Troubleshooting

### `idf.py` is missing or reports the wrong version

Close the regular PowerShell and reopen ESP-IDF PowerShell. Check `idf.py --version` and `where.exe idf.py` to confirm that the path points to the ESP-IDF v5.5.3 installation.

### The build reports a target mismatch

Run `idf.py set-target esp32p4` from the selected example directory. Do not reuse `sdkconfig` between the V1.3 and V3.2 projects.

### The board boots but finds no AP

Confirm module power and antenna connection. Check RESET, IRQ, SPI pins, module model, and BCF settings against the actual hardware schematic. Confirm that the gateway is running a HaLow AP and that its SSID, country code, and channel match the board configuration and local regulations. Keep the complete log from power-on through scan completion; `target responses=0` means that the target SSID was not observed in that scan and does not by itself indicate a password error.

### STA is associated but has no IP address

The wireless link is already established. Check the HaLow gateway's DHCP service, subnet, and address pool before changing the camera or SPI configuration.

### Which firmware should be used for V1.3 and V3.2?

Select the directory matching the silicon revision in the boot log: `rev1.3` uses `halow_P4_V1.3`, and `rev3.2` uses `halow_P4_V3.2`. Both use the MM6108 HaLow module; the main difference is the P4 silicon revision and its corresponding ESP-IDF/ISP configuration.

## 8. Relationship to the S3 companion program

Elecrow's S3 companion program and source material are available in [Wireless-Module-for-Wi-Fi-HaLow](https://github.com/Elecrow-RD/Wireless-Module-for-Wi-Fi-HaLow). In this repository, the original S3 companion files are named or grouped as `s3_example_code`; the legacy `README.legacy-s3.md` files document the original S3 web-camera workflow. The P4 examples can be tested with the [ThinkNode-G4 Wi-Fi HaLow Gateway](https://www.elecrow.com/thinknode-g4-wi-fi-halow-gateway-support-wi-fi-wi-fi-halow-ethernet-connections-supports-ap-sta-mesh-etc.html); configure the SSID, password, country code, and channel according to the actual devices before connecting. The S3 code is retained for reference, but P4 builds use the ESP-IDF examples and P4-specific `sdkconfig.defaults.esp32p4` in their respective directories. Do not copy the S3 DVP camera, BLE provisioning, or UART pin configuration directly into a P4 project.

The `s3_example_code` directory is for the S3 companion implementation only. It is not an alternative build target for the P4 projects, and its S3-specific pin assignments, camera interface, BLE provisioning, and UART configuration must not be applied to `halow_P4_V1.3` or `halow_P4_V3.2`.

For more example-specific information, see the `README.md` in each directory:

- `examples/scan`: scan for HaLow APs.
- `examples/sta_connect`: STA association, DHCP, and UDP.
- `examples/web_camera_serve`: P4 camera web service.
- `examples/iperf`: throughput testing.
- `examples/porting_assistant`: new-board hardware diagnostics.

