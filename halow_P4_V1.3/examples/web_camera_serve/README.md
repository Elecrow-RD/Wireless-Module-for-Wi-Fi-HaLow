# ESP32-P4 + SC2336 MIPI-CSI + Wi-Fi HaLow Web Camera

此目录适配 Elecrow ESP32-P4 V1.3 主板、SC2336 MIPI-CSI 摄像头和 FGH100MABMD / MM6108 HaLow 模块。芯片日志报告 **ESP32-P4 rev1.3**，P4 配置按 ESP-IDF v5.5.3 的 v1.x revision 系列构建。使用 ESP-IDF **v5.5.3**，Flash **16 MB**，启用板载 PSRAM。

## 历史记录：P4 rev3.2 摄像头 ISP 启动修复

最新附件显示已连接 `ThinkNode-G4_2a0ea0` 并获得 `192.168.12.193`，SC2336 识别 PID `0xcb3a`。v5 中 AWB 子窗口错误已消失，但 WBG 增益写入失败，`VIDIOC_STREAMON` 返回 errno=16。HTTP 启动不代表摄像头已经出帧；该日志没有 `First JPEG ready`。

根因是固定版本 esp_video 1.4.1 的 `isp_start_awb()` 和 `isp_reconfigure_awb()` 使用未清零的 `esp_isp_awb_config_t`。本机 ESP-IDF 5.5.3 的驱动在 P4 rev >= 3.0 会检查 `subwindow`，旧组件没有填写该字段，栈中的不确定值因此触发子窗口范围校验失败。

上一轮 v5 已把 AWB 配置清零，排除了未初始化子窗口错误。最新日志确认 v5 已越过该处，但在 `esp_isp_wbg_set_wb_gain()` 遇到 `failed to update wbg shadow register`，`VIDIOC_STREAMON` 返回 errno=16。IDF 的 WBG 接口支持 `update_once_configured` 强制更新；esp_video 1.4.1 默认关闭该选项、等待 VSYNC，而启动管线时 CSI 还没有首帧，因此更新请求失败。

兼容补丁现同时将首次 WBG 配置设为 `update_once_configured=1`，使 RGB 增益能在第一帧之前写入 ISP。AWB 主窗口继续正常工作，WBG 后续改写使用相同的强制更新策略。补丁只作用于 build 目录生成的 ISP 源码副本，不修改托管依赖；清理 build 或重新下载组件后会重新生成。升级 esp_video 时需重新核对补丁，源码结构不匹配会在配置阶段明确报错。

以上 rev3.x ISP 记录仅为历史。当前目标芯片为 ESP32-P4 rev1.3，P4 profile 按 v1.x revision 构建。当前固件启动标识为 v6，摄像头标识为 v9。网页界面采用英文；默认预览配置为 320x176、JPEG quality 45、上限 5 fps。采集成功时应看到以下日志：

~~~text
Elecrow P4 camera v6: P4 rev1.x; independent HaLow discovery
Camera compatibility v9: esp_video-managed SCCB I2C1; P4 rev1.x; preview max=5 fps, width=320, JPEG quality=45
CSI STREAMON succeeded; waiting for first JPEG
First JPEG ready: 640x352, ... bytes
~~~

本次修改尚未在硬件上验证。请在本目录使用 ESP-IDF v5.5.3 终端执行 `idf.py fullclean` 后运行 `idf.py build flash monitor`。启动时确认日志带有 v6 和 v9 标识，并出现 `SC2336 MIPI: ... esp_video-managed bus`；采集成功后应出现 `First JPEG ready`。

## 已实现

- SC2336 双通道 MIPI-CSI，24 MHz 板载摄像头时钟。
- esp_video + ISP，自动曝光/增益、白平衡，P4 硬件 JPEG 编码。
- 通过当前本地 morsemicro/halow 组件联网，沿用 scan 已在实物验证的 v8 RX alignment 修复。
- HTTP 80：网页、状态、下载照片、保存照片到 SD 卡；HTTP 81：MJPEG 预览。
- 默认 SC2336 RAW8 1280×720 采集，缩小到 **320×176** 后编码，JPEG 质量 45，最多 **5 帧/秒**。高度向下对齐 16 像素，有轻微比例变化。
- 摄像头任务持续收帧，只保留最新 JPEG；发送网络数据时不会占用 CSI DMA 帧。支持一个预览客户端，抓拍可同时使用。
- 无摄像头/无图像时返回 503，页面显示摄像头状态；无 SD 卡不阻止网页预览和下载。不自动格式化 SD 卡。

