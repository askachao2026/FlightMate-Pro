# 刷机说明 / Flashing Guide

## 适用设备

- FoloToy AI Passport
- ESP32-C3，8 MB Flash
- 240×320 显示屏
- ES8311 音频编解码器

其他硬件请不要直接刷写。合并镜像按 AI Passport 的引脚和分区表构建。

## 准备

- Python 3.10 或更高版本
- esptool 4.x 或 5.x：`python -m pip install --upgrade esptool`
- 一根支持数据传输的 USB-C 线

AI Passport 使用芯片原生 USB Serial/JTAG，Windows 10/11 通常无需额外驱动。

## 找到串口

Windows：

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Description
```

或在设备管理器的「端口 (COM 和 LPT)」中查看。常见为 `COM19`、`COM15` 等。

Linux/macOS：

```bash
ls /dev/ttyACM* /dev/tty.usbmodem* 2>/dev/null
```

## 刷写合并镜像

```bash
python -m esptool --chip esp32c3 --port COM19 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 firmware/FlightMatePro-20261020-merged.bin
```

把 `COM19` 换成实际串口。若当前 esptool 版本使用连字符命令，也可以写成 `write-flash`。

合并镜像会覆盖整片 Flash，包括 NVS。Wi-Fi 配置、屏幕亮度和本地飞行日志会被清除。

## 校验

刷写前：

```bash
sha256sum firmware/FlightMatePro-20261020-merged.bin
```

Windows PowerShell：

```powershell
Get-FileHash firmware\FlightMatePro-20261020-merged.bin -Algorithm SHA256
```

正确值：

```text
b8c39b60dc835c8713a35065d06f9833c38957343269349dace462ecf49c06c3
```

刷写后读回校验：

```bash
python -m esptool --chip esp32c3 --port COM19 --baud 460800 \
  verify_flash 0x0 firmware/FlightMatePro-20261020-merged.bin
```

## 常见问题

### 找不到串口

- 确认 USB 线支持数据传输，不是只供电线。
- 换一个 USB 口，避免使用无源 Hub。
- Windows 设备管理器中出现黄色感叹号时，安装对应 USB 串口驱动。

### 一直停在 Connecting

- 关闭占用串口的串口监视器、Arduino IDE、PlatformIO 或其他刷机工具。
- 让设备进入下载模式：按住 BOOT 键再点按 RESET，然后松开 BOOT，再执行刷写。
- 降低波特率：把 `--baud 460800` 改为 `--baud 115200`。

### 刷写后黑屏或反复重启

- 确认设备是 8 MB Flash 的 AI Passport。
- 重新下载固件并核对 SHA-256。
- 先执行 `erase_flash`，再重新写入合并镜像：

  ```bash
  python -m esptool --chip esp32c3 --port COM19 erase_flash
  ```

### 配网失败

- AI Passport 只支持 2.4 GHz Wi-Fi，不支持 5 GHz。
- 热点 `FlightMate-Setup` 的默认密码是 `flightmate`。
- 配网页面地址是 `http://192.168.4.1`，不是 HTTPS。
- 失败后按确认键重试；按下键清除已保存的 Wi-Fi 后重新配置。

## eFuse

FlightMate Pro 不执行任何 eFuse 写入，不启用 Secure Boot，也不启用 Flash 加密。仓库中的 `tools/check_no_efuse_write.py` 用于在构建产物中检查是否意外链接了 eFuse 写入路径。
