"""Versus 패키지(tipp7.versus.geode) 도구. GitHub 릴리스가 이 저장소만으로 만들어졌는지 누구나 확인할 수 있게 한다.

    python3 tools/geode_package.py modjson <GD 버전> [--repo 저장소 폴더]
        mod.json을 GD 2.2081(Geode 5) 또는 GD 2.2091(Geode 6) 빌드용으로 바꾼다. 바뀌는 것은 Geode 버전,
        플랫폼별 GD 버전, Eclipse 의존성의 버전 범위, Argon 의존성(GD 2.2091만)뿐이고 나머지 줄은 그대로 둔다.
    python3 tools/geode_package.py normalize <입력.geode> <출력.geode> [--epoch 초]
        같은 내용이면 언제 만들어도 바이트까지 같은 파일이 되도록 다시 묶는다
        (항목 이름순, 시각은 --epoch 또는 SOURCE_DATE_EPOCH, deflate 9, 권한 고정).
    python3 tools/geode_package.py check <패키지.geode> [--repo 저장소 폴더]
        바이너리 5개를 뺀 모든 항목이 저장소의 파일에서 왔는지 확인한다. mod.json은 패키지가 가리키는 GD 버전용으로
        바꾼 저장소의 mod.json(modjson)과 비교한다.
        PNG는 픽셀, 텍스트는 줄바꿈을 무시한 내용, 그 밖의 파일은 바이트로 비교한다.
        (Geode CLI가 PNG를 다시 압축하고, Windows 빌드는 텍스트를 CRLF로 꺼내므로 바이트는 다를 수 있다.)
    python3 tools/geode_package.py compare <a.geode> <b.geode>
        두 패키지의 SHA-256과 항목별 차이를 보여 준다. 다르면 종료 코드 1.
    python3 tools/geode_package.py list <패키지.geode>
        항목별 "sha256  크기  이름" 목록 (릴리스의 contents.sha256).

check는 Pillow가 필요하다 (pip install pillow).
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import io
import json
import os
import re
import sys
import time
import zipfile

MOD_ID = "tipp7.versus"
BINARIES = ("tipp7.versus.dll", "tipp7.versus.dylib", "tipp7.versus.ios.dylib",
            "tipp7.versus.android32.so", "tipp7.versus.android64.so")
ROOT_TEXT = ("mod.json", "about.md", "changelog.md")
ROOT_IMAGES = ("logo.png",)
TEXT_SUFFIXES = (".txt", ".md", ".json")
ZIP_EPOCH_MIN = 315532800  # 1980-01-01, zip이 담을 수 있는 가장 이른 시각
# 빌드할 수 있는 GD 버전: 그 버전의 Geode SDK 버전, Eclipse(선택 의존성)의 버전 범위, Argon 모드 의존성
# (없으면 None, GD 2.2081은 Argon을 CPM으로 넣는다).
# Eclipse는 v1.10.0부터 Geode 6용이다. Geode CLI는 Geode 버전을 보지 않고 범위에 맞는 가장 최근 버전을 받으므로,
# GD 2.2081 빌드가 Geode 5용 Eclipse(설정 API 헤더)를 받도록 범위를 나눈다.
GD_TARGETS = {
    "2.2081": {"geode": "5.7.1", "eclipse": "<v1.10.0", "argon": None},
    "2.2091": {"geode": "6.0.0", "eclipse": ">=v1.10.0", "argon": ">=v2.0.0-beta.1"},
}
GD_PLATFORMS = ("win", "android", "mac", "ios")
ARGON_ID = "dankmeme.argon"
ECLIPSE_ID = "eclipse.eclipse-menu"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def target_mod_json(text: str, gd: str) -> str:
    """mod.json 내용(text)을 GD 버전 gd 빌드용으로 바꾼 내용. 줄바꿈과 다른 줄은 그대로 둔다."""
    if gd not in GD_TARGETS:
        raise SystemExit(f"unknown GD version {gd} (known: {', '.join(GD_TARGETS)})")
    target = GD_TARGETS[gd]
    text, count = re.subn(r'^(\s*"geode"\s*:\s*)"[^"]*"', lambda m: f'{m.group(1)}"{target["geode"]}"', text,
                          count=1, flags=re.M)
    if count != 1:
        raise SystemExit('mod.json: no "geode" line')
    gd_block = re.search(r'"gd"\s*:\s*\{[^}]*\}', text)
    if not gd_block:
        raise SystemExit('mod.json: no "gd" object')
    block = re.sub(r'("(?:' + "|".join(GD_PLATFORMS) + r')"\s*:\s*)"[^"]*"', lambda m: f'{m.group(1)}"{gd}"',
                   gd_block.group(0))
    text = text[:gd_block.start()] + block + text[gd_block.end():]
    eclipse = re.search(r'("' + re.escape(ECLIPSE_ID) + r'"\s*:\s*\{[^}]*?"version"\s*:\s*)"[^"]*"', text)
    if not eclipse:
        raise SystemExit(f'mod.json: no "{ECLIPSE_ID}" dependency with a "version"')
    text = text[:eclipse.start()] + f'{eclipse.group(1)}"{target["eclipse"]}"' + text[eclipse.end():]
    argon_line = re.search(r'^[ \t]*"' + re.escape(ARGON_ID) + r'"\s*:\s*"[^"]*",?[ \t]*\r?\n', text, flags=re.M)
    if target["argon"] is None:
        if argon_line:
            text = text[:argon_line.start()] + text[argon_line.end():]
    elif argon_line:
        line = re.sub(r'(:\s*)"[^"]*"', lambda m: f'{m.group(1)}"{target["argon"]}"', argon_line.group(0), count=1)
        text = text[:argon_line.start()] + line + text[argon_line.end():]
    else:
        # Node IDs 의존성 바로 아래에 같은 들여쓰기로 넣는다.
        anchor = re.search(r'^([ \t]*)"geode\.node-ids"\s*:\s*"[^"]*",?[ \t]*(\r?\n)', text, flags=re.M)
        if not anchor:
            raise SystemExit('mod.json: no "geode.node-ids" dependency line to put Argon after')
        line = f'{anchor.group(1)}"{ARGON_ID}": "{target["argon"]}",{anchor.group(2)}'
        text = text[:anchor.end()] + line + text[anchor.end():]
    meta = json.loads(text)
    assert meta["geode"] == target["geode"], meta["geode"]
    assert all(meta["gd"][name] == gd for name in GD_PLATFORMS), meta["gd"]
    assert meta["dependencies"][ECLIPSE_ID]["version"] == target["eclipse"], meta["dependencies"]
    assert meta["dependencies"].get(ARGON_ID) == target["argon"], meta["dependencies"]
    return text


def write_mod_json(repo: str, gd: str) -> bool:
    """저장소의 mod.json을 gd 빌드용으로 바꿔 쓴다. 바뀌었으면 True."""
    path = os.path.join(repo, "mod.json")
    with open(path, encoding="utf-8", newline="") as handle:
        text = handle.read()
    result = target_mod_json(text, gd)
    if result != text:
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(result)
    return result != text


def package_gd(entries: dict[str, bytes]) -> str:
    """패키지의 mod.json이 가리키는 GD 버전."""
    meta = json.loads(entries["mod.json"])
    versions = {meta["gd"][name] for name in GD_PLATFORMS}
    if len(versions) != 1:
        raise SystemExit(f"mod.json gd versions differ by platform: {meta['gd']}")
    return versions.pop()


def read_entries(path: str) -> dict[str, bytes]:
    """패키지의 파일 항목 전부(폴더 항목 제외). zstd 항목은 읽지 않고 멈춘다."""
    with zipfile.ZipFile(path) as package:
        entries = {}
        for info in package.infolist():
            if info.is_dir():
                continue
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise SystemExit(f"{info.filename}: compression {info.compress_type} is not stored/deflate "
                                 "(build with zstd off: older Geode loaders cannot open zstd packages)")
            if info.filename in entries:
                raise SystemExit(f"duplicate entry {info.filename}")
            entries[info.filename] = package.read(info)
    return entries


def normalize(source: str, target: str, epoch: int) -> str:
    stamp = time.gmtime(max(epoch, ZIP_EPOCH_MIN))[:6]
    entries = read_entries(source)
    with zipfile.ZipFile(target, "w") as package:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=stamp)
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            package.writestr(info, entries[name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    with open(target, "rb") as handle:
        return sha256(handle.read())


def same_text(a: bytes, b: bytes) -> bool:
    return a.replace(b"\r\n", b"\n") == b.replace(b"\r\n", b"\n")


def same_pixels(a: bytes, b: bytes) -> bool:
    from PIL import Image
    left, right = Image.open(io.BytesIO(a)), Image.open(io.BytesIO(b))
    return left.size == right.size and left.convert("RGBA").tobytes() == right.convert("RGBA").tobytes()


def same_file(name: str, packed: bytes, original: bytes) -> tuple[bool, str]:
    """(같은지, 어떻게 비교했는지)."""
    if packed == original:
        return True, "bytes"
    lower = name.lower()
    if lower.endswith(".png"):
        return same_pixels(packed, original), "pixels"
    if lower.endswith(TEXT_SUFFIXES):
        return same_text(packed, original), "text"
    return False, "bytes"


def repo_resources(repo: str) -> dict[str, str]:
    """mod.json의 resources.files가 가리키는 저장소 파일: 패키지 안 이름 -> 저장소 경로."""
    with open(os.path.join(repo, "mod.json"), encoding="utf-8") as handle:
        meta = json.load(handle)
    resources = meta.get("resources", {})
    unknown = set(resources) - {"files"}
    if unknown:
        raise SystemExit(f"mod.json resources has {sorted(unknown)}: this checker only knows 'files'")
    found: dict[str, str] = {}
    for pattern in resources.get("files", []):
        for path in sorted(glob.glob(os.path.join(repo, pattern))):
            if not os.path.isfile(path):
                continue
            packed = f"resources/{MOD_ID}/{os.path.basename(path)}"
            if packed in found:
                raise SystemExit(f"two repository files would both be {packed}")
            found[packed] = path
    return found


def check(package_path: str, repo: str) -> bool:
    entries = read_entries(package_path)
    expected = repo_resources(repo)
    gd = package_gd(entries) if "mod.json" in entries else None
    problems: list[str] = []
    counts: dict[str, int] = {}

    def note(kind: str) -> None:
        counts[kind] = counts.get(kind, 0) + 1

    for name in BINARIES:
        if name not in entries:
            problems.append(f"missing binary {name}")
    for name, data in sorted(entries.items()):
        if name in BINARIES:
            note("binaries (built from src/ by the workflow)")
            continue
        if name == ".geode_cache":
            # Geode CLI가 만드는 목록. 이 모드는 스프라이트시트·폰트가 없으므로 비어 있어야 한다.
            if json.loads(data) != {"spritesheets": {}, "fonts": {}}:
                problems.append(".geode_cache is not empty")
            note("generated .geode_cache")
            continue
        if name == "mod.json":
            path = os.path.join(repo, name)
            with open(path, encoding="utf-8", newline="") as handle:
                original = target_mod_json(handle.read(), gd).encode("utf-8")
            ok, how = same_file(name, data, original)
            if not ok:
                problems.append(f"mod.json: differs from the repository's mod.json for GD {gd} ({how})")
            else:
                note(f"same as the repository ({how}, mod.json for GD {gd})")
            continue
        if name in ROOT_TEXT or name in ROOT_IMAGES:
            path = os.path.join(repo, name)
        elif name in expected:
            path = expected[name]
        else:
            problems.append(f"{name}: not from the repository")
            continue
        if not os.path.isfile(path):
            problems.append(f"{name}: {path} does not exist")
            continue
        with open(path, "rb") as handle:
            ok, how = same_file(name, data, handle.read())
        if not ok:
            problems.append(f"{name}: differs from {os.path.relpath(path, repo)} ({how})")
        else:
            note(f"same as the repository ({how})")
    for name in sorted(set(expected) - set(entries)):
        problems.append(f"{name}: in the repository but not in the package")

    print(f"{package_path}: GD {gd}, {len(entries)} entries, sha256 {sha256(open(package_path, 'rb').read())}")
    for kind, count in sorted(counts.items()):
        print(f"  {count:4d}  {kind}")
    for problem in problems:
        print(f"  PROBLEM  {problem}")
    print("OK: everything except the binaries comes from the repository" if not problems else "FAILED")
    return not problems


def listing(path: str) -> str:
    entries = read_entries(path)
    return "".join(f"{sha256(data)}  {len(data):9d}  {name}\n" for name, data in sorted(entries.items()))


def compare(a: str, b: str) -> bool:
    raw_a, raw_b = open(a, "rb").read(), open(b, "rb").read()
    print(f"{a}: sha256 {sha256(raw_a)} ({len(raw_a)} bytes)")
    print(f"{b}: sha256 {sha256(raw_b)} ({len(raw_b)} bytes)")
    if raw_a == raw_b:
        print("IDENTICAL: the two packages are byte-for-byte the same")
        return True
    left, right = read_entries(a), read_entries(b)
    for name in sorted(set(left) | set(right)):
        if name not in left or name not in right:
            print(f"  only in {'a' if name in left else 'b'}: {name}")
        elif left[name] != right[name]:
            print(f"  differs: {name} ({sha256(left[name])[:12]} vs {sha256(right[name])[:12]})")
    print("DIFFERENT")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    zero = sub.add_parser("modjson")
    zero.add_argument("gd", choices=sorted(GD_TARGETS))
    zero.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    one = sub.add_parser("normalize")
    one.add_argument("source")
    one.add_argument("target")
    one.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", ZIP_EPOCH_MIN)))
    two = sub.add_parser("check")
    two.add_argument("package")
    two.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    three = sub.add_parser("compare")
    three.add_argument("a")
    three.add_argument("b")
    four = sub.add_parser("list")
    four.add_argument("package")
    args = parser.parse_args()
    if args.command == "modjson":
        changed = write_mod_json(args.repo, args.gd)
        target = GD_TARGETS[args.gd]
        print(f"mod.json: GD {args.gd}, Geode {target['geode']}" + ("" if changed else " (already)"))
        return 0
    if args.command == "normalize":
        print(normalize(args.source, args.target, args.epoch))
        return 0
    if args.command == "check":
        return 0 if check(args.package, args.repo) else 1
    if args.command == "compare":
        return 0 if compare(args.a, args.b) else 1
    sys.stdout.write(listing(args.package))
    return 0


if __name__ == "__main__":
    sys.exit(main())
