# Elecrow ESP32-P4 scan configuration

## Photo-confirmed direct stacking: correction to the earlier diagnosis

The user's photographs show the HaLow module plugged directly into two soldered
headers, with its antenna connector at the top. There is no control ribbon cable
to remove. The earlier instructions to dismantle the direct connection were
based on an incorrect assumption that both right-hand headers number pins in
the same physical direction. Do not follow those earlier rewiring instructions.

Both Eagle schematic **and PCB package coordinates** have now been checked in:

- `SCH&PCB/ESP32-P4 Display 10.1 inch V1.2.sch` and `.brd`
- `SCH&PCB/Wireless Module for Wi-Fi Halow V1.0.sch` and `.brd`

J9, J11 and J3 number pins from top to bottom in the pictured orientation.
Module J4 is rotated 90 degrees, versus J3's 270 degrees, and numbers from
bottom to top. Thus direct stacking connects **J11.n to J4.(8-n)**.
For example, the module PCB puts J4.7/INT at Y=18.090 and J4.3/RESET_N
at Y=7.930, opposite J3.1 at Y=18.100 and J3.5 at Y=7.940.

The physical host is marked **Display 9.0 V1.2**, while the supplied design
files say 10.1. The photographed connector legend matches the required SPI,
RESET and IRQ GPIOs below. Other pins/revision details are not assumed identical.
In particular, no use is made of the right-hand IO45/IO46 pins, whose schematic
counterparts are shared I2C nets. Photos establish orientation and labels, not
electrical continuity or actual component population.

## Direct-stacked pin mapping

| Signal | ESP32-P4 GPIO | P4 header | HaLow header |
| --- | --- | --- | --- |
| CS | 27 | J9.1 | J3.1 |
| SCK | 8 | J9.2 | J3.2 |
| MISO | 7 | J9.3 | J3.3 |
| MOSI | 6 | J9.4 | J3.4 |
| 3.3 V | - | J9.5 | J3.5 |
| GND | - | J9.6 | J3.6 |
| RESET_N | 9 | J11.5 | J4.3 |
| IRQ / INT | 28 | J11.1 | J4.7 |

Right-hand rows, from the antenna end toward the bottom of the module:

| Physical row | Host legend / J11 pin | Module J4 pin and function |
| --- | --- | --- |
| 1 (top) | IO28 / J11.1 | J4.7: INT |
| 2 | IO46 / J11.2 | J4.6: no signal in module design |
| 3 | IO45 / J11.3 | J4.5: no signal in module design |
| 4 | NC / J11.4 | J4.4: BUSY |
| 5 | IO9 / J11.5 | J4.3: RESET_N |
| 6 | IO10 / J11.6 | J4.2: WAKEUP_IN via R1 (NC) |
| 7 (bottom) | NC / J11.7 | J4.1: no signal in module design |

GPIO10 is therefore **not INT** in this direct-stacked setup. The previous
IRQ=10 configuration and logged GPIO10 levels did not monitor the module IRQ.
The profile now uses IRQ=28 and keeps RESET=9. The schematic traces IRQ through
J11.1 -> R48 -> GPIO28 and J4.7 -> R6 -> module pad 26.

Module R1 is marked NC; R2 pulls its internal WAKEUP_IN high. Power-save GPIO
settings remain -1 and HaLow power saving remains disabled. GPIO10 is not driven
by this profile. Keep the pictured direct stacking in place; this profile does
not require moving soldered pins or adding reset/IRQ jumpers.

Power notes from the schematics: P4 J9.7 has an NC link L4; module J3.7 has an
NC link R10. The module FEM supply is connected to 3.3 V through R28 instead.
This configuration uses J3.5/J3.6 for power; the physically stacked seventh
contact is isolated by the NC links shown in the supplied schematics.
Check actual component population and adequate 3.3 V supply capacity on hardware.

## Firmware and BCF

The chip is MM6108, and the user's close-up photo identifies the actual ordering
code as **FGH100MABMD**. The selected file is now
`halow/components/firmware/morse-firmware/bcf/quectel/bcf_fgh100mabmd.bin`.
The local firmware repository's `WHENCE.md` lists it as FGH100M (US).

The former `bcf_fgh100mhaamd.bin` selection was based on the older vendor
project's BCF payload; it describes FGH100M-H and is superseded by the actual
module label. Both the active `sdkconfig` and target defaults are corrected.
BCF selection is used later in startup; changing it does not by itself explain
or establish a fix for the earlier CMD63 handshake failure.

