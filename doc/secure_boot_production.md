# Secure Boot v2 与镜像签名（阶段 3）

**工程**：`project/`（量产 v1）  
**算法**：Secure Boot v2 + RSA-3072（P3-1 暂定，与 ESP-IDF 默认一致）  
**关联**：[`doc/ota_development_plan.md`](ota_development_plan.md) 阶段 3、[`keys/README.md`](../keys/README.md)

---

## 两种 profile

| Profile | sdkconfig | eFuse | 用途 |
|---------|-----------|-------|------|
| **signed_ota** | `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` | 不烧 | 开发联调 O12：OTA 拒绝未签名包；仍可 USB 刷未签名固件 |
| **secure_boot** | `CONFIG_SECURE_BOOT` + v2 RSA | **首次启动后不可逆** | 量产 O11：仅签名镜像可启动 |

默认 `project/sdkconfig.defaults` **不含** 签名选项，日常 build 行为与阶段 1/2 一致。

---

## 开发联调（signed_ota，推荐第一步）

```powershell
idf signing-key-gen
idf signing-profile signed_ota --build
idf -p COM13 flash monitor
```

### O12 验收（未签名包被拒绝）

1. 设备 `run_ver` 记为 V（signed_ota 固件）。
2. 用**未签名** release 包（`idf signing-profile none` 后 `idf -Project project release <ver>`）尝试 SoftAP OTA。
3. 期望：串口 `esp_ota_end failed`；`GET /api/ota/status` → `state=idle`；`run_ver` 仍为 V。

### O13 验收（semver 独立于 SB）

上传**已签名**但版本 ≤ `run_ver` 的包 → 仍 HTTP **409**（D6，与阶段 1 相同）。

---

## 量产首烧（secure_boot）

> **警告**：启用硬件 Secure Boot 后，eFuse 一次性配置，**无法**通过 OTA 改回未签名 bootloader；USB 刷未签名 app 将无法启动。

### 1. 密钥

- 开发：`keys/dev/secure_boot_signing_key.pem`（脚本生成，不进 Git）
- 量产：`keys/prod/` 离线保管；CI/发布机签名，私钥不进仓库（P3-2）

### 2. 构建 signed release + flash bundle

```powershell
idf signing-profile secure_boot
idf -Project project release 1.2.3 --flash-bundle --signing-profile secure_boot --signing-key-id prod
```

产物见 `firmware/1.2.3/manifest.json` → `products.project.signed`、`signing`、`ota.signing_key_id`。

### 3. 烧录顺序（与 IDF 文档一致）

1. **`idf.py bootloader`** → 按终端提示 **单独** `esptool.py write_flash` 刷 **signed bootloader**（`idf.py flash` **不会**刷 SB bootloader）。
2. **`idf.py flash`** → 分区表 + **signed app** + otadata（或 `--flash-bundle` 整包脚本，须确认脚本内为 signed 文件名）。
3. 复位；串口确认 Secure Boot enabled、app 验签通过。
4. 应用侧在 `app_init` / Web 启动成功后调用 `ota_confirm_running_image()`（已有，D3 rollback 窗口不变）。

### 4. 禁止事项

| 操作 | 后果 |
|------|------|
| 对已 SB 设备 USB 刷**未签名** app | 无法启动；依赖 rollback 回旧槽（若旧槽有合法签名镜像） |
| 丢失量产私钥 | 无法发布新的可启动 OTA |
| 在未 Grill 的产线批量烧 eFuse | 不可逆配置错误 |

---

## O11 验收（已 SB 设备 OTA 合法签名包）

1. 设备已 `secure_boot` 首烧，`run_ver` = V。
2. `idf release <V+1> --signing-profile secure_boot`（或 `signed_ota` 在 SB 设备上亦须 signed 包）。
3. SoftAP 或 STA pull 上传 → apply → 新槽启动 → `mark_valid` 后 `run_ver` 升高。

---

## 与阶段 2 SHA256 的关系

- **SHA256**（manifest）：下载完整性，阶段 2 保留。
- **镜像签名**（IDF / Bootloader）：发布方身份 + 启动信任根。
- 二者叠加；SB **不替代** D6 semver 禁降级。

---

## 恢复未签名日常开发

```powershell
idf signing-profile none --build
```

---

## 参考

- [ESP-IDF Secure Boot v2 (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/security/secure-boot-v2.html)
- [Signed Applications](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/security/secure-boot-v2.html#signed-applications)
