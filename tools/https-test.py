#!/usr/bin/env python3
"""Exercise the standalone libcurl HTTPS transport against a local test CA."""

import hashlib
import http.server
import os
import pathlib
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/big":
            body = b"x" * 128
        elif self.path == "/large":
            body = b"x" * (4 * 1024 * 1024)
        elif self.path == "/torrent":
            info = (b"d6:lengthi1e4:name8:test.bin12:piece lengthi16384e6:pieces20:" +
                    hashlib.sha1(b"x").digest() + b"e")
            body = b"d4:info" + info + b"e"
        elif self.path == "/ok":
            body = b"p2play-test-poster"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format, *_args):
        pass


def run(command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: https-test.py HTTPS_TEST_BINARY")
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise SystemExit(f"HTTPS test binary does not exist: {binary}")
    if shutil.which("openssl") is None:
        raise SystemExit("openssl is required for the local HTTPS fixture")

    with tempfile.TemporaryDirectory(prefix="p2play-https-test-") as directory:
        root = pathlib.Path(directory)
        ca_key, ca_cert = root / "ca.key", root / "ca.pem"
        server_key, server_csr, server_cert = root / "server.key", root / "server.csr", root / "server.pem"
        extensions = root / "server.ext"
        extensions.write_text(
            "basicConstraints=CA:FALSE\n"
            "keyUsage=digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\n"
            "subjectAltName=DNS:localhost\n",
            encoding="ascii",
        )
        run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(ca_key), "-out", str(ca_cert), "-subj", "/CN=P2Play HTTPS Test CA",
        ])
        run([
            "openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(server_key), "-out", str(server_csr), "-subj", "/CN=localhost",
        ])
        run([
            "openssl", "x509", "-req", "-in", str(server_csr), "-CA", str(ca_cert),
            "-CAkey", str(ca_key), "-CAcreateserial", "-out", str(server_cert),
            "-days", "1", "-sha256", "-extfile", str(extensions),
        ])

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(server_cert), str(server_key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([str(binary), str(server.server_port), str(ca_cert)], check=True)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    main()
