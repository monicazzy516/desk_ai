# OpenAI API 配置说明

## ✅ 已完成的配置

### 1. 音频压缩
- **采样率**: 从 48kHz 降至 24kHz
- **录音时长**: 从 10秒 延长到 **20秒**
- **内存占用**: 保持 960KB 不变
- **说明**: 24kHz 是硬件兼容的最佳选择（16kHz 会导致 I2S 超时错误）

### 2. OpenAI API 集成

#### 后端配置 (`backend_server.py`)
- ✅ 已配置 OpenAI API Key（存储在 `.env` 文件中）
- ✅ 语音转文字（STT）: 使用 Whisper API，支持中文识别
- ✅ AI 回复生成: 使用 GPT-4O-Mini，中文对话
- ✅ 在后端日志中打印用户输入和 AI 回复

#### 前端配置 (ESP32)
- ✅ 启用中文字体：`lv_font_source_han_sans_sc_14_cjk` (思源黑体)
- ✅ UI 显示 AI 回复文本

## 📁 文件说明

### 新增文件
- `.env` - OpenAI API Key 配置（已添加到 .gitignore）
- `requirements.txt` - Python 依赖包
- `.gitignore` - 保护敏感文件

### 修改文件
- `backend_server.py` - 支持中文 STT 和 LLM 回复
- `main/audio.c` - 采样率改为 24kHz
- `main/ui.c` - 使用中文字体
- `sdkconfig` - 启用中文字体支持

## 🚀 使用步骤

### 1. 启动后端服务器

```bash
cd /Users/zhangziyu/Documents/desk_ai
python3 backend_server.py
```

后端会监听在 `0.0.0.0:5001`

### 2. 编译并烧录 ESP32

```bash
idf.py build flash monitor
```

### 3. 测试流程

1. **点击屏幕** → 进入 LISTENING 状态（录音开始）
2. **说中文** → 最长可录制 20秒
3. **再次点击** → 进入 RECORDED 状态
4. **再次点击** → 进入 THINKING 状态（上传音频到后端）
5. **后端处理**:
   - STT 转录音频为文字
   - LLM 生成中文回复
   - 在后端日志打印: `[backend] STT 最终文本: xxx`
   - 在后端日志打印: `[backend] LLM 回复: xxx`
6. **再次点击** → 进入 SPEAKING 状态
7. **屏幕显示** AI 回复的中文文本

## 📝 后端日志示例

```
[backend] /upload id=a1b2c3d4: samples=480000, duration=20.000s -> uploads/rec_20260216_190000_a1b2c3d4.wav
[backend] STT 原始结果: '今天天气怎么样' (长度=7)
[backend] STT 最终文本: 今天天气怎么样
[backend] LLM 回复: 今天天气晴朗，温度适宜。
```

## ⚠️ 注意事项

1. **API Key 安全**: `.env` 文件已添加到 `.gitignore`，不会被 git 提交
2. **网络连接**: ESP32 需要连接 WiFi，后端需要能访问 OpenAI API
3. **采样率限制**: 
   - ✅ 24kHz 正常工作
   - ❌ 16kHz 会导致硬件超时
   - ✅ 48kHz 可用但录音时间短
4. **中文字体**: 思源黑体字体包含常用汉字，但体积较大（约 24KB）

## 🔧 故障排查

### 问题1: 录音失败 (recorded 0 samples)
**解决**: 确保采样率为 24kHz 或 48kHz（不要使用 16kHz）

### 问题2: 后端无响应
**检查**: 
- 后端是否运行：`python3 backend_server.py`
- WiFi 是否连接
- 后端 URL 配置是否正确

### 问题3: 中文显示为方框
**解决**: 确保 `CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK=y` 已启用

### 问题4: OpenAI API 错误
**检查**:
- API Key 是否正确
- 网络是否能访问 OpenAI（可能需要代理）
- 查看后端日志的详细错误信息

## 📊 系统配置总结

| 项目 | 配置 |
|------|------|
| 采样率 | 24kHz |
| 录音时长 | 20秒 |
| 位深度 | 16-bit |
| 声道 | 单声道 |
| STT 模型 | Whisper-1 |
| LLM 模型 | GPT-4O-Mini |
| 字体 | 思源黑体 14px |
| 后端端口 | 5001 |

## 🎉 完成！

现在你的桌面 AI 助手已经可以：
- 录制最长 20秒的中文语音
- 自动转录为文字
- 生成中文 AI 回复
- 在屏幕上显示中文回复文本