The user confirmed that the board is operated in the United States, so this profile sets
`CONFIG_HALOW_COUNTRY_CODE="US"`. The placeholder `"??"` does not select a channel
list and causes `Channel list not set` during initialization and scanning.
If the operating location changes, select a country code supported by the
module/BCF and appropriate for that location.

## Build

The connected board reports ESP32-P4 silicon revision v1.3. ESP-IDF 5.5.3
treats v1.x and v3.x as mutually exclusive silicon families. The P4 profile
selects v1.x silicon:

```text
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

The resulting bootloader and application support revisions v1.0 through v1.99.
In menuconfig these options are under `Component config -> Hardware Settings ->
Chip revision`. Support for P4 revisions below v3.0 and v3.x is mutually
exclusive in this IDF version. Rebuild both bootloader and application after
changing this selection; do not bypass the revision check with `--force`.

Use ESP-IDF 5.5.3 (this checkout requires >=5.4.4 and <6.0), in an ESP-IDF terminal:

```powershell
Set-Location E:\ElecrowWork\WiFI_Halow\esp-halow-main-p4-V1_3\halow\examples\scan
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
```

In menuconfig, set `Wi-Fi HaLow Connection Manager -> Country code to use for
Wi-Fi HaLow` to the actual operating country. Also review the pin settings if
your wiring differs from the table. The target-specific defaults include the
required `CONFIG_MBEDTLS_CMAC_C=y` crypto option for ESP32-P4.

ESP-IDF defaults only seed new/unset settings. They do not override an existing
`sdkconfig`; update existing values through menuconfig when changing boards.
The current local `sdkconfig` has already been updated with this pin/BCF profile.
No hardware scan is performed by these configuration changes.

The boot log detected 16384 KiB of SPI flash. This profile therefore selects
`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` to match the hardware and remove the 2 MB image
header warning. The existing application partition layout is unchanged.

## Command timeout diagnostics

The current P4 profile requests a 10 MHz HaLow SPI clock instead of the upstream
40 MHz default. This is a diagnostic comparison for the observed command
timeouts, not a confirmed fix. Other targets keep the 40 MHz default unless
`CONFIG_MM_SPI_CLOCK_MHZ` is changed. The initial reset settling delay is 20 ms.

Diagnostic v2 fixes minimum RESET_N timing locally in `mmhal_wlan_hard_reset`.
The OS shim's original `mmosal_task_sleep(5)` becomes `vTaskDelay(0)` at the
configured 100 Hz tick rate. The reset path now rounds its delays up and adds
one tick for tick-boundary uncertainty, guaranteeing at least the requested
5 ms reset-low pulse and 20 ms settling delay during normal task execution.
General driver task-sleep behavior is unchanged.

`CONFIG_MM_TRANSPORT_DIAGNOSTICS=y` enables DEBUG logs only in `sdio.c` and
`sdio_spi.c`, without editing the SDK submodule. These logs distinguish startup
CMD63/CMD0 invalid acknowledgements, chip identification failure, and later
transport operations. The original `Transport init failed` message alone
does not identify which operation failed. Logging can affect timing.

`CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y` enables a fuller ESP32-P4 backtrace for
another MMOSAL assertion. The scan callback now distinguishes successful scans
from terminated scans; the old unconditional `Scan finished` message did not
indicate success.

Frame pointers increase the bootloader size. The diagnostic profile reduces
bootloader logging to warnings and errors so it fits at the existing partition
table offset of 0x8000. Application INFO logs remain enabled.

### Diagnostic v3: bounded startup comparison

The reported GPIO10 level changed from 0 to 1 between earlier runs, but
CMD63/CMD0 still returned invalid acknowledgements. The later photos established
that GPIO10 is not IRQ on the direct-stacked setup. These readings neither
confirm a physical wiring change nor establish the module's IRQ state. No valid
chip ID was reached in that log.

`CONFIG_MM_SPI_STARTUP_PROBE=y` enables a diagnostic startup override for
ESP32-P4 with MM6108. It keeps CS high during reset and tries the following
settings, stopping immediately at the first valid CMD63 acknowledgement:

| Stage | SPI clock | Minimum reset settling |
| --- | --- | --- |
| 1 | Configured clock (10 MHz in this profile) | Configured delay (20 ms) |
| 2 | Configured clock (10 MHz) | At least 200 ms |
| 3 | 1 MHz | At least 200 ms |

The 200 ms and 1 MHz values are diagnostic comparisons, not measured board
requirements. Each stage resets the module and uses the SDK's existing
CMD63/CMD0 command implementation and three-attempt limit. Stage 1 also
changes CS during reset compared with v2; success there cannot isolate CS
from the additional reset. The SDK's command validation is unchanged.

Logs include the actual SPI clock, the first 16 received bytes per command
(including ready-poll bytes for CMD0), and SPI API errors. `PASS` means only
that CMD63 was acknowledged; chip ID, firmware loading, and scanning still
require their normal validation. Successful settings stay active for all
subsequent transfers. The diagnostic code does not change RF settings or BCF.

If all three stages fail, capture the log and check at the module side:
RESET_N at J4.3 must go low then high; CS/SCK/MOSI/MISO are J3.1/J3.2/J3.4/J3.3;
verify 3.3 V at J3.5 relative to J3.6 and at the module-side 3V3 rail after
R27. Measure the reset and SPI waveforms with an oscilloscope or logic
analyzer if available. Disconnect power before continuity checks. The
0xff/0xfe bytes alone do not distinguish wiring, power, timing, or a defective
module. This firmware is compiled and checked locally, not hardware-validated.

### Diagnostic v4: compare hardware SPI with GPIO SPI

The supplied v3 run (ELF hash prefix `e8db62bb0`) completed all three hardware
stages. All failed with zero SPI API errors. At both 10 MHz and 1 MHz, after
20/200 ms settling, the first CMD63 polled 201 received bytes without a valid
ack (the first 16 logged bytes were all 0xff); later commands showed `fe 01`
for CMD63 and `ff fe 11` for
CMD0, including its ready-poll byte. The repeated byte pattern justifies a
sampling/framing comparison, but does not prove its cause. Zero API errors
only means the host controller accepted/completed transfers.

With `CONFIG_MM_SPI_GPIO_PROBE=y`, v4 adds a bounded GPIO comparison only after
all three hardware stages fail. It releases the hardware SPI controller and
uses the same SCK/MOSI/MISO/CS pins in slow mode 0, with at least 5 us on each
half-period. It samples MISO while SCK is high and drives MOSI while SCK is
low. This bypasses the SPI peripheral and DMA while retaining the SDK's
CMD63/CMD0 sequence, acknowledgement checks, CMD52/CMD53 handling and CRC.
The module is reset with at least 200 ms settling before this comparison.

After a valid CMD63 ack it reads the MM6108 chip-ID register `0x10054d20`,
using the address-window setup and accepted IDs from the SDK's
`porting_assistant` example. A PASS requires two consecutive matching IDs
from `0x206`, `0x306`, or `0x406`, with successful SDK data CRC checks.

- `GPIO SPI PROBE PASS`: the same physical bus supports slow GPIO-based
  communication. Investigate hardware-SPI sampling, transaction behavior,
  and signal integrity; this does not uniquely establish a peripheral bug.
- `GPIO SPI PROBE PARTIAL`: CMD63 passed, but repeatable ID/CRC validation
  did not. Do not treat this as usable communication.
- `GPIO SPI PROBE FAIL`: GPIO startup failed too. Check module-side power,
  reset, wiring, and waveforms. This alone does not prove a broken module.

**The GPIO comparison always stops before firmware download or RF scanning,
even on PASS.** It deselects CS and holds the module in reset. The subsequent
`mmwlan_boot failed` / initialization-failed messages are intentional on this
diagnostic path; use the preceding GPIO result. No byte shifting, response
acceptance relaxation, or RF settings changes are applied. Successful
communication has not been validated on the user's board.

### Field result: v4 failed on both transports

The supplied v4 log (ELF prefix `e5b8e4df5`, compiled 2026-10-23 18:50:45)
shows all three hardware-SPI stages failing, followed by
`GPIO SPI PROBE FAIL: CMD63 not acknowledged`. The GPIO test reproduces
the same `fe 01` and `ff fe 11` response sequences. Chip-ID/CRC reads were
not reached. The pin incorrectly configured as IRQ (GPIO10) was 0 in this
run, compared with 1 in the preceding v3 log. These are not module IRQ readings
for the photographed direct-stacked connection.

No root cause has yet been established. The result makes a failure confined
to the P4 SPI peripheral or DMA less likely; the GPIO test still shares
wiring, power, reset and the SDK's command protocol. Do not describe the
module as defective or apply response bit-shifting based on these bytes alone.
Further blind clock/delay changes are not supported by this comparison.

Measurement references, if startup still fails with the corrected v5 profile:

| Check | Module-side measurement points | What to verify |
| --- | --- | --- |
| Supply | J3.5 and R27.2, referenced to J3.6 | Approximately 3.3 V on both sides of the input link; check for dips during reset/startup with a scope |
| Reset | J4.3 -> R3 (0 ohm) -> U1 pad 8 | A low pulse followed by high during each attempt; confirm it reaches the module side of R3 |
| CS | J3.1 -> R11 (0 ohm) -> U1 pad 20 | High during training/reset, low during commands |
| SCK | J3.2 -> R8 (22 ohm) -> U1 pad 24 | Clocks reach the module side of R8 |
| MOSI | J3.4 -> R9 (22 ohm) -> U1 pad 23 | Mode-0/MSB-first command bytes: CMD63 `7f 00 00 00 00 ff`, CMD0 `40 00 00 00 00 ff` |
| MISO | J3.3 -> R7 (22 ohm) -> U1 pad 25 | Capture the full response on both module and host sides if possible |
| IRQ | J4.7 -> R6 (0 ohm) -> U1 pad 26 | Check continuity to P4 J11.1/GPIO28; voltage alone is insufficient |

Disconnect all power before resistance/continuity checks; the SPI series
paths include the resistor values above, so a continuity buzzer alone can
be misleading. Use accessible resistor pads and verify board pin numbering.
For waveform capture, trigger on RESET_N release and include CS/SCK/MOSI/MISO
from reset through the first command responses. A meter cannot resolve these
pulses or verify SPI data.

**v4 deliberately holds RESET_N low after the GPIO test finishes.** In the
reported log this occurs near 1.65 seconds after startup. A low voltage
measured after `GPIO comparison complete` is expected and is not evidence
of faulty reset wiring. Capture during the attempts to evaluate reset.
The v4 result alone did not justify another clock/delay change. Subsequent
photos established the IRQ and BCF corrections in v5 described below.

### v5：按照片纠正叠插配置

照片确认是焊接排针直接叠插。此前把 J11/J4 的编号方向视为相同，
导致错误地要求拆线，并把 IRQ 配成 GPIO10；此判断已撤回。PCB 焊盘
坐标表明右侧 J4 编号与 J11 相反，直插时 RESET 正好接 GPIO9，
INT 则接 GPIO28。无需按此前说明拆焊、切断排针或加跳线。

本次只修改有照片和设计文件依据的配置：

- 活动 sdkconfig 与默认配置均设 `CONFIG_MM_SPI_IRQ=28`。
- 模块标签为 `FGH100MABMD`，BCF 改为 `bcf_fgh100mabmd.bin`（US）。
- RESET=9、CS=27、SCK=8、MOSI=6、MISO=7 保持不变。
- 日志版本改为 v5，额外打印 IRQ 和 BCF 文件名以便核对固件。
- 保留 v4 的有限诊断流程，不增加新的频率或延时尝试。

重要限制：当前 CMD63 实现在 MISO 上同步轮询响应，并不等待外部 IRQ；
BCF 也在后续阶段才使用。因此以上两项是必须纠正的板级配置错误，
但不能据此宣称已修复 v4 的 `invalid ack`。需要实际运行确认。
GPIO10 旧日志无助于判断 INT；新日志才会读取 GPIO28。

2026-10-26 的新日志已确认 v5（ELF 前缀 `6823acf13`）仍失败：
IRQ=28、BCF=bcf_fgh100mabmd.bin 已生效，三组硬件 SPI 和 GPIO SPI
仍返回相同的 `fe 01` / `ff fe 11`。这证实 v5 修正没有解决 CMD63
握手故障。日志没有 panic、assert 或看门狗复位信息。

旧目录 `wifi_halow` 也已核对：
`framework/morselib/CMakeLists.txt` 固定链接
`lib/esp32-xtensa-lx7/libmorse_nocrypto.a`，scan 仅附带 ESP32-S3 的
目标专用默认配置。因此这套旧工程不能直接作为当前 ESP32-P4 的对照
固件；不能靠改成 `set-target esp32p4` 就使用该 Xtensa 预编译库。
用户进一步确认：同一模块曾使用旧 `wifi_halow` 在另一块 ESP32-S3
主板上正常工作。具体 S3 板型、当时接法、编译配置，以及现在能否复现
成功尚未确认；不能把这项历史记录当作本次同条件的交叉测试结果。

### v6：复位与片选控制对照（诊断，不是已验证修复）

对照检查包括旧源码和实际旧库 `libmorse_nocrypto.a` 的启动函数。
旧版同样先发训练时钟，CMD63 失败后发 CMD0，最多三轮；命令长度
6 字节，CMD0/63 尾字节 0xff，忽略 0xff 等待字节并要求 ACK=0。
旧、新 HAL 均使用 SPI mode 0。没有找到能解释现象的命令格式差异。
旧 HAL 默认 40 MHz，P4 当前 10 MHz，并已有 1 MHz 和慢速 GPIO
失败记录。旧工程的 WAKE/BUSY 操作与当前禁用省电的配置不同，但
所提供的模块原理图中 R1 为 NC，不能未经验证就启用这些引脚。

旧 `examples/scan/build.7z` 解出到当前 build/vendor_reference 供只读
比较，原文件保持不变。`esptool image_info` 确认其目标 ESP32-S3、
ESP-IDF v5.1.1-dirty、编译时间 2025-12-09 11:52:21；它不是 P4
固件，也尚未确认就是用户当时使用的二进制。

v6 保持已校正的 IRQ=28、RESET=9、BCF 和原有启动过程，仅在
GPIO 启动也失败后追加三组相同的固定长度原始采样：

| 日志标记 | RESET 输出 | 命令期间 CS 输出 | 目的 |
| --- | --- | --- | --- |
| CONTROL BASELINE | 正常复位后拉高 | 拉低 | 建立相同采样方式下的基线 |
| CONTROL RESET_HELD_LOW | 始终保持低 | 拉低 | 比较复位控制是否影响观测数据 |
| CONTROL CS_HELD_HIGH | 正常复位后拉高 | 始终保持高 | 比较片选控制是否影响观测数据 |

每组均发训练时钟，依次发送 CMD63/CMD0/CMD63 的原始字节；记录
每条命令的 6 字节发送内容、发送期间 MISO 的 6 字节和随后 MISO
的 16 字节。CMD0 前另记录一个 0xff 读周期，不做 ready 轮询。
它们不是原有 200 次轮询流程，不能把原始采样结果与 v5 的时序视为
完全等价，也不用于让驱动继续运行。没有更改命令有效性或 CRC 校验。

`P4 pad RESET/CS` 为开启输入缓冲后在主控引脚读到的电平，不是
模块端测量。若读值与输出要求不符，需要检查 GPIO/外部负载；即便
相符，也不能证明排针、0 欧姆电阻及模块端复位电平正常。

判读范围：三种条件若都重复同样异常字节，应优先检查复位、片选是否
真正到达模块及读入信号来源；若正常条件与两个控制条件明显不同，
说明观测数据会随控制变化，但仍不能仅凭此定位故障。全 ff、全 00、
IRQ 高低或某个偶然字节均不构成通信成功。软件采样无法测量 3.3 V
质量或模块端的实际边沿。最终仍可能需要正常 S3 主板对比或测量。

这次不改变焊接排针连接、不生成新的频率/延时遍历。对照结束仍将
CS 拉高、SCK 拉低、模块保持复位，并返回初始化失败。请保留从
`CONTROL PROBE` 到 `CONTROL PROBE complete` 的全部日志。
只有正常 SDK 握手、芯片 ID/CRC 和后续扫描通过，才算完成修复。

本次 v6 已编译通过，`scan.bin` 大小 1,386,160 字节，应用分区
剩余约 10%；镜像校验通过，支持 P4 revision 1.0～1.99。
ELF SHA256 为 `c185ce6a42aaf1798580186e9bec74c2474d421f47f2de6c3a61ed7e75da2af6`。
本次增量构建的应用描述符仍显示 2026-10-23 19:19:29，因此不能
只用 Compile time 判断版本，应同时核对 v6 标记和上述 ELF SHA256。
编译与镜像检查不能替代实际硬件验证；此处未自动烧录。

### v6 实测结果与 v7：MISO 偏置对照

2026-10-26 提交的新日志确认为 v6、ELF 前缀 `c185ce6a4`。
硬件 SPI 三组和 GPIO SPI 仍失败，不能把 v6 称为修复成功。
新增 CONTROL 原始采样给出以下证据：

- BASELINE 的第一条 CMD63 在发送期间读回完全相同的
  `7f 00 00 00 00 ff`，随后全 ff；后续 CMD0/CMD63 在发送期间
  全 ff，发送后分别为 `fe 11` / `fe 01`，其余为 ff。
- CS_HELD_HIGH 的三条命令在发送期间全部逐字节回显 TX，
  发送后均为 ff；P4 的 CS 输入回读为 1。
- RESET_HELD_LOW 前两条命令读回全 00，第三条又出现 TX 回显。
  P4 RESET 输入回读始终为 0，所以不能宣称模块始终处于复位，
  也不能把这个读回值当成模块端复位正常的证明。

已核对 GPIO 采样代码和本地 IDF 的 gpio_get_level 实现：发送用
GPIO6、接收用 GPIO7，接收字节来自 GPIO 输入寄存器，未复制 TX
缓冲区。以上回显仍不能区分高阻输入耦合、连接问题、模块侧行为
或其他电气因素；不构成 MOSI/MISO 短路、模块损坏的定论。

v7 针对这一新现象做有界对照：

1. 增加 `CONFIG_MM_SPI_MISO_PULLUP`，默认关闭，仅在本 P4 配置中
   开启。硬件 SPI 和 GPIO SPI 均启用 MISO 弱上拉，保持原有
   引脚、频率、复位时序及 SDK ACK/CRC 校验。它只是偏置改变，
   不是把 MISO 配置成输出，也不代表已确认需要板上拉电阻。
2. 如果原有启动仍失败，保留 CONTROL 采样，再执行 BIAS 对照。
   CS=1、SCK=0，不发送时钟，只把 MOSI 依次设为 0/1/0/1；
   分别使用 MISO FLOAT/PULLUP/PULLDOWN，记录 20 us 时的值，
   再等待 2 ms 后统计 16 次采样中高电平的次数。弱下拉只用于
   这项失败后的静态诊断，随后恢复配置中的偏置。
3. 输出 GPIO6/7 配置，便于核对 GPIO7 是否输入使能、输出关闭。
   这些仍是主控引脚观测，不能替代模块端电压和连通性测量。

判读：MISO 随弱上/下拉改变而不再随 MOSI 改变，支持原来存在
高阻/偏置敏感现象；若静态 MISO 在两种偏置下都随 MOSI 改变，
应重点核实信号路径及实物连接，但软件不能据此确认短路位置。
即便上拉后回显消失，只要 CMD63 仍失败，也没有完成通信修复。

正常硬件启动成功才会继续原有芯片识别、固件加载和扫描。
GPIO 诊断路径仍然停止扫描，最后只保证 P4 RESET 输出为低，
并未测量模块端是否真的保持复位。此次不自动烧录、不改变焊接
连接；请保留 v7 从启动到 CONTROL/BIAS 结束或扫描结束的日志。
2026-10-26 已编译通过（描述符时间 09:34:51），`scan.bin` 大小
1,390,288 字节，应用分区剩余 9%。`esptool image_info` 确认
checksum/validation hash 均有效，目标 P4 revision 1.0～1.99、
Flash 16 MB。ELF SHA256 为
`7019d3f82063cf50b1367203937bd875530b04b1245a8f1ae2ab29899f4c1151`。
编译日志为 `build/codex-miso-bias-build.log`。尚未自动烧录或硬件
验证，不能以编译通过作为修复成功的证据。

### v7 实测：上拉消除回显，握手仍失败

用户提交的 `af8b0cc5-82b2-4f57-8acd-74b6b05173a9/Pasted text.txt`
确认为 v7、ELF 前缀 `7019d3f82`、编译时间 2026-10-26 09:34:51。
上拉开关已生效；三组硬件 SPI 和慢速 GPIO SPI 仍返回同样的
`fe 01` / `ff fe 11`，没有进入芯片 ID 或固件加载阶段。

- CONTROL 的 CS_HELD_HIGH 和 RESET_HELD_LOW 全部读回 ff，
  v6 的 TX 回显及复位低电平下的全 00/回显变化均未再出现。
- BASELINE 第一条 CMD63 在发送及接收期间全 ff；后续 CMD0、
  CMD63 的接收依然分别是 `fe 11`、`fe 01`。
- GPIO7 转储为 InputEn=1、OutputEn=0、Pullup=1，确为输入。
- BIAS FLOAT/PULLUP 在 MOSI=0/1 时均为 16/16 高；PULLDOWN
  在 MOSI=0/1 时均为 0/16 高，MOSI 主控端读回符合设定值。

这些结果支持：在被测试的未选中静态状态下，MISO 可由弱偏置
改变，并非始终随 MOSI 改变；v6 回显不是永久低阻短接的充分
证据。不能用软件读数排除所有焊接/供电/动态时序问题。上拉
虽然改变了空闲采样，却未修复 CMD63，不能把悬空回显当作
本次握手失败的完整原因。尚未建立正常扫描或可靠芯片 ID。
日志无 panic、assert 或看门狗复位，最后为初始化失败返回。

### v8：经芯片 ID/CRC 验证的一位 RX 对齐回退

本次直接读取 Morse Micro 官方 Linux 驱动 `morse_driver/main/spi.c`，
对照副本存为 `build/vendor_reference/morse_driver_spi.c`，SHA256 为
`733d6ae7c66be94f541fc8b57bd1735bae626f69a4baa2a669b72a33dd4c1ccf`。
获取地址（2026-10-26，main 快照，未替换当前 SDK）：
`https://raw.githubusercontent.com/MorseMicro/morse_driver/main/spi.c`。
其中 `morse_spi_find_response` 对首个非 ff 的 fe 报告一位错位，
`morse_spi_xfer` 仅对 RK3288 使用右移一位的适配。这是检验
错位假设的依据，不证明 P4 有同一缺陷，也不是官方 P4 修复。

