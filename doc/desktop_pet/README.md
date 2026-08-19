# desktop_pet 文档入口

桌宠工程文档集中于此目录。仓库其它 `doc/*.md`（编码规范、OTA、分区等）与本产品无关，勿混读。

## 读这四个就够

| 文件 | 内容 |
|------|------|
| [`CONTEXT.md`](CONTEXT.md) | 术语表（皮肤包、Dock、对话页…） |
| [`framework.md`](framework.md) | 引擎：`pet_core` / 事件意图 / `pack.bin` / 模拟器 |
| [`product.md`](product.md) | 产品约定：皮肤与开机、主界面、对话页 D、决策与实现顺序 |
| [`cloud_asr.md`](cloud_asr.md) | 云端 ASR / 小智会话协议与里程碑 |

## 小智服务端（Docker）

对话页依赖本机 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)。当前联调目录在仓库外：`D:\Docker\xiaozhi-server`（含 `docker-compose.yml`、`data/`、`models/`）。

```powershell
cd D:\Docker\xiaozhi-server
docker compose up -d
docker logs -f xiaozhi-esp32-server
```

常用：

| 命令 | 说明 |
|------|------|
| `docker compose up -d` | 后台启动 |
| `docker logs -f xiaozhi-esp32-server` | 跟日志（确认 WS 地址） |
| `docker compose down` | 停止并移除容器 |
| `docker compose restart` | 重启 |

设备 WebSocket（把 IP 换成 PC 当前局域网地址；**勿用** Docker 内网 `172.x`）：

```text
ws://<PC-LAN-IP>:8000/xiaozhi/v1/
```

固件**当前默认接官方云**（`wss://api.tenclass.net/xiaozhi/v1/` + OTA 登记）。局域网 Docker 需把 `CONFIG_DESKTOP_PET_AGENT_OTA_URL` 留空，并把 WS URI 改回上面的 `ws://…`。

官方云：进对话页后字幕/日志出现 `code XXXXXX`，到 [xiaozhi.me](https://xiaozhi.me)「添加设备」填入；绑完退出再进对话。不要填控制台 MCP 的 `wss://api.xiaozhi.me/mcp/?token=…`。

## 辅助

- 圆屏 HTML 预览：[`tools/pet_tool/web/`](../../tools/pet_tool/web/)（`py -3 tools/pet_tool/serve.py`）— 仅产品态：开机 C / 主界面 / 对话 D
- 皮肤工具：[`tools/pet_tool/skin/README.md`](../../tools/pet_tool/skin/README.md)
- 工具伞：[`tools/pet_tool/README.md`](../../tools/pet_tool/README.md)
- AI 出图方案：[`skin_ai_asset_brief.md`](skin_ai_asset_brief.md)
- 工程说明：[`desktop_pet/README.txt`](../../desktop_pet/README.txt)
- **从哪开干**：见 [`product.md`](product.md) §E（当前建议：对话页 D）
