#!/usr/bin/env python3
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def repo_to_pages_base(repo: str) -> str:
    owner, name = repo.split("/", 1)
    return f"https://{owner}.github.io/{name}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="owner/repo")
    ap.add_argument("--tag", required=True, help="release tag, e.g. v16.0.0")
    ap.add_argument("--update-file", required=True, help="path to the update zip on disk")
    ap.add_argument("--models-dir", required=True, help="directory containing model files to publish")
    ap.add_argument("--out", required=True, help="output manifest.json path")
    args = ap.parse_args()

    repo = args.repo
    tag = args.tag
    update_file = Path(args.update_file)
    models_dir = Path(args.models_dir)
    out_path = Path(args.out)

    pages_base = repo_to_pages_base(repo)
    generated_at = dt.datetime.now(dt.timezone.utc).isoformat()

    update_name = update_file.name
    update = {
        "tag": tag,
        "url": f"https://github.com/{repo}/releases/download/{tag}/{update_name}",
        "sha256": sha256_file(update_file),
        "size": update_file.stat().st_size,
    }

    models = []
    if models_dir.exists():
        for p in sorted(models_dir.iterdir()):
            if not p.is_file():
                continue
            if p.suffix.lower() not in {".tfl", ".tflite"}:
                continue
            models.append(
                {
                    "name": p.name,
                    "url": f"{pages_base}/models/{p.name}",
                    "sha256": sha256_file(p),
                    "size": p.stat().st_size,
                }
            )

    manifest = {
        "generated_at": generated_at,
        "repo": repo,
        "update": update,
        "models": models,
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

