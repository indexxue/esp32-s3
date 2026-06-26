# OTA / Secure Boot 签名密钥

**私钥不得提交 Git。** 所有操作通过仓库根目录 **`idf`** 完成（见 [`README.md`](../README.md)）。

PowerShell 若提示找不到 `idf`：用 **`.\idf.cmd`**，或终端选 **「ESP-IDF PowerShell」**。

---

## 命令说明

### `idf signing-key-gen`

在 `keys/dev/` 下生成开发用 **RSA-3072** 私钥（Secure Boot v2），文件名为 `secure_boot_signing_key.pem`。内部调用 `espsecure generate_signing_key`。

| 选项 | 作用 |
|------|------|
| `--force` | 密钥已存在时强制重新生成（会覆盖旧密钥，已签名固件将无法再用旧钥验证） |

**前置**：无。**后续**：在 `signed_ota` / `secure_boot` 联调或量产构建前至少执行一次。

```powershell
idf signing-key-gen
idf signing-key-gen --force   # 仅在确认要轮换开发钥时使用
```

---

### `idf signing-profile <profile>`

切换当前工程（默认 `project/`）的签名 **sdkconfig 配置档**，并同步 `bootloader/sdkconfig.defaults`。

| profile | 作用 | eFuse |
|---------|------|-------|
| `none` | 恢复未签名日常开发（仅 `sdkconfig.defaults`） | 不涉及 |
| `signed_ota` | OTA 校验签名镜像，**不**启用硬件 Secure Boot | 不烧写 |
| `secure_boot` | 启用 Secure Boot v2（首次启动后 eFuse **不可逆**） | 量产首烧 |

**副作用（重要）**：会删除工程下的 `sdkconfig`、`sdkconfig.old` 和整个 `build/` 目录，下次需重新 `idf build`。`signed_ota` / `secure_boot` 还要求 `keys/dev/secure_boot_signing_key.pem` 已存在。

| 选项 | 作用 |
|------|------|
| `--build` | 切换配置后立刻执行完整编译（等价于切换 + `idf build`） |

```powershell
idf signing-profile signed_ota          # 只切换配置，不编译
idf signing-profile signed_ota --build    # 切换并编译（O12 联调常用）
idf signing-profile secure_boot --build   # 量产 Secure Boot 构建
idf signing-profile none                  # 恢复未签名开发
```

典型联调顺序见 [`doc/secure_boot_production.md`](../doc/secure_boot_production.md)。

---

### `idf release <版本> --signing-profile … --signing-key-id …`

对指定产品（默认 `project`）按 semver 版本号编译，并将产物归档到 `firmware/<版本>/`（`.bin`、`.hex`、`manifest.json` 等）。

| 选项 | 作用 |
|------|------|
| `--signing-profile signed_ota` | 合并 `sdkconfig.defaults.signed_ota` 并产出**已签名**镜像（会清除现有 `sdkconfig`） |
| `--signing-profile secure_boot` | 同上，使用 `sdkconfig.defaults.secure_boot` |
| `--signing-key-id dev` | 写入 manifest 的密钥标识，便于审计（设了 profile 时默认 `dev`） |
| `--flash-bundle` | 额外打包 bootloader、分区表、otadata 及 esptool 刷机脚本 |
| `--debug` | 额外打包 `.elf` / `.map`（勿用于现场 OTA） |

版本号与 `--signing-*` 选项顺序任意；`idf.ps1` 会自动调整为 `idf.py` 所需顺序。

```powershell
# 以下两种写法等价
idf -Project project release 1.0.10 --signing-profile signed_ota --signing-key-id dev
idf release --signing-profile signed_ota --signing-key-id dev 1.0.10
```

---

## 命令速查

| 目的 | 命令 |
|------|------|
| 生成开发密钥 | `idf signing-key-gen` |
| 启用 signed_ota（O12 联调） | `idf signing-profile signed_ota --build` |
| 启用 secure_boot（量产，eFuse） | `idf signing-profile secure_boot --build` |
| 恢复未签名开发 | `idf signing-profile none` |
| 打 signed release | `idf release 1.0.10 --signing-profile signed_ota --signing-key-id dev` |

---

## 密钥路径

| 路径 | 用途 |
|------|------|
| `keys/dev/secure_boot_signing_key.pem` | 开发 / 联调（`idf signing-key-gen` 生成，Git 忽略） |
| `keys/prod/` | 量产私钥目录（离线保管，不进 Git） |

---

## 环境验证（ESP-IDF 5.5.4）

以下命令已在仓库 ESP-IDF 环境中实测可用（2026-06-26，使用 `.\idf.cmd`）：

| 命令 | 验证方式 | 结果 |
|------|----------|------|
| `signing-key-gen --help` | 查看帮助 | 通过 |
| `signing-key-gen` | 执行（密钥已存在时提示用 `--force`） | 通过 |
| `signing-profile --help` | 查看帮助 | 通过 |
| `signing-profile signed_ota` | 切换配置（无 `--build`） | 通过；输出 `SDKCONFIG_DEFAULTS=…signed_ota` |
| `signing-profile secure_boot` | 切换配置（无 `--build`） | 通过 |
| `signing-profile none` | 恢复未签名 | 通过 |
| `release --help` | 查看帮助 | 通过 |
| `release 1.0.10 --signing-profile signed_ota --signing-key-id dev` | 启动 release（参数重排、`Using release version 1.0.10`、进入 cmake）后中止 | 通过 |

**说明**：`signing-profile` 会清空 `build/`；若 build 目录被占用（例如编译未结束），需关闭占用进程后重试。完整 `release` / `--build` 编译耗时较长，日常验证用 `--help` 或无 `--build` 的 `signing-profile` 即可。

---

详见 [`doc/secure_boot_production.md`](../doc/secure_boot_production.md)。
