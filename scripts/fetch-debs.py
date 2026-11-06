#!/usr/bin/env python3
"""Minimal debootstrap replacement for macOS hosts.

Downloads .deb packages (with dependencies) from an Ubuntu-ports mirror and
unpacks them into a staging rootfs directory, registering them in the dpkg
database so that apt on the device sees them as installed. No maintainer
scripts are run (the few that matter are handled by rootfs overlay units).

Usage: fetch-debs.py --staging DIR [--seed "pkg1 pkg2 ..."]
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

MIRROR = "https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports"
SUITE = "resolute"
COMPONENTS = ["main"]
ARCH = "arm64"
DEB_CACHE = os.path.join(os.path.dirname(__file__), "..", "downloads", "debs")


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), check=True, **kw)


def fetch(url, dest, sha256=None):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest
    for attempt in range(5):
        try:
            urllib.request.urlretrieve(url, dest + ".part")
            os.replace(dest + ".part", dest)
            break
        except Exception as e:
            print(f"retry {attempt+1} {url}: {e}", file=sys.stderr)
            if os.path.exists(dest + ".part"):
                os.remove(dest + ".part")
    else:
        raise SystemExit(f"download failed: {url}")
    if sha256:
        h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
        if h != sha256:
            raise SystemExit(f"sha256 mismatch for {url}")
    return dest


def parse_packages():
    """package -> {version, depends, filename, sha256, size, provides}"""
    index = {}
    for comp in COMPONENTS:
        url = f"{MIRROR}/dists/{SUITE}/{comp}/binary-{ARCH}/Packages.gz"
        gz = fetch(url, os.path.join(DEB_CACHE, f"Packages-{comp}.gz"))
        text = gzip.open(gz, "rt", encoding="utf-8", errors="replace").read()
        for stanza in text.split("\n\n"):
            if not stanza.strip():
                continue
            pkg = {}
            key = None
            for line in stanza.splitlines():
                if line.startswith((" ", "\t")):
                    continue  # multiline fields (Description) not needed
                if ":" in line:
                    key, _, val = line.partition(":")
                    key = key.strip()
                    pkg[key] = val.strip()
            name = pkg.get("Package")
            if name:
                index[name] = pkg
    return index


def parse_installed_status(staging):
    """names from the staging dpkg status (ubuntu base)"""
    path = os.path.join(staging, "var", "lib", "dpkg", "status")
    if not os.path.exists(path):
        return set()
    installed = set()
    name = None
    status = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("Package: "):
            name = line.split(": ", 1)[1].strip()
        elif line.startswith("Status: "):
            status = line.split(": ", 1)[1].strip()
        elif not line.strip() and name:
            if "installed" in status:
                installed.add(name)
            name, status = None, ""
    return installed


def parse_depends(dep_str):
    """'libc6 (>= 2.39), foo | bar' -> [['libc6'], ['foo', 'bar']]"""
    if not dep_str:
        return []
    out = []
    for group in dep_str.split(","):
        alts = [re.split(r"\s*[ (]", a.strip(), 1)[0] for a in group.split("|")]
        alts = [a for a in alts if a]
        if alts:
            out.append(alts)
    return out


def resolve(index, installed, seeds):
    """BFS; returns ordered list of package names to install"""
    todo = list(seeds)
    seen = set()
    order = []
    provides_map = {}
    for name, pkg in index.items():
        for p in re.split(r",\s*", pkg.get("Provides", "")):
            p = re.split(r"\s*[ (=]", p.strip(), 1)[0]
            if p:
                provides_map.setdefault(p, name)
    while todo:
        name = todo.pop(0)
        if name in installed or name in seen:
            continue
        if name not in index:
            name2 = provides_map.get(name)
            if not name2 or name2 in seen or name2 in installed:
                continue
            name = name2
        seen.add(name)
        order.append(name)
        for group in parse_depends(index[name].get("Depends", "")):
            pick = None
            for alt in group:
                if alt in installed:
                    pick = None
                    break
                if alt in index or alt in provides_map:
                    pick = alt
                    # prefer the first real package over a virtual provider
                    if alt in index:
                        break
            if pick:
                todo.append(pick)
    return order


def deb_extract(deb, staging):
    """unpack data.tar.* into staging, return control stanza text"""
    tmp = tempfile.mkdtemp(prefix="debx-")
    sh(["ar", "x", deb, "--outdir", tmp])
    data = None
    ctrl = None
    for f in os.listdir(tmp):
        if f.startswith("data.tar"):
            data = os.path.join(tmp, f)
        elif f.startswith("control.tar"):
            ctrl = os.path.join(tmp, f)
    if not data:
        raise SystemExit(f"no data.tar in {deb}")
    # bsdtar handles .zst/.xz/.gz transparently by extension
    sh(["tar", "-xf", data, "-C", staging, "--no-same-owner"])
    control = ""
    if ctrl:
        ctmp = tempfile.mkdtemp(prefix="debc-")
        sh(["tar", "-xf", ctrl, "-C", ctmp, "--no-same-owner"])
        for cand in ("control", "./control"):
            p = os.path.join(ctmp, cand)
            if os.path.exists(p):
                control = open(p, encoding="utf-8", errors="replace").read()
                break
    return control


def register(staging, name, version, control):
    """append an installed entry to the staging dpkg status"""
    status_path = os.path.join(staging, "var", "lib", "dpkg", "status")
    stanza = []
    stanza.append(f"Package: {name}")
    stanza.append(f"Status: install ok unpacked")
    stanza.append(f"Version: {version}")
    for line in control.splitlines():
        if line.startswith(("Package:", "Status:", "Version:")) or not line.strip():
            continue
        stanza.append(line)
    stanza.append("")
    with open(status_path, "a", encoding="utf-8") as f:
        f.write("\n".join(stanza) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", required=True)
    ap.add_argument("--seed", required=True)
    args = ap.parse_args()

    os.makedirs(DEB_CACHE, exist_ok=True)
    print("== parsing package index ==", file=sys.stderr)
    index = parse_packages()
    print(f"{len(index)} packages in index", file=sys.stderr)
    installed = parse_installed_status(args.staging)
    print(f"{len(installed)} already installed in staging", file=sys.stderr)

    seeds = args.seed.split()
    order = resolve(index, installed, seeds)
    print(f"== resolving {len(seeds)} seeds -> {len(order)} packages ==", file=sys.stderr)

    for name in order:
        pkg = index[name]
        url = f"{MIRROR}/{pkg['Filename']}"
        dest = os.path.join(DEB_CACHE, os.path.basename(pkg["Filename"]))
        print(f"fetch {name} {pkg['Version']}", file=sys.stderr)
        fetch(url, dest, pkg.get("SHA256"))
        control = deb_extract(dest, args.staging)
        register(args.staging, name, pkg["Version"], control)

    # mark everything as configured (we run no maintainer scripts)
    sp = os.path.join(args.staging, "var", "lib", "dpkg", "status")
    txt = open(sp, encoding="utf-8").read()
    txt = txt.replace("Status: install ok unpacked", "Status: install ok installed")
    open(sp, "w", encoding="utf-8").write(txt)
    print(f"== done: {len(order)} packages unpacked ==", file=sys.stderr)


if __name__ == "__main__":
    main()
