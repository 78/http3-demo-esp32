# QUIC 客户端项目

这是一个基于 ESP32 的物联网 QUIC/HTTP3 客户端项目，支持 WiFi 和 4G 网络连接，集成了电源管理功能，主要用于音频流传输和物联网数据通信。

## 🌟 项目特性

### 网络连接
- **双网络模式**：支持 WiFi 和 4G 网络自动切换
- **QUIC/HTTP3**：基于 RFC 9000/9114 协议，支持现代网络传输
- **并发请求**：支持同一连接上的多流并发通信
- **自动重连**：网络断开后自动重连机制

### 硬件集成
- **4G 调制解调器**：支持 EC801E 等 EC 系列模块的 UART 以太网通信
- **电源管理**：集成 AXP2101 PMIC 芯片，支持电池充电管理和低功耗模式
- **GPIO 控制**：支持重启按钮和低功耗唤醒

### 应用功能
- **音频流传输**：支持 OGG 音频文件的流式上传
- **物联网通信**：适用于远程监控、数据上传等场景
- **低功耗设计**：支持 Light Sleep 和 GPIO 唤醒

## 📋 支持的目标平台

| 目标平台 | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 | Linux |
|----------|-------|----------|----------|----------|----------|-----------|----------|----------|----------|----------|-------|
| 支持状态 | ✅    | ❌       | ❌       | ❌       | ❌       | ❌        | ❌       | ❌       | ✅       | ✅       | ❌     |

## 🚀 快速开始

### 环境要求

- ESP-IDF v5.4+
- 支持的 ESP32 开发板
- 可选：4G 调制解调器模块（EC801E 等 EC 系列）
- 可选：AXP2101 电源管理模块

### 构建和烧录

1. **克隆项目**
   ```bash
   git clone <repository-url>
   cd quic-client
   ```

2. **配置项目**
   ```bash
   idf.py set-target esp32s3
   idf.py menuconfig
   ```

3. **构建项目**
   ```bash
   idf.py build
   ```

4. **烧录到设备**
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## ⚙️ 配置选项

### 网络模式选择

项目支持两种网络模式，通过 `USE_WIFI_MODE` 宏控制：

- `USE_WIFI_MODE = 1`：WiFi 模式
- `USE_WIFI_MODE = 0`：4G 模式（默认）

### WiFi 配置

在 `main.cc` 中配置 WiFi 凭据：
```cpp
auto& wifi = WifiStation::GetInstance();
wifi.AddAuth("YOUR_SSID", "YOUR_PASSWORD");
```

### 4G 配置

UART 引脚配置（在 `main.cc` 中定义）：
```cpp
static constexpr gpio_num_t MODEM_TX_PIN = GPIO_NUM_13;
static constexpr gpio_num_t MODEM_RX_PIN = GPIO_NUM_14;
static constexpr gpio_num_t MODEM_RESET_PIN = GPIO_NUM_17;
static constexpr gpio_num_t MODEM_POWER_PIN = GPIO_NUM_18;
static constexpr gpio_num_t MODEM_RI_PIN = GPIO_NUM_11;
static constexpr gpio_num_t MODEM_DTR_PIN = GPIO_NUM_12;
```

## 📁 项目结构

```
quic-client/
├── CMakeLists.txt                 # 主构建配置
├── sdkconfig.defaults            # 默认 SDK 配置
├── main/                          # 主应用程序
│   ├── CMakeLists.txt
│   ├── main.cc                    # 应用程序入口
│   ├── http3_manager.h/cc         # HTTP3 连接管理器
│   ├── uart_eth_modem.h/cc        # 4G 调制解调器驱动
│   ├── uart_uhci.h/cc             # UART UHCI 驱动
│   ├── test.ogg                   # 测试音频文件
│   └── pmic/                      # 电源管理模块
│       ├── pmic.h/cc              # PMIC 主接口
│       ├── axp2101.h/cc           # AXP2101 驱动
│       └── i2c_device.h/cc        # I2C 设备基础类
├── components/                    # 组件
│   └── esp-http3/                 # QUIC/HTTP3 协议栈
├── managed_components/            # 托管组件
│   ├── espressif__iot_eth/        # 以太网组件
│   └── 78__esp-wifi-connect/      # WiFi 连接组件
└── build/                         # 构建输出目录
```

## 🔧 核心组件说明

### Http3Manager

QUIC/HTTP3 连接管理器，提供：
- 持久连接维护
- 并发流支持
- 自动重连机制
- 流控和错误处理

### UartEthModem

4G 调制解调器驱动，支持：
- UART 通信协议
- MRDY/SRDY 握手协议
- 网络状态监控
- 低功耗管理

### PMIC (AXP2101)

电源管理集成电路驱动，提供：
- 电池状态监控
- 充电管理
- 电源路径控制
- 中断处理

## 📡 API 使用示例

### 基本 HTTP3 请求

```cpp
// 初始化管理器
auto& manager = Http3Manager::GetInstance();
Http3ManagerConfig config;
config.hostname = "api.tenclass.net";
config.port = 443;
manager.Init(config);

// 发送请求
Http3Request req;
req.method = "GET";
req.path = "/pocket-sage/health";

Http3Response resp;
if (manager.SendRequest(req, resp)) {
    ESP_LOGI(TAG, "Status: %d, Body: %s", resp.status, resp.body.c_str());
}
```

### 音频流上传

```cpp
Http3Request chat_req;
chat_req.method = "POST";
chat_req.path = "/pocket-sage/chat/stream";
chat_req.headers = {
    {"content-type", "application/octet-stream"},
    {"x-audio-sample-rate", "16000"}
};

int stream = manager.OpenStream(chat_req, callbacks);
manager.QueueWrite(stream, audio_data, audio_size);
manager.FinishStream(stream);
```

## 🔍 测试和调试

### 测试功能

项目包含完整的测试流程：
1. 连接建立测试
2. 并发请求测试
3. 音频流上传测试
4. 连接保持测试

### 日志输出

使用 ESP-IDF 日志系统，标签：
- `HTTPS_DEMO`: 主程序日志
- `HTTP3_MGR`: HTTP3 管理器日志
- `UART_MODEM`: 调制解调器日志
- `PMIC`: 电源管理日志

### 连接统计

运行测试后会输出详细的连接统计信息：
```
=== Connection Stats ===
  Packets sent: 13
  Packets received: 10
  Bytes sent: 1868
  Bytes received: 5881
  RTT: 72 ms
```

## 🛠️ 故障排除

### 常见问题

1. **网络连接失败**
   - 检查 WiFi 凭据或 4G 模块配置
   - 确认硬件连接正确

2. **QUIC 握手失败**
   - 检查服务器证书和网络连通性
   - 确认防火墙设置

3. **电源管理问题**
   - 检查 AXP2101 I2C 连接
   - 确认电池和充电电路

### 调试命令

```bash
# 监控串口输出
idf.py monitor

# 清理构建
idf.py clean

# 完整重构建
idf.py clean build
```

## 📚 技术文档

### 协议支持
- **QUIC v1** (RFC 9000)
- **HTTP/3** (RFC 9114)
- **TLS 1.3** (RFC 8446)

### 硬件接口
- **UART**: 高速 UART 通信 (3Mbps)
- **I2C**: PMIC 电源管理
- **GPIO**: 控制和中断信号

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📄 许可证

[根据项目许可证]

---

**注意**: 这是一个物联网演示项目，使用前请根据实际硬件和网络环境调整配置参数。