当前 HaLow 在 1 MHz SPI RX 对齐回退下成功扫描。这里的帧率是上限，不是网络吞吐保证。若改进配置仍卡顿，可在菜单中将帧率降至 3；若需要更清晰画面，可提高 JPEG 最大宽度，但会增加链路负载。不要为提速关闭已验证的 SPI 对齐修复。

## 板级引脚

依据工作区 SCH&PCB/ESP32-P4 Display 10.1 inch V1.2.sch 的 **U7 实际 GPIO 引脚**，不是遗留网络名。用户确认 9 寸和 10 寸板的摄像头原理图一致；摄像头初始化时序现与已验证可出图的 Lesson13-Camera_Real-Time 例程保持一致。

| 功能 | GPIO / 连接 |
| --- | --- |
| 摄像头 SCCB SDA | GPIO12 → R90 → Q6 → FPC1.5 |
| 摄像头 SCCB SCL | GPIO13 → R89 → Q7 → FPC1.4 |
| 摄像头 RESET | GPIO11 → R88 → Q4 → FPC1.8；软件按 Lesson13 设置为不主动驱动（`reset_pin=-1`） |
| 摄像头 XCLK | Y3 板载 24 MHz → FPC1.11，不使用 LEDC 输出 |
| 摄像头 PWDN | -1，不占用 GPIO |
| CSI D0 / CLK / D1 | P4 专用 MIPI 引脚，经 R152~R157 到 FPC1 |
| HaLow SCK / MOSI / MISO | 8 / 6 / 7 |
| HaLow CS / RESET / IRQ | 27 / 9 / 28 |
| SD 卡 CLK / CMD / D0 | 43 / 44 / 39，1-bit SDMMC，J5 VDD 直连板上 3.3 V |
| 串口控制台 | GPIO38 / GPIO37，COM9（当前连接） |

注意：摄像头复位网络名 IO46_CSI_RESET 最终接的是 U7.GPIO11；SCL 网络名 IO1 最终接的是 U7.GPIO13。不要按网络名改为 GPIO46/GPIO1。相机启动前会按 Lesson13 打开 LDO3=2.5 V、LDO4=3.3 V 并使用 SCCB I2C1；esp_video 随后会对 LDO3 做同压引用。J5 SD 电源不经 P4 LDO。

## 配置和运行

在 VS Code **打开本目录**，选择 ESP-IDF v5.5.3，目标 esp32p4。也可在 ESP-IDF PowerShell 中执行：

~~~powershell
cd E:\ElecrowWork\WiFI_Halow\esp-halow-main\halow\examples\web_camera_serve
idf.py menuconfig
~~~

进入 **Elecrow P4 HaLow camera**：

1. HaLow SSID、密码和 WPA3-SAE 已写在 main/p4_main.c 的 HALOW_WIFI_SSID 与 HALOW_WIFI_PASSWORD 常量中。
2. SD 保存默认启用；没有 SD 卡可关闭 Enable SD card photo saving。J5 的 SD 电源来自板上 3.3 V，不由 P4 LDO4 供电。
3. 宽度、JPEG 质量、帧率均可在此调整。

然后：

~~~powershell
idf.py build
idf.py -p COM9 flash monitor
~~~

先退出其他占用 COM9 的串口监视器。首次从 scan 切换过来应使用完整的 idf.py flash，更新 bootloader、分区表及应用，不要只刷 app。本版保留当前 scan 的 NVS/phy_init 地址及大小，应用放在 0x10000；使用单 factory 应用分区。若设备已使用其他分区布局且有重要文件数据，请先备份。