v7 已排除“启用弱上拉即可恢复握手”，当前 fe 01 / fe 11 在
多种时序下仍重复。v8 保留未校正的三组硬件 SPI 启动；失败后
才启用 `CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL`（默认关闭，P4 配置
开启）。实验在 1 MHz 下进行流式 RX 右移一位，每字节最高位
来自上一原始字节的最低位，CS 拉高时重置。进位跨 TX-only、
单字节和缓冲区传输保存；发送字节和时钟数量保持不变，SDK
ACK=0、数据 token 和 CRC 检查不变，不直接把 fe 当作成功。
收发使用对齐暂存区，避免修改调用者的发送数据或短 DMA 缓冲。

启用前运行 C 算法自检；之后须满足全部条件：

1. 原 SDK CMD63 成功且 SPI API 无错误。
2. 使用原 SDK CMD52/53 连续两次读到相同有效 MM6108 ID
   （206/306/406），每次都通过 SDK 数据 CRC。
3. 再硬复位一次并完整重做，ID 必须与前一轮相同。

全部通过才打印 `ALIGN VALIDATED` 并保留校正进入 SDK 的正常
芯片识别、固件加载及扫描；这条日志本身仍不代表扫描已成功。
验证失败则打印 `ALIGN REJECTED`、关闭校正，继续原有 GPIO、
CONTROL、BIAS 诊断。关闭配置即可回到 v7 的未校正路径。
此适配当前未经实机验证，不作为量产吞吐或可靠性结论。

