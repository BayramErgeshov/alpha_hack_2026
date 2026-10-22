"""Prepare synthetic datasets and page-addressable bank analytics, entirely locally."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile


def safe_stem(name: str) -> str:
    return re.sub(r"[^\w.-]+", "_", Path(name).stem).strip("_.") or "analytics"


def clean_text(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\x00", "")
    return "\n".join(line.rstrip() for line in text.lstrip("\ufeff").splitlines()).strip()


def write_text_atomic(path: Path, text: str) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def prepare_companies(root: Path) -> None:
    archive_path = next(root.glob("Данные для участников*.zip"), None)
    if archive_path is None:
        raise SystemExit("Expected the supplied company-data ZIP archive in the project root")
    destination = root / "data" / "input"
    destination.mkdir(parents=True, exist_ok=True)
    required = {"df_Uid_data_hack.csv", "df_Uid_payment_hack.csv", "df_Fid_Uid_link_hack.csv"}
    with ZipFile(archive_path) as archive:
        found = {Path(info.filename).name: info for info in archive.infolist() if not info.is_dir()}
        missing = required - found.keys()
        if missing:
            raise SystemExit("Dataset archive is missing: " + ", ".join(sorted(missing)))
        for name in sorted(required):
            with archive.open(found[name]) as source, (destination / name).open("wb") as target:
                shutil.copyfileobj(source, target)
            print(f"Extracted {name}", flush=True)


def find_windows_ocr(root: Path, explicit: Path | None) -> Path | None:
    if explicit:
        if not explicit.is_file():
            raise SystemExit(f"OCR executable does not exist: {explicit}")
        return explicit.resolve()
    candidates = [root / "x64/Release/AlphaPdfOcr.exe", root / "x64/Debug/AlphaPdfOcr.exe",
                  root / "build/Release/AlphaPdfOcr.exe", root / "build/AlphaPdfOcr.exe"]
    return next((path for path in candidates if path.is_file()), None)


def prepare_analytics(root: Path, args: argparse.Namespace) -> None:
    try:
        import pypdfium2 as pdfium
    except ImportError as exc:
        raise SystemExit("Install the local PDF engine: python -m pip install pypdfium2") from exc

    archive_path = next(root.glob("Аналитические материалы*.zip"), None)
    if archive_path is None:
        raise SystemExit("Expected the supplied analytics ZIP archive in the project root")
    output = root / "data" / "analytics"
    output.mkdir(parents=True, exist_ok=True)
    windows_ocr = find_windows_ocr(root, args.ocr_executable) if os.name == "nt" else None
    tesseract = shutil.which("tesseract")
    method = "none"
    if args.ocr in ("auto", "windows") and windows_ocr:
        method = "windows_ocr"
    elif args.ocr in ("auto", "tesseract") and tesseract:
        method = "tesseract_ocr"
    if args.ocr == "windows" and method != "windows_ocr":
        raise SystemExit("Build AlphaPdfOcr in Visual Studio, or pass --ocr-executable PATH")
    if args.ocr == "tesseract" and method != "tesseract_ocr":
        raise SystemExit("Tesseract OCR is not installed or is not on PATH")
    language = args.ocr_language or ("ru" if method == "windows_ocr" else "rus+eng")
    print(f"Local OCR: {method}; language: {language}", flush=True)

    manifest: dict = {"schema_version": "1.0", "source_archive": archive_path.name, "documents": []}
    temporary_root = root / "work" / "ocr-temp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with ZipFile(archive_path) as archive, tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
        temporary = Path(temporary)
        entries = [entry for entry in archive.infolist()
                   if not entry.is_dir() and entry.filename.lower().endswith(".pdf")]
        if not entries:
            raise SystemExit("Analytics archive contains no PDF files")
        used_names: set[str] = set()
        for entry in entries:
            filename = Path(entry.filename).name
            stem = safe_stem(filename)
            if stem in used_names:
                stem += "_" + str(len(used_names) + 1)
            used_names.add(stem)
            raw = archive.read(entry)
            document = pdfium.PdfDocument(raw)
            pages: list[dict] = []
            texts: list[str] = []
            try:
                for index in range(len(document)):
                    page = document[index]
                    try:
                        textpage = page.get_textpage()
                        try:
                            text = clean_text(textpage.get_text_bounded())
                        finally:
                            textpage.close()
                        origin = "pdf_text" if text else "unavailable"
                        if not text and method != "none":
                            image_path = temporary / "page.png"
                            text_path = temporary / "page.txt"
                            width, height = page.get_size()
                            scale = min(3.0, 2500 / max(width, height))
                            bitmap = page.render(scale=scale)
                            try:
                                bitmap.to_pil().save(image_path)
                            finally:
                                bitmap.close()
                            if method == "windows_ocr":
                                command = [str(windows_ocr), str(image_path), str(text_path), language]
                            else:
                                command = [str(tesseract), str(image_path), "stdout", "-l", language]
                            result = subprocess.run(command, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=120, check=False)
                            if result.returncode:
                                raise RuntimeError(f"OCR failed for {filename}, page {index + 1}: {result.stderr.strip()}")
                            text = clean_text(text_path.read_text(encoding="utf-8")
                                if method == "windows_ocr" else result.stdout)
                            origin = method if text else "unavailable"
                        texts.append(text)
                        pages.append({"page": index + 1, "method": origin, "characters": len(text)})
                        print(f"  {filename}: page {index + 1}/{len(document)} - {len(text)} characters ({origin})", flush=True)
                    finally:
                        page.close()
            finally:
                document.close()

            origins = sorted({page["method"] for page in pages if page["method"] != "unavailable"})
            extraction = "+".join(origins) or "unavailable"
            lines = [f"# {filename}\n", f"<!-- extraction: {extraction} -->\n"]
            for index, text in enumerate(texts, start=1):
                lines.extend([f"\n[[PAGE: {index}]]\n", text or "<!-- TEXT_UNAVAILABLE -->", "\n"])
            write_text_atomic(output / f"{stem}.md", "".join(lines))
            manifest["documents"].append({
                "source_file": entry.filename, "source_sha256": hashlib.sha256(raw).hexdigest(),
                "text_file": f"{stem}.md", "extraction": extraction, "pages": pages,
                "page_count": len(pages), "pages_with_text": sum(bool(text) for text in texts),
                "text_characters": sum(len(text) for text in texts),
            })

    manifest["summary"] = {
        "document_count": len(manifest["documents"]),
        "page_count": sum(item["page_count"] for item in manifest["documents"]),
        "pages_with_text": sum(item["pages_with_text"] for item in manifest["documents"]),
        "text_characters": sum(item["text_characters"] for item in manifest["documents"]),
    }
    write_text_atomic(output / "manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    summary = manifest["summary"]
    print("Analytics prepared: " + json.dumps(summary, ensure_ascii=False), flush=True)
    if summary["pages_with_text"] != summary["page_count"]:
        raise SystemExit("Some pages have no text. Build AlphaPdfOcr (Windows) or install Tesseract (Linux) and rerun --analytics-only")


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).resolve().parents[1])
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--analytics-only", action="store_true")
    modes.add_argument("--data-only", action="store_true")
    parser.add_argument("--ocr", choices=("auto", "windows", "tesseract", "none"), default="auto")
    parser.add_argument("--ocr-executable", type=Path)
    parser.add_argument("--ocr-language")
    args = parser.parse_args()
    root = args.root.resolve()
    if not args.analytics_only:
        prepare_companies(root)
    if not args.data_only:
        prepare_analytics(root, args)


if __name__ == "__main__":
    main()
