#!/usr/bin/env python3
"""Stage an rx3-toolkit build into this port's runtime, beside the legacy core.

The toolkit (github.com/Tratosca/rx3-toolkit, MPL-2.0) is built for a real XDJ-RX3:
its autoexec.bin, run from the USB stick, copies librx3_core.so and the panel art
into /root/pdj, patches rbp and exports one environment variable per module. This
port launches rbp itself, so this script does the same work once, ahead of time:

  <runtime>/root/pdj/toolkit/librx3_core.so   the hook (from `make hook`)
  <runtime>/root/pdj/toolkit/rbp              our rbp + the core's image-table patch
  <runtime>/root/pdj/toolkit/*.rgb565         panel art, found through RX3_TAB_*
  <runtime>/root/pdj/rx3-glyph-atlas-*.rgb565 pad glyphs (fixed path in the core)
  <out>/toolkit.env                           the modules' exports, for the launcher

Nothing the legacy core uses is touched: its art keeps its names in /root/pdj, its
librx3_core.so and rbp stay where they are. The launcher picks one or the other
(start-rk3399.sh, RX3_CORE=toolkit). docs/rk3399/00-port.md, "rx3-toolkit".
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

# Our rbp differs from stock 1.20 at the core's image-table patch: the legacy core
# wrote its own private-ID base there (0a3601e3, movw r3,#0x160a); the toolkit's
# guard wants its value (a53601e3, movw r3,#0x16a5). Stock is cc3501e3.
IMAGE_TABLE_OFF = 1874220                      # file offset, VA 0x1d192c
IMAGE_TABLE_OK_BEFORE = (bytes.fromhex("0a3601e3"), bytes.fromhex("cc3501e3"))
IMAGE_TABLE_TOOLKIT = bytes.fromhex("a53601e3")

# (module dir, slot, asset base name): the stage_panel_asset calls of each module.sh.
PANELS = [
    ("core", "02", "status-none-selected"),
    ("keyshift", "00", "key-selected"),
    ("keyshift", "03", "none-selected"),
    ("keyshift", "07", "single-key-none"),
    ("keyshift", "08", "single-key-selected"),
    ("stems", "01", "stems-selected"),
    ("stems", "09", "single-stems-none"),
    ("stems", "10", "single-stems-selected"),
    ("samples", "04", "samples-selected"),
    ("samples", "05", "samples-none-selected"),
    ("samples", "06", "samples-beatfx-selected"),
]


def sha1(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def asset_source(toolkit, module, target):
    """The repository file a module's manifest installs under `target`."""
    man = json.load(open(os.path.join(toolkit, "mod/modules", module, "manifest.json")))
    for f in man.get("files", []):
        if f["target"] == target:
            return os.path.join(toolkit, "mod/modules", module, f["source"])
    return None


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    os.chmod(dst, 0o644)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--toolkit", required=True, help="rx3-toolkit tree with build/librx3_core.so")
    ap.add_argument("--runtime", required=True, help="the chroot (work/runtime/rx3)")
    ap.add_argument("--out", required=True, help="where toolkit.env is written")
    ap.add_argument("--stems-dir", default="/root/pdj/RX3_STEMS",
                    help="RX3_STEMS_DIR as rbp sees it (default: the runtime copy)")
    ap.add_argument("--samples-dir", default="/media/usb1/sda1/RX3_RUNTIME/samples",
                    help="RX3_SAMPLES_DIR as rbp sees it; exported only if it exists")
    ap.add_argument("--theme", default="", help="RX3_THEME letter (empty = dark, stock look)")
    ap.add_argument("--browse-field", default="13", choices=["7", "11", "13", "15"],
                    help="third BROWSE column (module default 13)")
    a = ap.parse_args()

    pdj = os.path.join(a.runtime, "root/pdj")
    tk = os.path.join(pdj, "toolkit")
    hook = os.path.join(a.toolkit, "build/librx3_core.so")
    if not os.path.isfile(hook):
        sys.exit("no %s: run `make hook` in the toolkit tree first" % hook)

    # 1. the hook
    copy(hook, os.path.join(tk, "librx3_core.so"))
    os.chmod(os.path.join(tk, "librx3_core.so"), 0o755)

    # 2. rbp with the toolkit's image-table word; every other byte is ours
    rbp = open(os.path.join(pdj, "rbp"), "rb").read()
    word = rbp[IMAGE_TABLE_OFF:IMAGE_TABLE_OFF + 4]
    if word not in IMAGE_TABLE_OK_BEFORE + (IMAGE_TABLE_TOOLKIT,):
        sys.exit("rbp @%d holds %s, not a known image-table word: refusing"
                 % (IMAGE_TABLE_OFF, word.hex()))
    patched = rbp[:IMAGE_TABLE_OFF] + IMAGE_TABLE_TOOLKIT + rbp[IMAGE_TABLE_OFF + 4:]
    out_rbp = os.path.join(tk, "rbp")
    with open(out_rbp, "wb") as f:
        f.write(patched)
    os.chmod(out_rbp, 0o755)
    diff = sum(1 for x, y in zip(rbp, patched) if x != y)

    # 3. panel art, addressed by RX3_TAB_DARK_/LIGHT_<slot>
    env = []
    for module, slot, name in PANELS:
        src = asset_source(a.toolkit, module, name + ".rgb565")
        if not src:
            sys.exit("%s: no asset %s" % (module, name))
        copy(src, os.path.join(tk, name + ".rgb565"))
        env.append(("RX3_TAB_DARK_" + slot, "/root/pdj/toolkit/%s.rgb565" % name))
        light = asset_source(a.toolkit, module, name + "-light.rgb565")
        if light:
            copy(light, os.path.join(tk, name + "-light.rgb565"))
            env.append(("RX3_TAB_LIGHT_" + slot, "/root/pdj/toolkit/%s-light.rgb565" % name))
        else:
            env.append(("RX3_TAB_LIGHT_" + slot, ""))

    # 4. pad glyphs: rx3_pad_atlas.h reads these two fixed paths
    for variant in ("dark", "light"):
        src = asset_source(a.toolkit, "core", "glyph-atlas-%s.rgb565" % variant)
        copy(src, os.path.join(pdj, "rx3-glyph-atlas-%s.rgb565" % variant))

    # 5. the modules' exports (module.sh -> module_export)
    env += [
        ("RX3_STEMS_DIR", a.stems_dir),                 # stems
        ("RX3_KEYSHIFT", "1"),                          # keyshift
        ("RX3_KEY_SYNC", "1"),                          # key-sync (needs key-match)
        ("RX3_KEY_SYNC_RANGE", "1"),
        ("RX3_KEY_SYNC_MODE", "identical"),
        ("RX3_KEY_MATCH", "1"),                         # key-match
        ("RX3_KEY_MATCH_RULES", "2"),
        ("RX3_BROWSE_COLUMNS", "1"),                    # browse-columns
        ("RX3_BROWSE_FIELD", a.browse_field),
        ("RX3_SEARCH_LATIN", "1"),                      # search-latin
        ("RX3_THEME", a.theme),                         # theme-white (empty = off)
    ]
    samples_host = os.path.join(a.runtime, a.samples_dir.lstrip("/"))
    if os.path.isdir(samples_host):
        env.append(("RX3_SAMPLES_DIR", a.samples_dir))
    os.makedirs(a.out, exist_ok=True)
    env_path = os.path.join(a.out, "toolkit.env")
    with open(env_path, "w") as f:
        f.write("# written by scripts/rk3399/toolkit/stage-toolkit.py; sourced by\n"
                "# start-rk3399.sh when RX3_CORE=toolkit. Do not edit: re-run the script.\n")
        for k, v in env:
            f.write("export %s='%s'\n" % (k, v))

    commit = ""
    cpath = os.path.join(a.toolkit, "TOOLKIT_COMMIT")
    if os.path.isfile(cpath):
        commit = open(cpath).read().strip()
    print("toolkit %s" % (commit or "(commit unknown)"))
    print("hook     %s  sha1 %s" % (os.path.join(tk, "librx3_core.so"), sha1(hook)))
    print("rbp      %s  %d byte(s) changed @%d (%s -> %s)"
          % (out_rbp, diff, IMAGE_TABLE_OFF, word.hex(), IMAGE_TABLE_TOOLKIT.hex()))
    print("art      %d panel image(s) in %s, glyph atlases in %s" % (len(PANELS), tk, pdj))
    print("env      %s (%d settings; samples %s)"
          % (env_path, len(env), "on" if os.path.isdir(samples_host) else "off: no " + a.samples_dir))


if __name__ == "__main__":
    main()