验证记录：编译通过、镜像 checksum/hash 有效；独立位流参考
测试覆盖 65,536 个进位/字节组合、88 个分段流、3 个 ID/CRC
帧，以及损坏 CRC、丢失跨调用进位的反例。参考测试未执行
P4 二进制；实际 C 自检和通信验证需要用户在板上运行。
本机 ESP clang 不含 Windows x86 后端，因此未宣称主机 C
执行测试通过。目标 ESP-IDF 编译无本次修改相关的警告/错误。

v8 镜像：`build/scan.bin`，1,392,288 字节，分区剩余 9%，
P4 revision 1.0～1.99、Flash 16 MB；描述符时间
2026-10-26 09:46:53，ELF SHA256：
`3167c80f9ed0b0099c7567453a6863d7eccc0ec93a69af353bccad0da035f10e`。
编译日志 `build/codex-rx-alignment-build.log`。未访问串口或自动
烧录。请保留从 `ALIGN TRIAL` 到扫描结束或最终失败的日志。

After rebuilding and flashing, confirm these boot messages:

```text
scan: HaLow SPI diagnostic v8; configured clock=10 MHz, RESET_N=9, settle=20 ms
scan: HaLow board profile: stacked headers, IRQ=28, BCF=bcf_fgh100mabmd.bin
MMHaLow: MISO weak pull-up enabled: comparing with v6; ACK/CRC checks unchanged
MMHaLow: HaLow SPI requested 10 MHz, actual 10000 kHz; SCK=8 MOSI=6 MISO=7 CS=27 IRQ=28
```

