#!/usr/bin/env python3
"""Convert Qualcomm vendor DSI command blobs (from thor.dts) into
   mipi_dsi_dcs_write_seq_multi() C code, following the same packet format
   as lmdpdg: [dtype][ctrl][vc][flags][wait][dlen_hi][dlen_lo][payload]."""
import re
import sys

DCS_NAMES = {
    0x51: "mipi_dsi_dcs_set_display_brightness_multi",
    0x11: "mipi_dsi_dcs_exit_sleep_mode_multi",
    0x29: "mipi_dsi_dcs_set_display_on_multi",
    0x28: "mipi_dcs_set_display_off_multi",
    0x10: "mipi_dsi_dcs_enter_sleep_mode_multi",
    0x2a: "mipi_dsi_dcs_set_column_address_multi",
    0x2b: "mipi_dsi_dcs_set_page_address_multi",
    0x35: "mipi_dsi_dcs_set_tear_on_multi",
}

def parse(blob):
    cmds = []
    i = 0
    while i + 7 <= len(blob):
        dtype, ctrl, vc, flags, wait = blob[i:i+5]
        dlen = (blob[i+5] << 8) | blob[i+6]
        i += 7
        payload = blob[i:i+dlen]
        i += dlen
        cmds.append((dtype, wait, payload))
    assert i == len(blob), f"trailing bytes: {len(blob)-i}"
    return cmds

def emit(cmds, indent="\t"):
    out = []
    for dtype, wait, payload in cmds:
        if dtype in (0x05, 0x15) and payload and payload[0] in DCS_NAMES:
            name = DCS_NAMES[payload[0]]
            if name.endswith("brightness_multi"):
                out.append(f"{indent}{name}(&dsi_ctx, 0x{payload[1]:02x}{payload[2]:02x} if len(payload)>2 else 0);")
            elif name.endswith("column_address_multi") or name.endswith("page_address_multi"):
                out.append(f"{indent}{name}(&dsi_ctx, 0x{(payload[1]<<8)|payload[2]:04x}, 0x{(payload[3]<<8)|payload[4]:04x});")
            elif name.endswith("tear_on_multi"):
                out.append(f"{indent}{name}(&dsi_ctx, 0x{payload[1]:02x});")
            else:
                out.append(f"{indent}{name}(&dsi_ctx);")
        elif dtype in (0x39, 0x15, 0x05):
            cmd = payload[0]
            args = payload[1:]
            line = f"{indent}mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x{cmd:02x}"
            for a in args:
                line += f", 0x{a:02x}"
            line += ");"
            out.append(line)
        else:
            out.append(f"{indent}/* dtype 0x{dtype:02x} payload {payload.hex()} */")
        if wait:
            out.append(f"{indent}mipi_dsi_msleep(&dsi_ctx, {wait});")
    return "\n".join(out)

def get_blob(dts_path, node_marker, prop):
    text = open(dts_path).read()
    # find node region
    n = text.index(node_marker)
    # from there, find the prop (byte array) - may span lines until ']'
    m = re.search(re.escape(prop) + r"\s*=\s*\[((?:\s*[0-9a-fx]+\s*)+)\]", text[n:], re.S)
    if not m:
        m = re.search(re.escape(prop) + r"\s*=\s*<((?:[^>;]+))>", text[n:], re.S)
        if not m:
            return None
        words = re.findall(r"0x[0-9a-f]+|\b\d+\b", m.group(1))
        vals = [int(w, 0) for w in words]
        blob = b"".join(int(v).to_bytes(4, "big") for v in vals)
        return blob
    toks = re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", m.group(1))
    return bytes(int(t, 0) & 0xFF for t in toks)

if __name__ == "__main__":
    dts = sys.argv[1]
    node = "qcom,mdss_dsi_l1_38_0c_0a_dsc_cmd"
    # restrict to the l1 node body: until the next top-level panel node
    for prop, label in [
        ("qcom,mdss-dsi-on-command", None),
        ("qcom,mdss-dsi-off-command", None),
    ]:
        # find within node region
        pass
    # simpler: caller passes prop name; we locate the occurrence inside the node
    prop = sys.argv[2]
    text = open(dts).read()
    start = text.index(node + " {")
    end = text.find("qcom,mdss_dsi_", start + 10)
    region = text[start:end]
    m = re.search(re.escape(prop) + r"\s*=\s*\[((?:\s*[0-9a-fx]+\s*)+)\]", region, re.S)
    if not m:
        print(f"// {prop}: NOT FOUND or is u32 array", file=sys.stderr)
        sys.exit(1)
    toks = re.findall(r"[0-9a-fA-F]+", m.group(1).replace("0x", ""))
    blob = bytes(int(t, 16) & 0xFF for t in toks)
    cmds = parse(blob)
    print(f"/* {prop}: {len(cmds)} commands, {len(blob)} bytes */")
    print(emit(cmds))
