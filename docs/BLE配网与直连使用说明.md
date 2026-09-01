# ESP32-S3 蓝牙配网 + 直连看温度/控灯 使用说明（只用 nRF Connect）

> 本版本**不需要 EspBlufi**，所有操作都用通用蓝牙调试 App **nRF Connect**（安卓/iOS 均可），
> 通过往自定义 GATT 特征写一条文本命令完成配网、控灯。

## 一、功能与 GATT 结构

BLE 广播名：**ESP32S3-IoT**，开机后一直广播（手机断开会自动重新广播）。

| Service | 特征 UUID | 属性 | 作用 |
|---|---|---|---|
| **0xE000** IoT Service | `0xE001` Temperature | Read / Notify | 读温度，订阅后每 30s 自动推送 |
| | `0xE002` Command | Write / WriteNoRsp | **写命令入口**（配网/控灯/查询，UTF-8 文本） |
| | `0xE003` Status | Read / Notify | 配网结果、IP、查询应答，**配网前先订阅它** |

### 0xE002 命令速查（大小写不敏感）
| 命令 | 含义 |
|---|---|
| `WIFI:热点名,密码` | 保存 WiFi 到 NVS 并立即联网（开放网络可只写 `WIFI:热点名`） |
| `STATE?` | 查询当前联网状态（结果从 0xE003 返回） |
| `ON` / `OFF` | 开 / 关 RGB 灯 |
| `RED` `GREEN` `BLUE` `YELLOW` `WHITE` `CYAN` `MAGENTA` | 命名颜色（自动开灯） |
| `#FF0000` 或 `255,0,0` | 自定义颜色 |

> 说明：`WIFI:` 后的热点名和密码**保持原样大小写**，用第一个逗号分隔，因此密码里可以含逗号。

---

## 二、menuconfig（已写入 sdkconfig，一般无需改）

Component config → Bluetooth：
- Bluetooth = Enabled，Host = **Bluedroid**
- Bluedroid Options：GATTS Enabled；**BLE 4.2 features = 开、BLE 5.0 features = 关**（否则传统广播 API 编译报错）
- **BluFi 已关闭**（本方案不用，省约 14KB）

---

## 三、首次蓝牙配网（重点，照做）

### 1. 擦除并烧录（清空旧 WiFi，进入待配网状态）
```powershell
idf.py -p COM30 erase-flash
idf.py -p COM30 flash monitor
```
现象：RGB 亮**青色**，串口提示
`No saved Wi-Fi! Use nRF Connect, write to 0xE002: WIFI:ssid,password`，并开始 BLE 广播。

### 2. 手机 nRF Connect 操作
1. 打开 nRF Connect，右上角 Scan，找到 **ESP32S3-IoT**，点 **CONNECT**。
2. 展开服务，找到 **Unknown Service 0xE000**。
3. **先点 `0xE003` 旁的三个向下箭头（Notify 订阅）**——这样配网结果会自动弹给你。
4. 找到 `0xE002`（Unknow Characteristic，带向上箭头 Write 图标），点**向上箭头**：
   - Value 类型选 **Text / UTF-8**（不要选 Hex）
   - 输入你的热点，例如：`WIFI:led,12345678`
   - 发送（建议选 **Write** 或 **Write without response** 都行）
5. 观察 `0xE003` 下方依次推送（串口同步打印）：
   ```
   Connecting to WiFi 'led' ...
   WiFi connected, IP: 10.17.118.210
   ```
   - 成功：设备灯变**紫**，随后自动连 OneNET MQTT、开始温度上报。
   - 失败：0xE003 推送 `WiFi connect FAILED: wrong password or AP not found (retrying)`，检查热点名/密码、确认是 **2.4GHz** 热点后重发命令即可。

### 3. 以后上电
凭证已存 NVS，开机自动连 WiFi + MQTT，**无需再配网**；BLE 仍可随时连来看温度/控灯。

### 更换 WiFi / 重新配网
- 直接再连 BLE，往 0xE002 写新的 `WIFI:新热点,新密码`，会覆盖旧凭证并切换网络。
- 彻底清空：`idf.py -p COM30 erase-flash`。

---

## 四、蓝牙直连看温度 / 控灯（联网后也能用）

### 看温度（0xE001）
- 点**单个向上箭头 Read**：立即读一次当前温度（如 `43.2`）。
- 点**三个向下箭头 Notify**：订阅后设备每个上报周期自动推送新温度。

### 控灯（0xE002，Text 写入）
- 写 `ON` 开灯、`OFF` 关灯。
- 写 `RED` / `#00FF00` / `0,0,255` 变色（写颜色会自动开灯）。
- 写 `STATE?` 可在 0xE003 看到当前 WiFi/IP 状态。

---

## 五、整体流程

```
上电
 ├─ NVS 有 WiFi？─有→ 自动连 WiFi → 连 MQTT（温度上报/云端控灯）
 │                └无→ 青色灯 + BLE 广播
 │                       nRF Connect 写 WIFI:ssid,pass → 存 NVS → 联网 → 连 MQTT
 └─ BLE 常驻：随时连 0xE000 服务读温度/收状态/写命令
```

---

## 六、常见问题

| 现象 | 解决 |
|---|---|
| 扫描不到 ESP32S3-IoT | 打开手机蓝牙+定位权限，靠近板子，按复位；手机断开后设备会自动重新广播 |
| 写了 WIFI 命令没反应 | 确认写的是 **0xE002**、类型是 **Text/UTF-8** 不是 Hex；格式 `WIFI:ssid,password` |
| 收不到配网结果 | 写命令前先订阅 **0xE003** 的 Notify；也可直接看串口日志 |
| 一直 FAILED | 热点必须 2.4GHz；密码正确；信号足够；可写 `STATE?` 看状态 |
| 编译报 esp_ble_gap_start_advertising 未定义 | menuconfig 里开 BLE 4.2、关 BLE 5.0 |
| 运行时蓝牙内存不足 | menuconfig→Bluedroid 开 Allocate BT stack from PSRAM（本板 8MB PSRAM） |
| 想清空已保存 WiFi | `idf.py -p COM30 erase-flash` |
