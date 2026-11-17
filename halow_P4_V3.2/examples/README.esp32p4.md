# ESP32-P4 example support

The P4 profiles target the Elecrow ESP32-P4 board (silicon revision 3.2, 16 MB
flash) with the directly stacked FGH100MABMD/MM6108 module. They configure the
board SPI pins, reset and IRQ, US channel list, module BCF, and the P4 revision
range required by ESP-IDF 5.5.3.

For an example with `sdkconfig.defaults.esp32p4`, select the target once from its
own directory, then build:

```powershell
idf.py set-target esp32p4
idf.py build
```

ESP-IDF layers `sdkconfig.defaults.esp32p4` over the example's generic defaults.
If the directory already has an `sdkconfig` from another target, `set-target`
regenerates it; keep a copy first if it contains local menuconfig changes.

| Example | P4 status |
| --- | --- |
| `scan` | P4 profile and board-specific diagnostic notes are provided. |
| `sta_connect` | P4 profile and board-specific connection notes are provided. |
| `softap` | Uses the P4 HaLow module profile; set a valid US S1G channel and operating class for the intended bandwidth. |
| `sta_reboot` | Uses the P4 HaLow module profile. |
| `iperf` | Uses the P4 HaLow module profile; connect before starting the serial REPL test. |
| `porting_assistant` | Uses the P4 HaLow module profile for transport and chip self-tests. |
| `web_camera_serve` | P4 implementation uses SC2336 MIPI-CSI and the P4 HaLow profile. |
| `dual_if` | Not supported on P4 as written: it requires the ESP's native 2.4 GHz Wi-Fi radio for its second interface, which ESP32-P4 does not contain. |

The profile selects the correct build target and board configuration; it does
not guarantee that a gateway is discovered or that a physical transport works.
Check runtime logs and the example-specific notes for those tests.
