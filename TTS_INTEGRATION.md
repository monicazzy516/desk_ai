# OpenAI TTS 集成文档

## 📝 概述

已成功集成 OpenAI TTS（Text-to-Speech）功能到 desk_ai 项目。现在系统在 SPEAKING 状态时会：
1. 同时播放 AI 生成的语音
2. 在屏幕上显示文字
3. SPEAKING 状态时长 = 音频播放时长
4. 播放完成后自动返回 IDLE 状态

---

## ✅ 已完成的修改

### 1. **后端（Python）** - `backend_server.py`

#### 新增功能
- ✅ 添加 `_tts_to_pcm()` 函数
  - 使用 OpenAI TTS API（`tts-1` 模型，`alloy` 语音）
  - 输入：文本
  - 输出：16-bit PCM @ 24kHz 单声道原始音频数据

#### 修改功能
- ✅ 更新 `/upload` 端点
  - **旧版**: 返回纯 JSON（`{"ok": true, "user_text": "...", "reply_text": "..."}`）
  - **新版**: 返回 JSON + "\n" + PCM 音频
  - 流程: `STT → LLM → TTS → 返回文字+音频`

#### 返回格式
```
JSON内容 + "\n" + PCM音频数据
```

JSON 字段：
```json
{
  "ok": true,
  "user_text": "用户说的话（STT结果）",
  "reply_text": "AI回复文字（LLM结果）",
  "sample_rate": 24000
}
```

---

### 2. **ESP32 固件 - 音频模块** (`main/audio.c` & `main/audio.h`)

#### 新增接口
```c
// 播放完成回调函数类型
typedef void (*audio_play_done_cb_t)(uint32_t samples, uint32_t sample_rate_hz);

// 更新后的播放函数签名
void audio_play_pcm(
    const int16_t *pcm, 
    uint32_t samples, 
    uint32_t sample_rate_hz, 
    audio_play_done_cb_t done_cb  // 新增：播放完成回调
);
```

#### 实现细节
- ✅ `play_pcm_task()` 播放完成后调用 `done_cb` 回调
- ✅ 所有错误情况也会触发回调（samples=0）
- ✅ 支持不同采样率（24kHz for TTS, 48kHz for recording）

---

### 3. **ESP32 固件 - 状态机** (`main/state.c`)

#### 新增功能
- ✅ 添加 `audio_play_done_callback()` 回调函数
  - 播放完成后自动调用 `set_state(STATE_IDLE)`
  - 计算并打印播放时长

#### 流程优化
- ✅ **THINKING 状态**: 后端成功后**自动切换**到 SPEAKING（无需点击）
- ✅ **SPEAKING 状态**: 开始播放音频，播放完成后**自动返回** IDLE
- ✅ **无音频时**: 允许用户点击返回 IDLE

---

### 4. **ESP32 固件 - UI** (`main/ui.c`)

#### 交互逻辑更新
```c
IDLE → [点击] → LISTENING
LISTENING → [点击] → RECORDED
RECORDED → [点击] → THINKING
THINKING → [自动] → SPEAKING  // 后端处理完成后自动切换
SPEAKING → [自动] → IDLE      // 音频播放完成后自动返回
SPEAKING → [点击] → IDLE      // 允许用户提前中断
```

#### 修改内容
- ✅ THINKING 状态点击无效（等待后端处理）
- ✅ SPEAKING 状态允许点击提前中断
- ✅ 更新 UI 初始化日志

---

## 🎯 完整工作流程

### 用户操作流程
```
1. [IDLE] 点击屏幕 → 进入 LISTENING
2. [LISTENING] 说话，再次点击 → 进入 RECORDED
3. [RECORDED] 点击 → 进入 THINKING（上传音频）
4. [THINKING] 后端处理中...
   ↓ 自动切换（无需点击）
5. [SPEAKING] 播放音频 + 显示文字
   ↓ 音频播放完成（自动）
6. [IDLE] 回到待机状态
```

