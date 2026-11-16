"""Summarise what occupies .text in an MSVC /MAP file.

Symbol sizes are inferred from the distance to the next symbol's address, then
grouped by the object/library the linker attributes them to, and by a rough
"family" from the demangled-ish symbol name (std::filesystem, nlohmann, ImGui,
iostream, WinRT...). With /LTCG the object attribution is coarse, so the
name-based families are usually the more useful view.

Usage: python map_text.py <file.map> [topN]
"""
import re
import sys
from collections import defaultdict

FAMILIES = [
    ("std::filesystem", re.compile(r"filesystem|__std_fs_")),
    ("iostream/locale", re.compile(r"basic_ostream|basic_istream|basic_ios|ios_base|locale|codecvt|ctype|numpunct|num_put|num_get|basic_filebuf|basic_streambuf|basic_ofstream|basic_ifstream")),
    ("nlohmann_json", re.compile(r"nlohmann")),
    ("ImGui", re.compile(r"ImGui|ImFont|ImDraw|ImStb|stbtt|stbrp|stb_|ImVector|ImGuiTable|ImPool|ImRect|ImTextChar|ImFormat")),
    ("WinRT", re.compile(r"winrt")),
    ("future/promise/thread", re.compile(r"promise|future|_Associated_state|thread")),
    ("std::function", re.compile(r"_Func_impl|_Func_base")),
    ("regex", re.compile(r"regex|_Matcher|_Builder")),
]


def main(path, top=25):
    lines = open(path, encoding="latin-1").read().splitlines()
    syms = []
    in_pub = False
    for ln in lines:
        if "Publics by Value" in ln:
            in_pub = True
            continue
        if in_pub:
            if ln.strip().startswith("entry point at") or "Static symbols" in ln:
                in_pub = False
                continue
            m = re.match(r"\s*0001:([0-9a-fA-F]+)\s+(\S+)\s+([0-9a-fA-F]+)\s+(f\s+)?(i\s+)?(.*)$", ln)
            if m:
                syms.append((int(m.group(1), 16), m.group(2), m.group(6).strip()))
        if "Static symbols" in ln:
            in_pub = "static"
            continue
    # static symbols section too
    in_static = False
    for ln in lines:
        if "Static symbols" in ln:
            in_static = True
            continue
        if in_static:
            m = re.match(r"\s*0001:([0-9a-fA-F]+)\s+(\S+)\s+([0-9a-fA-F]+)\s+(f\s+)?(i\s+)?(.*)$", ln)
            if m:
                syms.append((int(m.group(1), 16), m.group(2), m.group(6).strip()))
    syms.sort()
    by_obj = defaultdict(int)
    by_fam = defaultdict(int)
    total = 0
    for i, (off, name, obj) in enumerate(syms):
        nxt = syms[i + 1][0] if i + 1 < len(syms) else off
        size = max(0, nxt - off)
        if size > 1 << 20:
            size = 0
        total += size
        by_obj[obj.split()[-1] if obj else "?"] += size
        fam = "other"
        for fname, rx in FAMILIES:
            if rx.search(name):
                fam = fname
                break
        by_fam[fam] += size
    print(f".text symbols: {len(syms)}, attributed bytes: {total}")
    print("\nBy family:")
    for k, v in sorted(by_fam.items(), key=lambda kv: -kv[1]):
        print(f"  {v:9d}  {k}")
    print(f"\nTop {top} objects:")
    for k, v in sorted(by_obj.items(), key=lambda kv: -kv[1])[:top]:
        print(f"  {v:9d}  {k}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 25)