正常启动时应能看到以下关键日志（扫描结果可能重复）：

~~~text
Elecrow P4 camera v6: P4 rev1.x; independent HaLow discovery
Startup 1/3: connecting HaLow; camera and SD initialization deferred until IP
Discovery: wildcard US scan, dwell=30 ms/channel; STA/camera/SD not started
HaLow AP: ThinkNode-G4_2a2d82; RSSI=... dBm, freq=... kHz, RX BW=... MHz, operating BW=8 MHz, security=SAE, target=yes
Discovery complete: state=0 (all channels scanned), responses=..., target responses=...
HaLow counters (cumulative): scans=..., restarts=..., RX read failures=..., RX alloc failures=..., RX queue drops=..., RX mgmt queue peak=..., TX status timeouts=...
STA scan dwell=150 ms/channel; retry interval capped at 16 s
HaLow IP: ...; browser: http://.../
Startup 2/3: HaLow obtained IP; starting camera
SC2336 MIPI: SCCB I2C1 SDA=12 SCL=13, esp_video-managed bus, board RESET control, LDO3=2.5 V LDO4=3.3 V, XCLK=24 MHz
Sensor format: 1280x720, fourcc=..., returned type=...
First JPEG ready: 640x352, ... bytes
HaLow after camera startup: STA state=2, IP event=present
Startup 3/3: mounting SD
HaLow after SD startup: STA state=2, IP event=present
HTTP ready: port 80 page/status/snapshot, port 81 MJPEG
~~~

电脑/手机应能访问 HaLow AP 分配给 P4 的地址。打开日志打印的地址，点击“开始预览”。“下载照片”保存到电脑/手机；“保存到 SD 卡”写入卡内 /photos/P0000001.JPG 等文件，不覆盖旧照片。

### 排错

- 如需更换 HaLow 网络，在 main/p4_main.c 修改 HALOW_WIFI_SSID 和 HALOW_WIFI_PASSWORD 后重新编译。
- v4 保留先获取 IP、再启动相机和 SD 的顺序。用户确认网关为 US、44 信道（924 MHz 中心频率）、8 MHz，当前 US 信道表包含该信道。8 MHz 中心频率不等于主信道扫描频率，因此保留完整 US 信道表，不将扫描锁在 924 MHz，也不强制所有扫描使用 8 MHz。
- v4 在 STA 连接之前执行独立通配扫描，先使用与 scan 示例一致的默认 30 ms/信道；若未发现目标，再执行 150 ms/信道扫描。两轮顺序执行，最终回调结束后才进入下一步；请求失败、扫描中断或超时则停止启动并保留错误信息。之后 STA 扫描也使用 150 ms，重试等待上限从默认 512 秒改为 16 秒。加长等待是待实物验证的扫描参数调整，尚不能证明根因或保证恢复连接。
- Discovery complete 的 target responses=0 表示独立扫描也未观察到目标。若独立扫描能找到而 STA 找不到，应对照两类扫描结果；如果两轮都为 0，继续查看 HaLow counters。计数累计不清零：RX read failures 是驱动读取失败（含格式错误），RX alloc failures / RX queue drops 是接收内存或队列丢弃，TX status timeouts 是发送状态超时。计数为 0 不能证明射频或 SPI 无故障。请保留完整启动至至少两轮 STA 扫描的日志。
- 如果 Startup 1/3 阶段仍连续 responses=0，说明本次相机/SD 初始化尚未执行，不应继续修改相机格式或将问题归为 DHCP。如果取得 IP 后、相机或 SD 启动后掉线，保留对应的 HaLow after ...、关联状态和 IP lost 日志，以定位两者共存时的问题。
- No JPEG received within 5 seconds：SC2336 可能已在 I2C 上识别，但 CSI 尚无有效帧。检查 FPC 接口、排线方向和摄像头电源。程序仍保留网络诊断能力。
- `Get sensor ID failed` / `failed to detect MIPI-CSI camera sensor`：SCCB 没有读到 SC2336，不是 HaLow 或浏览器问题。当前代码沿用正常工程的 esp_video SCCB 初始化；若仍出现，检查摄像头 FPC 是否插牢、触点方向、摄像头板供电和 RESET 网络，再确认 GPIO12/13 的 SDA/SCL 设置。
- Camera setup failed at ...：按日志中的步骤定位。此前已识别到 SC2336 PID=0xcb3a，随后 errno=22 的原因是 esp_video 1.4.1 初始 G_FMT 返回 type=0，旧代码直接复用该结构请求 S_FMT；本版按 Lesson13 重新构造 type=VIDEO_CAPTURE 的 RGB565 请求。仍失败时请保留 Sensor format 和具体失败步骤。
- HaLow AP / HaLow scan complete：打印扫描收到的 AP 名称、信号、频率及安全模式，每轮最多显示前 8 个非目标结果，目标始终显示。target responses=0 表示本轮未观察到配置的 SSID，不能据此认定密码或 DHCP 有误；target=yes 但未认证时，再检查 AP 安全配置及后续认证事件。计数是收到的响应数，不是不同 AP 的数量。
- All hardware SPI startup probes failed：这是共享 HAL 的原始 SPI 探测失败，后续仍会尝试 v8 对齐；必须继续看 ALIGN VALIDATED 和实际连接结果。
- STA is not associated：模块尚未完成 AP 认证/关联，先确认 SSID、密码、WPA3-SAE、AP 电源和信道；此时不是 DHCP 问题。STA is associated, but DHCP has not assigned an IP：链路已建立，再检查 AP DHCP 服务。
- SD unavailable：不影响预览/下载；确认 FAT 格式卡和板载 J5 接口，不会自动格式化。