### 后端处理流程
```
用户音频(PCM) → Whisper STT → user_text
                     ↓
                 GPT-4O-Mini → reply_text
                     ↓
                 TTS API → reply_audio_pcm
                     ↓
          返回: JSON + PCM 音频
```

---

## 🔧 技术参数

| 参数 | 录音 | TTS播放 |
|------|------|---------|
| 采样率 | 48kHz | 24kHz |
| 位深度 | 16-bit | 16-bit |
| 声道 | 单声道 | 单声道 |
| 格式 | PCM | PCM |
| 模型 | Whisper-1 | TTS-1 |
| 语音 | - | alloy |
| 语言 | 英文 | 英文 |

---

## 📊 文件修改统计

| 文件 | 修改内容 |
|------|----------|
| `backend_server.py` | +47 行，新增 TTS 功能，修改返回格式 |
| `main/audio.c` | +30 行，添加播放完成回调 |
| `main/audio.h` | +9 行，新增回调类型定义 |
| `main/state.c` | +25 行，添加自动状态切换 |
| `main/ui.c` | +5 行，更新交互逻辑 |

---

## 🚀 使用方法

### 1. 启动后端服务器
```bash
cd /Users/zhangziyu/Documents/desk_ai
python3 backend_server.py
```

### 2. 编译并烧录 ESP32
```bash
. ~/esp/esp-idf/export.sh
cd /Users/zhangziyu/Documents/desk_ai
idf.py fullclean build flash monitor
```

### 3. 测试流程
1. 点击屏幕进入录音
2. 说英文（例如："What's the weather today?"）
3. 再次点击停止录音
4. 点击上传（进入 THINKING）
5. **自动切换到 SPEAKING**，播放 AI 语音回复
6. **自动返回 IDLE**

---

## ⚡ 性能优化

### 音频时长计算
- 24kHz 采样率
- 假设回复 2 秒音频 = 48,000 samples
- 传输大小 ≈ 96KB

### 自动化优势
- **旧版**: 需要用户点击 3 次才能听到回复
- **新版**: 只需点击 3 次，后续自动播放并返回 IDLE
- **用户体验**: 更流畅，无需额外操作

---

## 🐛 故障排查

### 问题1: 没有声音
**检查**:
- 后端日志是否显示 "TTS: generated XXX bytes"
- ESP32 日志是否显示 "play_pcm: played XXX samples"
- 扬声器硬件连接是否正常

### 问题2: 播放后没有自动返回 IDLE
**检查**:
- 回调函数是否被调用（查看日志 "audio play done"）
- 音频数据是否有效（samples > 0）

### 问题3: OpenAI TTS API 错误
**检查**:
- API Key 是否正确
- 网络是否能访问 OpenAI（可能需要代理）
- 查看后端日志的详细错误信息

### 问题4: 音频卡顿或失真
**可能原因**:
- I2S 配置问题
- 采样率不匹配（确认后端返回 24kHz）
- DMA 缓冲区过小

---

## 📝 待优化项（可选）

1. **音频格式优化**
   - 考虑使用 MP3 压缩减少传输量
   - ESP32 端添加 MP3 解码器

2. **多语言支持**
   - TTS 支持多种语音（nova, echo, fable 等）
   - 动态选择语言和语音

3. **流式播放**
   - 边接收边播放，减少等待时间
   - 需要重构缓冲区管理

4. **音量控制**
   - 添加音量调节功能
   - 支持静音模式

---

## ✅ 测试清单

- [x] 后端 TTS API 调用成功
- [x] ESP32 接收并解析音频数据
- [x] 音频播放正常
- [x] 播放完成回调触发
- [x] 自动返回 IDLE 状态
- [x] 文字同步显示
- [x] 用户可提前中断播放
- [ ] 长时间运行稳定性测试
- [ ] 多次连续对话测试

---

## 🎉 完成！

现在你的桌面 AI 助手已经具备完整的语音对话能力：
- ✅ 语音输入（STT）
- ✅ AI 理解（LLM）
- ✅ 语音输出（TTS）
- ✅ 屏幕显示（UI）
- ✅ 自动化流程（UX）