The actual clock is reported by the SPI driver and can differ on other hardware.
The bootloader's `SPI Speed : 80MHz` refers to flash and is independent of the
HaLow module's SPI clock. If errors persist, retain the full log including the
new backtrace; command timeouts alone do not establish whether the cause is
SPI timing, IRQ wiring, module supply, or firmware handling.

## v9：重复扫描对照

2026-10-26，用户提供的最新 v8 日志（ELF 前缀 `3167c80f9`）完成了扫描，
但没有收到 AP 响应；同一固件此前曾发现 ThinkNode-G4_2a2d82。
因此当前未发现网关的现象也出现在独立 scan 固件中，不能仅归因于摄像头工程。
当前 camera v4 的实物日志已覆盖约 8 分钟、26 轮扫描，响应均为 0；
增加等待时间未恢复联网，错误计数为 0 也不足以确认射频或 SPI 正常。

v9 只调整本例程的扫描流程，继续使用原来的 GPIO、BCF、国家码和经验证后
启用的 1 MHz RX 对齐路径。自动顺序执行 5 轮通配扫描：第一轮为 SDK 默认
30 ms/信道，后四轮为 150 ms/信道，轮次之间等待 2 秒。每轮打印收到的
响应数及目标 SSID 的响应数；响应可以重复，不等于不同 AP 的数量。
AP 详情新增接收频率和接收带宽，以区分 8 MHz 工作中心频率与主信道。
保留完整国家信道表，不将扫描固定到 924 MHz。