## 代码和依赖

- main/p4_main.c：启动、HaLow STA、DHCP 等待。密码不输出日志。
- main/p4_camera.c：CSI/ISP 采集、缩放、JPEG 最新帧缓存。
- main/p4_web.c：网页、HTTP API 和 MJPEG。
- main/p4_storage.c：P4 1-bit SDMMC 及照片保存。
- main/Kconfig.projbuild：板级和网络配置。
- sdkconfig.defaults.esp32p4：与工作 scan 一致的 P4/HaLow 默认配置。
- esp_video 固定 1.4.1；dependencies.lock 锁定本次验证的所有依赖。

旧 main/src 下的 S3 DVP、BLE、UART 配网代码保留供参考，**不再参与编译**。本版使用 源码常量配置网络；没有迁移原有 BLE/AT 配网协议。原说明保存在 README.legacy-s3.md。LCD 显示不在此网页摄像头例程范围内。

HaLow 凭据直接写在 main/p4_main.c 的 HALOW_WIFI_SSID / HALOW_WIFI_PASSWORD 常量中；密码不会输出到日志。分享源码前请注意，源码中会包含该密码。

## 验证范围

2026-10-26：v2 实物日志已确认 SC2336 PID=0xcb3a，格式修复后出现 First JPEG ready: 640x352, 5487 bytes，相机采集和 JPEG 编码成功；浏览器画面尚未验证。HaLow 正常启动，但多轮扫描 responses=0，尚未进入认证，也未取得 IP。此前 a5e3f2775 固件的日志既有长时间未联网的启动，也有重连后取得 192.168.12.193 并启动 HTTP 的记录；当时相机探测失败，尚不能仅凭修改前后关系确认网络故障根因。

此前 v4 增加连接前的独立扫描、BSSID/接收和工作带宽日志、接收失败和队列计数，并调整 STA 扫描等待及重试间隔。当前 SCCB 上拉修复尚未通过本机 ESP-IDF 编译或实机验证；网页吞吐与 SD 保存仍待验证。

本次 build/web_camera_serve.bin 为 1,797,504 字节，镜像 checksum 和内嵌校验哈希有效；ELF SHA256 前缀为 e5f1130f3，固件文件 SHA256 为 596c7b124aff2e049c339fb9b233d1fbe1471c9bced30c895e6025a4bbdcdf53。增量构建可能保留旧的 Compile time，请通过 v4 启动标识与 ELF 哈希确认本次固件。



