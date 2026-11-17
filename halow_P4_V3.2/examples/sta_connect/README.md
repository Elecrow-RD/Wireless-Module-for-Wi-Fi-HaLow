# sta_connect：Elecrow ESP32-P4 + FGH100MABMD

已按当前 ESP32-P4 v1.3 主板与排针直插的 FGH100MABMD（MM6108）模块配置，使用 ESP-IDF v5.5.3、16 MB Flash、美国 US 信道表和模块专用 `bcf_fgh100mabmd.bin`。

## 引脚与传输配置

| 信号 | P4 GPIO |
| --- | --- |
| RESET_N | 9 |
| SPI SCK | 8 |
| SPI MOSI | 6 |
| SPI MISO | 7，启用内部上拉 |
| SPI CS | 27 |
| IRQ | 28 |
| WAKE / BUSY | 未连接（-1） |

`sdkconfig.defaults.esp32p4` 沿用 scan 示例的配置，包括 SPI 启动探测和一位接收对齐校验。配置请求 10 MHz；启动探测可能选择 1 MHz。判断模块是否启动成功，应查看后续芯片 ID、固件版本和网络日志。硬件 SPI 探测失败后若接收对齐验证及模块启动成功，前面的探测失败日志不代表整个初始化失败。

## AP 配置

默认参数已编入固件，无需运行时输入：

- SSID：`ThinkNode-G4_2a2d82`
- 密码：`elecrow.com`
- 安全方式：WPA3-SAE
- 国家码：`US`

可在 `idf.py menuconfig` 的 `HaLow sta_connect Example Configuration` 修改 SSID 和密码。默认值在 `main/Kconfig.projbuild`；已有 `sdkconfig` 时以该文件为准。日志不打印密码。

## 编译和烧录

在 ESP-IDF v5.5.3 终端中进入本示例目录：

```powershell
cd E:\ElecrowWork\WiFI_Halow\esp-halow-main\halow\examples\sta_connect
idf.py build
idf.py -p COM9 flash monitor
```

工程默认目标为 ESP32-P4，首次配置会自动加载 `sdkconfig.defaults.esp32p4`。若将旧工程的配置复制进来，请先保留自定义设置，再运行 `idf.py set-target esp32p4` 重新生成配置。本示例不初始化摄像头和 SD 卡。

## 运行结果

串口启动标识为 `Elecrow P4 sta_connect v1`，随后依次打印扫描结果、SAE 认证、关联和 `HaLow IP: ...; gateway: ...`。

程序创建好事件资源后才启动异步连接，并等待实际获得 DHCP 地址后，向 `192.168.12.1:12345` 发送一次 `Hello from esp32\n` UDP 数据。目标地址和端口可在 `main/app_main.c` 修改。该例程不提供网页服务；浏览器访问板子 IP 不会显示页面。`UDP sent` 只说明本机发送调用成功，确认接收需要目标端监听 UDP 12345。

每等待 30 秒会报告当前连接阶段，驱动继续尝试连接：

- `target responses=0`：本轮未观察到目标 AP，还没有进入认证或 DHCP。
- 已看到目标 AP，但一直未关联：查看认证、关联日志与 AP 的安全配置。
- `Associated, still waiting for DHCP`：已关联，但尚未取得地址，检查 AP DHCP 服务。
- `HaLow initialization failed`：模块启动失败，应先检查之前的传输日志。

构建通过仅验证编译、链接及镜像配置；实际扫描、获取 IP 和 UDP 接收仍需在当前板子上运行确认。
