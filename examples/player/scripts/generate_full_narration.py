from __future__ import annotations

import re
from pathlib import Path

import numpy as np
import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = Path(
    r"C:\Users\atkio2\.codex\attachments\c6760b17-a179-47d1-885e-8445f35dbe34\pasted-text.txt"
)
OUTPUT_DIR = ROOT / "voice_output" / "full_narration_parts"
REFERENCE = ROOT / "voice_output" / "reference_00m00s_00m09s.wav"
FINAL_WAV = ROOT / "voice_output" / "ESP32-S3_无损播放器_李思默口播_完整版.wav"

REF_TEXT = (
    "从飞线变成四层PCB。这次重新设计PCB的时候，我老公没有直接照着洞洞板"
    "重新连一遍。第一步先考虑整块板子的结构。"
)
MAX_CHARS = 115

PLACEHOLDER_LINES = {
    "【以下内容等实测完成后补录】",
    "频率响应方面……",
    "底噪方面……",
    "THD+N 方面……",
    "ABX 一共进行了……轮。",
    "正确……轮。",
    "最后的结果是……",
}


def normalize_for_speech(text: str) -> str:
    replacements = {
        "ESP32-S3-WROOM-1-N16R8": "ESP三十二 S三 WROOM一 N十六 R八",
        "ESP32-S3": "ESP三十二 S三",
        "ESP-IDF 5.5.1": "ESP I D F 五点五点一",
        "Sound Blaster PLAY! 3": "Sound Blaster Play 三",
        "Sony Xperia 5 V": "Sony Xperia Five V",
        "Xperia 5 V": "Xperia Five V",
        "Xperia": "Xperia",
        "CX31993": "C X 三一九九三",
        "PCM5100A": "P C M 五一零零 A",
        "RP2040": "R P 二零四零",
        "ST7789": "S T 七七八九",
        "JLCPCB": "J L C P C B",
        "PCB": "P C B",
        "USB": "U S B",
        "UAC2.0": "U A C 二点零",
        "UAC1": "U A C 一",
        "SD": "S D",
        "SPI2": "S P I 二",
        "SPI3": "S P I 三",
        "SPI Host": "S P I Host",
        "I2S": "I 方 S",
        "DAC": "D A C",
        "OTA": "O T A",
        "DRC": "D R C",
        "BOM": "B O M",
        "CPL": "C P L",
        "SMT": "S M T",
        "CNC": "C N C",
        "THD+N": "T H D 加 N",
        "ABX": "A B X",
        "MP3": "M P 三",
        "WAV": "W A V",
        "FLAC": "FLAC",
        "PCM": "P C M",
        "PSRAM": "P S RAM",
        "FAT32": "FAT 三十二",
        "Q1.31": "Q 一点三一",
        "24-bit": "二十四 bit",
        "24 位": "二十四位",
        "32 位": "三十二位",
        "16 位": "十六位",
        "96kHz": "九十六千赫兹",
        "44.1kHz": "四十四点一千赫兹",
        "20MHz": "二十兆赫兹",
        "40MHz": "四十兆赫兹",
        "400kHz": "四百千赫兹",
        "1133KiB/s": "每秒一千一百三十三 K i B",
        "3.5 毫米": "三点五毫米",
        "5V": "五伏",
    }
    for source, target in replacements.items():
        text = text.replace(source, target)
    return text


def load_chunks() -> tuple[list[str], set[int]]:
    lines = SCRIPT_PATH.read_text(encoding="utf-8").splitlines()
    chunks: list[str] = []
    section_breaks: set[int] = set()
    pending: list[str] = []
    pending_len = 0

    def flush() -> None:
        nonlocal pending, pending_len
        if pending:
            chunks.append(normalize_for_speech("".join(pending)))
            pending = []
            pending_len = 0

    for raw in lines:
        line = raw.strip()
        if line.startswith("## 视频简介中的"):
            break
        if not line or line == "---":
            continue
        if line.startswith("#"):
            flush()
            if chunks:
                section_breaks.add(len(chunks))
            continue
        if line in PLACEHOLDER_LINES or re.fullmatch(r"https?://\S+", line):
            continue
        if pending and pending_len + len(line) > MAX_CHARS:
            flush()
        pending.append(line)
        pending_len += len(line)
    flush()
    return chunks, section_breaks


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    chunks, section_breaks = load_chunks()
    (OUTPUT_DIR / "chunks.txt").write_text(
        "\n".join(f"{i:03d}\t{text}" for i, text in enumerate(chunks, 1)),
        encoding="utf-8",
    )
    print(f"chunks={len(chunks)} cuda={torch.cuda.is_available()}", flush=True)

    model = Qwen3TTSModel.from_pretrained(
        "Qwen/Qwen3-TTS-12Hz-1.7B-Base",
        device_map="cuda:0",
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
    )
    voice_prompt = model.create_voice_clone_prompt(
        ref_audio=str(REFERENCE), ref_text=REF_TEXT, x_vector_only_mode=False
    )

    sample_rate = 24000
    for index, text in enumerate(chunks, 1):
        part_path = OUTPUT_DIR / f"part_{index:03d}.wav"
        if part_path.exists() and part_path.stat().st_size > 1000:
            print(f"skip {index:03d}/{len(chunks):03d}", flush=True)
            continue
        print(f"generate {index:03d}/{len(chunks):03d}: {text}", flush=True)
        wavs, sample_rate = model.generate_voice_clone(
            text=text,
            language="Chinese",
            voice_clone_prompt=voice_prompt,
            max_new_tokens=4096,
        )
        sf.write(part_path, wavs[0], sample_rate)

    assembled: list[np.ndarray] = []
    for index in range(1, len(chunks) + 1):
        audio, sr = sf.read(OUTPUT_DIR / f"part_{index:03d}.wav", dtype="float32")
        if sr != sample_rate:
            raise RuntimeError(f"Unexpected sample rate {sr} in part {index}")
        assembled.append(audio)
        pause_seconds = 0.72 if index in section_breaks else 0.30
        assembled.append(np.zeros(round(sample_rate * pause_seconds), dtype=np.float32))
    sf.write(FINAL_WAV, np.concatenate(assembled), sample_rate, subtype="PCM_16")
    print(f"completed={FINAL_WAV}", flush=True)


if __name__ == "__main__":
    main()
