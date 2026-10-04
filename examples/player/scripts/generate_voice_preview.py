from pathlib import Path

import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "voice_output" / "reference_00m00s_00m09s.wav"
OUTPUT = ROOT / "voice_output" / "preview_opening.wav"

REF_TEXT = (
    "从飞线变成四层PCB。这次重新设计PCB的时候，我老公没有直接照着洞洞板"
    "重新连一遍。第一步先考虑整块板子的结构。"
)
PREVIEW_TEXT = (
    "我家的嵌入式工程师老公，最近又折腾了一个新东西。"
    "一台自己做的无损音乐播放器。"
    "我一开始其实挺不理解的。"
)


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    print(f"cuda={torch.cuda.is_available()} device={torch.cuda.get_device_name(0)}")
    model = Qwen3TTSModel.from_pretrained(
        "Qwen/Qwen3-TTS-12Hz-1.7B-Base",
        device_map="cuda:0",
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
    )
    wavs, sample_rate = model.generate_voice_clone(
        text=PREVIEW_TEXT,
        language="Chinese",
        ref_audio=str(REFERENCE),
        ref_text=REF_TEXT,
        max_new_tokens=2048,
    )
    sf.write(OUTPUT, wavs[0], sample_rate)
    print(f"wrote={OUTPUT} sample_rate={sample_rate}")


if __name__ == "__main__":
    main()
