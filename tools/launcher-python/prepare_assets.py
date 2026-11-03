"""Encode the source artwork for the launcher's small background panel."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageOps


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    with Image.open(args.source) as original:
        image = ImageOps.exif_transpose(original).convert('RGB')
        source_size = image.size
        image.thumbnail((1280, 1280), Image.Resampling.LANCZOS)
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        image.save(args.destination, 'JPEG', quality=85, optimize=True, progressive=True)
    print(json.dumps({'source_size': source_size, 'size': image.size,
                      'bytes': args.destination.stat().st_size}))


if __name__ == '__main__':
    main()
