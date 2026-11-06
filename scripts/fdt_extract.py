#!/usr/bin/env python3
"""Extract raw (unmangled) data from a downstream Qualcomm DTB.

The fdt repo's dtb_to_pretty_dts.py guesses phandles when printing, which
mangles plain integers (e.g. panel-width). This script prints raw words and
resolves only real phandle references (property values that point at nodes
carrying a matching 'phandle' property).

Usage:
    fdt_extract.py <file.dtb> <command> [args]
Commands:
    prop <node-substring> <prop-substring>   # print matching props
    node <node-substring>                    # print full node(s)
    panels                                   # list panel node names
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src", "fdt"))

from pyfdt.pyfdt import FdtBlobParse, FdtNode, FdtPropertyWords, FdtPropertyBytes, FdtPropertyStrings


def iter_nodes(node, path="/"):
    yield path.rstrip("/") or "/", node
    for sub in node.subdata:
        if isinstance(sub, FdtNode):
            name = sub.get_name()
            yield from iter_nodes(sub, path + name + "/")


def collect_phandle_map(root):
    """phandle value -> node path"""
    m = {}
    for path, node in iter_nodes(root):
        for item in node.subdata:
            if isinstance(item, FdtPropertyWords) and item.get_name() == "phandle":
                m[item.words[0]] = path
    return m


def fmt_prop(prop, phandles):
    name = prop.get_name()
    if isinstance(prop, FdtPropertyWords):
        vals = []
        for w in prop.words:
            if w in phandles and (name.endswith("-supply") or "gpio" in name or len(prop.words) in (1, 3, 4)):
                vals.append(f"<0x{w:x} -> {phandles[w]}>")
            else:
                vals.append(f"{w} (0x{w:x})")
        return f"{name} = <{' '.join(vals)}>"
    if isinstance(prop, FdtPropertyBytes):
        return f"{name} = [{prop.raw.hex(' ')}] ({len(prop.raw)} bytes)"
    if isinstance(prop, FdtPropertyStrings):
        return f"{name} = {' '.join(prop.strings)!r}"
    return f"{name};"


def main():
    dtb_path, cmd = sys.argv[1], sys.argv[2]
    with open(dtb_path, "rb") as f:
        fdt = FdtBlobParse(f).to_fdt()
    root = fdt.get_rootnode()
    phandles = collect_phandle_map(root)

    if cmd == "panels":
        for path, node in iter_nodes(root):
            for item in node.subdata:
                if isinstance(item, FdtPropertyStrings) and item.get_name() == "qcom,mdss-dsi-panel-name":
                    print(f"{path}: {item.strings}")
        return

    if cmd == "node":
        sub = sys.argv[3].lower()
        for path, node in iter_nodes(root):
            if sub in path.lower():
                print(f"\n=== {path} ===")
                for item in node.subdata:
                    if not isinstance(item, FdtNode):
                        try:
                            print(f"  {fmt_prop(item, phandles)}")
                        except Exception as e:
                            print(f"  {item.get_name()} <error {e}>")
                for sub2 in node.subdata:
                    if isinstance(sub2, FdtNode):
                        print(f"  [child] {sub2.get_name()}")
        return

    if cmd == "prop":
        node_sub, prop_sub = sys.argv[3].lower(), sys.argv[4].lower()
        for path, node in iter_nodes(root):
            if node_sub and node_sub not in path.lower():
                continue
            for item in node.subdata:
                if isinstance(item, (FdtPropertyWords, FdtPropertyBytes, FdtPropertyStrings)):
                    if prop_sub in item.get_name().lower():
                        try:
                            print(f"{path}: {fmt_prop(item, phandles)}")
                        except Exception as e:
                            print(f"{path}: {item.get_name()} <error {e}>")
        return

    print(__doc__)


if __name__ == "__main__":
    main()
