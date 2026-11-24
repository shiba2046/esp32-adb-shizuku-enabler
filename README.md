# ESP32-S3-Zero Shizuku Universal Auto-Activator

基于 **ESP32-S3-Zero** 与 `EspUsbHost` 的离线硬件 **Shizuku** 自动激活器。无需电脑、无需 Wi-Fi 环境，插上 Android 手机即可通过原生 USB Host 发起 ADB 握手并自动启动 Shizuku。

## ✨ 核心特性

* **永久通用路径（Universal）：** 通过 `pm path` 动态定位当前安装的 `libshizuku.so`（适配 Shizuku v13.5+），手机端升级 Shizuku 或更换设备均无需重新烧录固件。
* **RSA-2048 密钥持久化：** 首次运行自动生成 ADB 认证密钥并保存在芯片 NVS 闪存中；手机端勾选“一律允许”后，后续重启手机可实现免弹窗盲操激活。
* **6 阶段 RGB 状态灯反馈：** 针对 ESP32-S3-Zero 板载 WS2812（GPIO 21，已适配 GRB 通道顺序）提供直观的进度指示。
* **内置 Wi-Fi 纯文本日志监控：** 解决单口板子做 USB Host 时无法查看串口日志的痛点，自带热点与极简纯文本日志页。
* **无超时常驻待命：** 取消单次运行超时锁死，拔掉手机自动复位回蓝灯待命；支持短按 `B` 键软重启。

---

## 🛠️ 硬件准备与接线拓扑（关键）

### 所需硬件
* **主控板：** ESP32-S3-Zero（需支持原生 USB OTG / Host）
* **供电与转接：** 带 PD 独立供电口的普通 Type-C 扩展坞（USB Hub）+ 外部电源（充电宝/充电器）+ 支持数据传输的 USB-A 转 Type-C 数据线

### ⚠️ 为什么不能用双头 C 线直接连手机？（USB 角色死锁）
ADB 激活要求 **ESP32 做 USB Host（主机）**，**Android 手机做 USB Device（从机）**。
在 OPPO（ColorOS）等机型上，如果用双头 C 线或手机端 OTG 线直连，手机会抢占 Host 角色对外供电，导致两台设备形成 **Host 对 Host 死锁**，无法触发 USB 枚举。

### ✅ 正确接线方式（反向拓扑）
必须让 **ESP32 接上游主机口**，**手机接下游设备口**，并由外部电源供电：

```text
[ 充电宝 / 电源 ] ──▶ 扩展坞 PD 供电口
[ ESP32-S3-Zero ] ──▶ 扩展坞 Type-C 公头（上游 Host 端）
[ Android 手机   ] ◀── USB-A 转 C 数据线 ── 扩展坞 USB-A 母口（下游 Device 端）
```

---

## 💡 指示灯与按键说明

### 板载 RGB LED（GPIO 21）
| 灯光颜色 | 当前阶段 | 说明 |
| :--- | :--- | :--- |
| 🔵 **蓝色** | 待命 | 板子已就绪，等待手机接入 |
| 🩵 **青色** | 已连接 | 已枚举到手机 ADB 接口，正在握手 |
| 🟡 **黄色** | 等待授权 | 首次连接，请在已解锁的手机屏幕上点击“允许 USB 调试” |
| 🟣 **紫色** | 执行中 | ADB 认证通过，正在运行 `libshizuku.so` |
| 🟢 **绿色** | **激活成功** | Shizuku 已启动，可直接拔掉设备 |
| 🔴 **红色** | 报错 | 握手或命令执行失败，请连入 Wi-Fi 查看具体日志 |

*(注：若你的板子批次为标准 RGB 而非 GRB，请将代码中 `setLedColor` 内的 `neopixelWrite(RGB_LED_PIN, g, r, b)` 改为 `(RGB_LED_PIN, r, g, b)`。)*

### 按键功能
* **`B`（BOOT / GPIO 0）：**
  * **通电前按住插电脑：** 进入固件下载（Bootloader）模式。
  * **运行中短按：** 白灯闪烁一下，清空日志并软重启板子。
* **`R`（RST）：** 硬件复位重启。

---

## 🚀 编译与烧录

### 方式一：使用 Arduino IDE（macOS / Windows）
1. 在 `Settings -> Additional boards manager URLs` 添加乐鑫官方源：
   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. 在 **Boards Manager** 中安装 `esp32 by Espressif Systems`（3.0.x 或最新版）。
3. 在 **Library Manager** 中安装 **`EspUsbHost`**（作者 `tanakamasayuki`）。
4. **按住板子上的 `B`（BOOT）键不放**，将数据线插入电脑，1 秒后松开进入下载模式。
5. 在 `Tools` 菜单核对以下参数后点击 **Upload**：
   * **Board:** `ESP32S3 Dev Module`
   * **USB CDC On Boot:** `Disabled`
   * **USB Mode:** `USB-OTG (TinyUSB)`
   * **Flash Size:** `4MB (32Mb)`
6. 烧录完成后按一下 `RST` 键，亮起蓝灯即可使用。

### 方式二：使用 `arduino-cli`（Windows 11 无界面轻量烧录）
```powershell
# 1. 安装 CLI 与依赖（仅首次）
winget install ArduinoSA.CLI
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install EspUsbHost

# 2. 按住 BOOT 键插上板子，查看 COM 端口号
arduino-cli board list

# 3. 进入项目目录编译并上传（将 COM3 替换为实际端口）
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=default,USBMode=default,FlashSize=4M .
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32s3:CDCOnBoot=default,USBMode=default,FlashSize=4M .
```

---

## 📱 手机端前置设置与使用

1. **开启开发者选项：** 在手机设置中开启 **USB 调试**。
2. **OPPO / 一加 / 真我（ColorOS）必做项：**
   * 在开发者选项最底部开启 **“禁止权限监控”**（Disable Permission Monitoring），否则 Shizuku 虽然能启动，但授权应用无法正常执行底层操作。
   * 建议同时开启 **“仅充电模式下允许 ADB 调试”**。
3. **首次激活：** 按上述“反向拓扑”接好电源与手机，板子亮**黄灯**时在手机上勾选 **“一律允许使用这台计算机进行调试”** 并确定，随后转为**绿灯**即完成激活。
4. **日常重启后：** 直接按拓扑接上手机，等待 1~2 秒看到**绿灯**即可拔掉。

---

## 🔍 Wi-Fi 实时日志调试

板子通电后会自动发射一个调试热点，用于排查 USB 枚举或 Shell 执行状态：
* **Wi-Fi 名称：** `ESP32-ADB-LOG`
* **密码：** `12345678`
* **日志地址：** 浏览器访问 `http://192.168.4.1`（纯文本输出，刷新页面即可获取最新日志）

正常激活日志示例：
```text
[0.4s] ADB key: loaded from NVS
[0.5s] Shizuku ESP32-S3-Zero Ready (No timeout).
[3.2s] USB Connected: addr=1 vid=0x22d9 pid=0x2769
[3.2s] ADB interface matched (itf=0)
[3.2s] ADB send: CNXN
[3.4s] ADB send: AUTH SIGNATURE
[3.5s] ADB connected! Running Shizuku...
[3.5s] ADB send: OPEN shell...
[3.6s] Shell output:
info: starter begin
info: starting server...
info: shizuku_starter exit with 0
SHIZUKU_OK
[3.9s] [PASS] Shizuku started!
```

---

## 📄 致谢
* USB Host 与 ADB 协议栈基于 [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost)
* 权限框架基于 [Shizuku](https://github.com/RikkaApps/Shizuku)
