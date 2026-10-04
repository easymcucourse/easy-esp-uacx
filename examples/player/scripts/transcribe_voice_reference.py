from pathlib import Path
import sys

from faster_whisper import WhisperModel


DEFAULT_SOURCE = Path(r"C:\Users\atkio2\Documents\xwechat_files\lisimo1992_0b64\msg\file\2026-09\田中町 67.m4a")


def main() -> None:
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    model = WhisperModel("small", device="cpu", compute_type="int8")
    segments, info = model.transcribe(
        str(source), language="zh", vad_filter=True, beam_size=5
    )
    print(f"language={info.language} probability={info.language_probability:.3f}")
    for segment in segments:
        print(f"{segment.start:8.2f}\t{segment.end:8.2f}\t{segment.text.strip()}")


if __name__ == "__main__":
    main()