每轮完成后打印累计接收读取/内存失败、队列丢弃、发送状态超时与模块重启
计数。单轮超过 60 秒则请求中止并停止测试；扫描不完整或请求失败也停止，
不会重叠发起下一轮扫描。最后输出目标在几轮中被观察到；即使所有信道
扫描完成，只要响应数为 0，仍明确提示没有收到 AP 响应。本例程不认证、
不申请 DHCP，不修改网关，也不自动复位模块重试。

启动标识为 `Scan comparison v9`，HAL 仍保留 `HaLow SPI diagnostic v8`
日志以标明底层诊断路径未变。原 v8 的 bin、elf、sdkconfig 及 app_main.c
已备份到 `build/reference_v8/`；原 bin 的 SHA256 为
`e304a367b2abb458c274d83f9e2c7ca710958a5416767bf2ef138e7bb57e194b`。
该备份在 build 下，执行 fullclean 前如需保留应先另存。

建议先在网关 US / 8 MHz / 信道 44 设置不变时完成一次冷启动对照。
若 5 轮均为 0，再单独将网关切换为 US 下可选的 2 MHz 信道进行比较，
保留 SSID 和安全配置，并记录实际信道；切换带宽后信道 44 未必仍可选。
这个测试用于缩小问题范围，不表示已经确认 8 MHz 不兼容。

