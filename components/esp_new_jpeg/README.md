# esp_new_jpeg (vendored)

ESP32-S3 无硬件 JPEG，本工程使用 Espressif `esp_new_jpeg` 预编译库（SIMD 优化）。

- 来源：https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg
- 当前仅包含 `lib/esp32s3/libesp_new_jpeg.a`

若需更新库文件，在联网环境执行：

```powershell
curl.exe -L -o components/esp_new_jpeg/lib/esp32s3/libesp_new_jpeg.a `
  "https://cdn.jsdelivr.net/gh/espressif/esp-adf-libs@master/esp_new_jpeg/lib/esp32s3/libesp_new_jpeg.a"
```

或使用组件 registry（需网络正常）：

```text
idf.py add-dependency "espressif/esp_new_jpeg^1.0.2"
```

并删除本目录后改回 registry 依赖。
