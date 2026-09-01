# 简历项目经历（可直接复制）

> 说明：时间、项目名称可按实际调整。技术点均来自工程真实实现，面试可逐条展开。

---

## 版本一：推荐版（内容充实，适合项目经历是重点的简历）

### 基于 ESP32-S3 的 OneNET 物联网云控终端（温度采集 / RGB 远程控制 / OTA 升级）
**个人项目 · 独立开发** ｜ 2026.08
**硬件平台**：ESP32-S3（双核 240MHz，16MB Flash，8MB PSRAM）
**技术栈**：C、ESP-IDF v4.4.3、FreeRTOS、MQTT、HTTP、cJSON、RMT、mbedtls、CMake、中国移动 OneNET

**项目描述**：
基于 ESP32-S3 设计并实现一套接入中国移动 OneNET 云平台的物联网终端，实现设备数据上行（片内温度周期采集上报）、云端指令下行（远程控制 WS2812 RGB 灯颜色与亮灭）以及固件 OTA 在线升级；采用组件化分层架构，具备断线重连、异常自愈与 A/B 双分区无损升级能力。

**主要工作**：
- **分层架构设计**：基于 ESP-IDF 组件（component）机制将工程拆分为 Wi-Fi、传感器、RGB 驱动、MQTT 通信、OTA、业务主逻辑共 6 个低耦合模块，模块间通过头文件接口与注册回调解耦，由 CMake 统一构建，各驱动可独立复用。
- **联网与云平台通信**：基于 esp_event 默认事件循环 + FreeRTOS 事件组实现 STA 联网状态同步与断线自动重连；使用设备三元组（ProductID / DeviceName / Token，HMAC-MD5 签名）完成 OneNET MQTT 鉴权，按物模型协议实现属性上报（property/post）、指令下行（property/set）与应答（set_reply），QoS1 订阅，温度数据 30s 周期上行。
- **云端指令解析与外设执行**：基于 cJSON 解析物模型下行报文，对 number / bool / string 三类参数统一分发；实现 RGB 颜色字符串多格式解析（颜色英文名 / `#RRGGBB` / `r,g,b`），并通过 **RMT 外设**按 WS2812 纳秒级单总线时序（80MHz 八分频、1 tick=100ns）编码 GRB 波形驱动，开关状态与颜色状态解耦，支持云端实时变色与亮灭。
- **OTA 双分区远程升级（核心）**：自定义 16MB Flash 分区表（ota_0 / ota_1 各 6MB，另含 NVS、otadata、SPIFFS），独立任务实现「MQTT 升级通知 → HTTP 版本/任务校验 → 分块下载写入备用分区 → 镜像合法性校验 → 切换启动分区 → 重启运行」完整 A/B 升级链路；集成 mbedtls 对固件做**流式 MD5 完整性校验**，按平台协议上报下载进度（step）与结果状态码，并实现防重入、HTTP 失败重试、Content-Length 校验、新固件 mark-valid 防回滚等保护，升级失败可回退原固件、零变砖风险；实测约 860KB 固件 19s 内完成下载与自动切换。
- **稳定性问题攻关**：定位并解决多个底层疑难——关闭 modem-sleep 规避片内温度传感器与 PHY/RTC 时钟冲突导致的任务看门狗（TWDT）复位；用 topic_len + memcmp 精确匹配非 `\0` 结尾的 MQTT 主题、修正 `set_reply` 主题格式，避免被 Broker 主动断开；OTA 期间暂停周期上报任务以规避网络/内存竞争，重启前经回调优雅释放 MQTT 与 Wi-Fi 连接（同时避免组件循环依赖）。

**项目成果**：
设备稳定接入 OneNET 平台，温度上报与 RGB 云控端到端响应 <1s；连续多版本 OTA（1.0.2→1.0.6）均一次成功且可回滚。系统掌握了 FreeRTOS 多任务调度与同步、ESP-IDF 外设驱动、MQTT 物模型协议与嵌入式 OTA 全流程设计。

---

## 版本二：精简版（简历空间紧张时用，4 条）

### 基于 ESP32-S3 的 OneNET 物联网云控终端
**个人项目 · 独立开发** ｜ C / ESP-IDF / FreeRTOS / MQTT / OTA ｜ 2026.08
- 基于 ESP32-S3 + ESP-IDF 搭建接入中国移动 OneNET 的物联网终端，组件化拆分 Wi-Fi、外设驱动、MQTT 通信、OTA 等 6 个模块，用事件组与回调实现多任务同步与模块解耦。
- 基于 MQTT 物模型实现温度周期上报与 RGB 灯远程控制：cJSON 解析 number/bool/string 下行指令，通过 RMT 外设按 WS2812 纳秒时序编码驱动，支持云端实时变色与开关。
- 独立实现 A/B 双分区 OTA：自定义分区表，完成「MQTT 通知→HTTP 分块下载→写备用分区→MD5 校验→切换分区→重启」全链路，含防重入、失败重试、进度上报与防回滚，860KB 固件约 19s 升级完成、失败可回退、零变砖。
- 攻关解决任务看门狗复位（温度传感器与 Wi-Fi 时钟冲突）、MQTT 主题匹配导致被 Broker 断开等底层问题，设备长时间稳定在线、端到端控制延迟 <1s。

---

## 可单独抽取的「技能关键词」

ESP32-S3、ESP-IDF、FreeRTOS（任务/事件组/看门狗）、C、MQTT（QoS/物模型/Topic 设计）、HTTP Client、cJSON、RMT、WS2812、OTA（A/B 双分区、bootloader、防回滚）、mbedtls/MD5、NVS、Wi-Fi STA、中国移动 OneNET、CMake、串口日志调试

---

## 附：面试高频考点（对照准备，确保每条都能讲清）

1. **讲一下你的 OTA 流程 / 为什么用双分区？**
   A/B 分区：运行 ota_0 时把新固件写 ota_1，校验通过才改 otadata 启动指针，失败仍从旧分区启动，避免刷写中断变砖。
2. **otadata 的作用？mark_app_valid 是干什么的？**
   otadata 记录从哪个分区启动；新固件首次启动是 PENDING_VERIFY 状态，确认运行正常后 mark valid，否则下次启动回滚。
3. **FreeRTOS 里怎么同步 Wi-Fi 连上再启动 MQTT？**
   事件组 EventGroup：拿到 IP 事件置位，主流程 xEventGroupWaitBits 阻塞等待。
4. **RMT 驱动 WS2812 的原理？为什么是 GRB？**
   WS2812 用高低电平脉宽编码 0/1（T0H/T1H 纳秒级），RMT 硬件能精确产生时序；灯珠内部顺序是 GRB，所以要调换 R/G。
5. **任务看门狗（TWDT）为什么触发？怎么解决？**
   任务长时间不让出 CPU / 底层读取自旋卡死，IDLE 任务得不到喂狗。解决：读取前后 esp_task_wdt_reset、降低任务优先级、vTaskDelay 让出 CPU、关闭时钟冲突源。
6. **MQTT QoS0/1 区别？断线怎么办？**
   QoS1 保证至少一次到达（有 PUBACK）；断线事件里自动重连，重连后重新订阅主题。
7. **MD5 校验怎么做的？为什么要边下边算？**
   mbedtls MD5 上下文对每个下载分片 update，全部完成后 final 得到摘要，与平台返回的 md5 比对；流式计算不需要额外 860KB 内存缓存整个固件。
8. **组件之间怎么解耦？**
   OTA 不直接调用 MQTT（会形成组件循环依赖），而是暴露「注册预重启回调」接口，由 main 注入关闭网络的函数。
