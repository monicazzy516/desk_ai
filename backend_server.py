#!/usr/bin/env python3
"""
简易后端：PCM → Whisper STT → user_text → LLM → reply_text；只返回 JSON，无音频；设备端在 UI 显示 reply_text。
运行：python3 backend_server.py  需 OPENAI_API_KEY（Whisper + Chat）。
默认监听 0.0.0.0:5001。请将 ESP32 Backend URL 设为 http://<本机IP>:5001
"""
import json
import os
import struct
import uuid
from datetime import datetime
from flask import Flask, request, jsonify, Response
from dotenv import load_dotenv

# 加载 .env 文件中的环境变量
load_dotenv()

app = Flask(__name__)

PORT = 5001
UPLOAD_DIR = "uploads"


def make_wav_header(sample_rate: int, channels: int, num_samples: int) -> bytes:
    """16bit PCM, mono or stereo."""
    byte_rate = sample_rate * channels * 2
    data_size = num_samples * channels * 2
    header = (
        b"RIFF"
        + struct.pack("<I", 36 + data_size)
        + b"WAVE"
        + b"fmt "
        + struct.pack("<IHHIIHH", 16, 1, channels, sample_rate, byte_rate, channels * 2, 16)
        + b"data"
        + struct.pack("<I", data_size)
    )
    return header


@app.route("/chat", methods=["POST"])
def chat():
    body = request.get_data(as_text=True)
    print("[backend] received:", body or "(empty)")
    return jsonify({"reply": "ok", "echo_len": len(body or "")})


# 音频上传协议：Body = raw PCM (little-endian int16)，格式由 HTTP Header 描述：
#   X-Sample-Rate, X-Channels, X-Format (e.g. pcm16)
# 可选扩展：JSON body { "sample_rate", "channels", "format", "data": base64 }


# 回复结构（与 ESP32 约定）：HTTP body = 纯 JSON，含 ok, user_text, reply_text，无音频


def _llm_reply(user_text: str) -> str:
    """用 OpenAI Chat 根据 user_text 生成回复，只返回 reply_text。失败返回空串。"""
    api_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not api_key:
        print("[backend] LLM: OPENAI_API_KEY not set, skip")
        return ""
    try:
        from openai import OpenAI
        client = OpenAI(api_key=api_key)
        r = client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[
                {"role": "system", "content": "You are a concise desk assistant. The user speaks in English. You must always reply in English only, in one or two short sentences."},
                {"role": "user", "content": user_text or "(no input)"},
            ],
            max_tokens=150,
        )
        reply_text = (r.choices[0].message.content or "").strip()
        print(f"[backend] LLM reply: {reply_text}")
        return reply_text
    except Exception as e:
        print(f"[backend] LLM error: {e}")
        return ""


# Whisper 在听不清/短音频时常见幻觉，视为无效（小写匹配）
STT_HALLUCINATION_PHRASES = (
    "subs by",
    "zeoranger",
    "thank you for watching",
    "subscribe",
    "you can't ask me that",
    "hey, you can't ask me that",
    "i can't help with that",
    "as an ai",
    "as a language model",
)


def _stt_whisper(wav_path: str, duration_sec: float = 0) -> str:
    """调用 OpenAI Whisper API 转写 wav 文件，返回 text。失败返回空串或提示。"""
    api_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not api_key:
        print("[backend] STT: OPENAI_API_KEY not set, skip Whisper")
        return ""
    try:
        from openai import OpenAI
        client = OpenAI(api_key=api_key)
        with open(wav_path, "rb") as f:
            transcription = client.audio.transcriptions.create(
                model="whisper-1",
                file=f,
                language="en",  # 指定英文语言减少误检与幻觉
                prompt="Transcribe the following speech in English.",
            )
        text = (transcription.text or "").strip()
        print(f"[backend] STT raw result: '{text}' (len={len(text)})")
        # 过滤已知幻觉/字幕水印
        lower = text.lower()
        if any(phrase in lower for phrase in STT_HALLUCINATION_PHRASES):
            print(f"[backend] STT FILTERED (hallucination): {text}")
            return ""
        if duration_sec > 0 and duration_sec < 0.5:
            print(f"[backend] STT: audio very short ({duration_sec:.2f}s), result may be unreliable")
        print(f"[backend] STT final text: {text}")
        return text
    except Exception as e:
        print(f"[backend] STT error: {e}")
        return ""


@app.route("/upload", methods=["POST"])
def upload():
    """接收 raw PCM，存 WAV，STT+LLM 后只返回 JSON（ok, user_text, reply_text），无音频。"""
    raw = request.get_data()
    sample_rate = int(request.headers.get("X-Sample-Rate", "48000"))
    channels = int(request.headers.get("X-Channels", "1"))
    fmt = request.headers.get("X-Format", "pcm16").strip().lower()

    if fmt != "pcm16":
        print(f"[backend] /upload: unsupported X-Format={fmt}, expect pcm16")
    bytes_per_sample = 2
    num_samples = len(raw) // (bytes_per_sample * channels)
    if num_samples == 0:
        print("[backend] /upload: empty body")
        return jsonify({"ok": False, "error": "empty"}), 400

    os.makedirs(UPLOAD_DIR, exist_ok=True)
    # 每个请求用唯一文件名，避免同一秒内多请求互相覆盖导致“转录错音频”
    req_id = uuid.uuid4().hex[:8]
    filename = os.path.join(UPLOAD_DIR, f"rec_{datetime.now().strftime('%Y%m%d_%H%M%S')}_{req_id}.wav")
    header = make_wav_header(sample_rate, channels, num_samples)
    with open(filename, "wb") as f:
        f.write(header)
        f.write(raw)

    duration_sec = num_samples / (sample_rate * channels)
    print(f"[backend] /upload id={req_id}: samples={num_samples}, duration={duration_sec:.3f}s -> {filename}")

    # PCM → Whisper → user_text → LLM → reply_text
    user_text = _stt_whisper(filename, duration_sec)
    reply_text = _llm_reply(user_text)

    # 只返回 JSON，无音频；ESP32 在 UI 上显示 reply_text
    reply = {
        "ok": True,
        "user_text": user_text,
        "reply_text": reply_text,
    }
    json_str = json.dumps(reply, separators=(",", ":"), ensure_ascii=False)
    body = json_str.encode("utf-8")
    resp = Response(body, mimetype="application/json")
    resp.headers["Content-Length"] = str(len(body))
    return resp


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT, debug=False)
