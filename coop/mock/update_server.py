#!/usr/bin/env python3
"""A stand-in for GitHub's releases/latest and its download, for the in-game
updater (port/src/port_update.c) in tests:

  coop/mock/update_server.py <zip> [--tag v0.2.2] [--port 8765] [--digest good|bad|none]

  GET /releases/latest         the release, shaped like GitHub's answer, with
                               the zip as its one asset (size, digest, url)
  GET /download/<tag>/<name>   the zip itself (slowly enough to show progress)

Point the game at it with GE_UPDATE_API=http://127.0.0.1:<port>/releases/latest.
--digest bad lists a wrong SHA-256 (the game must refuse the zip), none
lists no digest (the game must refuse to install it unchecked).
"""
import argparse
import hashlib
import http.server
import json
import os
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zip")
    ap.add_argument("--tag", default="v0.2.2")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--digest", choices=("good", "bad", "none"), default="good")
    ap.add_argument("--rate", type=int, default=2 * 1024 * 1024, help="bytes a second for the zip")
    args = ap.parse_args()

    with open(args.zip, "rb") as f:
        data = f.read()
    name = os.path.basename(args.zip)
    sha = hashlib.sha256(data).hexdigest()
    if args.digest == "bad":
        sha = "0" * 64
    base = "http://127.0.0.1:%d" % args.port
    asset = {
        "url": base + "/assets/1",
        "id": 1,
        "name": name,
        "label": "",
        "uploader": {"login": "github-actions[bot]", "id": 41898282},
        "content_type": "application/zip",
        "state": "uploaded",
        "size": len(data),
        "download_count": 0,
        "browser_download_url": "%s/download/%s/%s" % (base, args.tag, name),
    }
    if args.digest != "none":
        asset["digest"] = "sha256:" + sha
    release = {
        "url": base + "/releases/1",
        "html_url": "https://github.com/reaffirmed/goldeneye-coop/releases/tag/" + args.tag,
        "tag_name": args.tag,
        "target_commitish": "main",
        "name": "GoldenEye Co-op " + args.tag,
        "draft": False,
        "prerelease": False,
        "assets": [asset],
        "body": "Test release.\n\n- \"quoted\" \\ backslash é",
    }
    body = json.dumps(release, indent=2).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            sys.stderr.write("update_server: GET %s (%s)\n" % (self.path, self.headers.get("User-Agent")))
            if self.path == "/releases/latest":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/download/%s/%s" % (args.tag, name):
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                step = max(1, args.rate // 10)
                for i in range(0, len(data), step):
                    self.wfile.write(data[i:i + step])
                    time.sleep(0.1)
            else:
                self.send_response(404)
                self.end_headers()

        def log_message(self, *a):
            pass

    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