v9 已通过 ESP-IDF v5.5.3 编译、链接、分区大小检查及镜像 checksum/hash
校验；未自动烧录或验证实物扫描结果。`build/scan.bin` 为 1,396,208 字节，
应用分区剩余 9%，支持 P4 revision 1.0～1.99；ELF SHA256 前缀为
`fbfa61d61`，固件文件 SHA256 为
`d4936566bcb27d14862d1340b594ccc6e2e656ed7ba6f2e75cdffd4fd7b98457`。
增量构建保留了旧的 Compile time，确认版本应使用 v9 标识和 ELF 哈希。

## Flash

With the board connected to the CH340K USB serial interface, use the ESP-IDF
terminal in this example directory:

```powershell
idf.py -p COM9 -b 460800 flash
```

COM9 is the port used by the connected board in the original flash log. Select
the current CH340K port if Windows assigns a different number after reconnecting.
The rebuilt bootloader and application have both been checked with
`esptool image_info --version 2`: each must specify minimum revision v1.0 and
maximum revision v1.99, covering the board's v1.3 silicon.

The `Total image size` output only reports build size; it does not confirm that
flashing succeeded. Check the flash command's final result. If it reports that
the serial port does not exist, restore the USB serial connection before
retrying. Do not use `--force` to bypass a chip revision mismatch.
