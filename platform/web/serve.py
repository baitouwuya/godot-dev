#!/usr/bin/env python3

import argparse
import contextlib
import os
import re
import shutil
import socket
import subprocess
import sys
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RANGE_HEADER_RE = re.compile(r"^bytes=(\d*)-(\d*)$")


# See cpython GH-17851 and GH-17864.
class DualStackServer(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # Suppress exception when protocol is IPv4.
        with contextlib.suppress(Exception):
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        return super().server_bind()


class CORSRequestHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    _byte_range = None

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range, ETag")
        super().end_headers()

    @staticmethod
    def _make_etag(fs):
        return f'"{fs.st_mtime_ns:x}-{fs.st_size:x}"'

    def _send_full_file(self, f, fs, ctype, etag):
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-type", ctype)
        self.send_header("Content-Length", str(fs.st_size))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", etag)
        self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
        self.end_headers()
        return f

    def send_head(self):
        self._byte_range = None
        range_header = self.headers.get("Range")
        if not range_header:
            path = self.translate_path(self.path)
            if os.path.isfile(path):
                ctype = self.guess_type(path)
                try:
                    f = open(path, "rb")
                except OSError:
                    self.send_error(HTTPStatus.NOT_FOUND, "File not found")
                    return None
                try:
                    fs = os.fstat(f.fileno())
                    etag = self._make_etag(fs)
                    return self._send_full_file(f, fs, ctype, etag)
                except Exception:
                    f.close()
                    raise
            return super().send_head()

        path = self.translate_path(self.path)
        if os.path.isdir(path):
            return super().send_head()
        ctype = self.guess_type(path)
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND, "File not found")
            return None

        try:
            fs = os.fstat(f.fileno())
            etag = self._make_etag(fs)
            if_range = self.headers.get("If-Range")
            if if_range and if_range.strip() != etag:
                return self._send_full_file(f, fs, ctype, etag)

            size = fs.st_size
            match = RANGE_HEADER_RE.match(range_header.strip())
            if not match or (not match.group(1) and not match.group(2)):
                f.close()
                self.send_error(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE, "Invalid Range")
                return None

            start_str, end_str = match.group(1), match.group(2)
            if start_str:
                start = int(start_str)
                end = int(end_str) if end_str else size - 1
            else:
                suffix = int(end_str)
                if suffix <= 0:
                    f.close()
                    self.send_error(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE, "Invalid suffix length")
                    return None
                start = max(0, size - suffix)
                end = size - 1

            if size == 0 or start >= size or start > end:
                f.close()
                self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.send_header("ETag", etag)
                self.end_headers()
                return None

            end = min(end, size - 1)
            length = end - start + 1
            f.seek(start)
            self._byte_range = (start, length)

            self.send_response(HTTPStatus.PARTIAL_CONTENT)
            self.send_header("Content-type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(length))
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
            self.end_headers()
            return f
        except Exception:
            f.close()
            raise

    def copyfile(self, source, outputfile):
        with contextlib.suppress(BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            if self._byte_range is None:
                shutil.copyfileobj(source, outputfile, length=65536)
                return
            _, remaining = self._byte_range
            while remaining > 0:
                chunk = source.read(min(65536, remaining))
                if not chunk:
                    break
                outputfile.write(chunk)
                remaining -= len(chunk)


def shell_open(url):
    if sys.platform == "win32":
        os.startfile(url)
    else:
        opener = "open" if sys.platform == "darwin" else "xdg-open"
        subprocess.call([opener, url])


def serve(root, port, run_browser):
    os.chdir(root)

    address = ("", port)
    httpd = DualStackServer(address, CORSRequestHandler)

    url = f"http://127.0.0.1:{port}"
    if run_browser:
        # Open the served page in the user's default browser.
        print(f"Opening the served URL in the default browser (use `--no-browser` or `-n` to disable this): {url}")
        shell_open(url)
    else:
        print(f"Serving at: {url}")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nKeyboard interrupt received, stopping server.")
    finally:
        # Clean-up server
        httpd.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", help="port to listen on", default=8060, type=int)
    parser.add_argument(
        "-r", "--root", help="path to serve as root (relative to `platform/web/`)", default="../../bin", type=Path
    )
    browser_parser = parser.add_mutually_exclusive_group(required=False)
    browser_parser.add_argument(
        "-n", "--no-browser", help="don't open default web browser automatically", dest="browser", action="store_false"
    )
    parser.set_defaults(browser=True)
    args = parser.parse_args()

    # Change to the directory where the script is located,
    # so that the script can be run from any location.
    os.chdir(Path(__file__).resolve().parent)

    serve(args.root, args.port, args.browser)
