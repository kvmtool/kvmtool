#!/usr/bin/env python3
import argparse
import os
import socket
import subprocess
import sys
import time


def checksum(data: bytes) -> bytes:
	return f"#{sum(data) & 0xff:02x}".encode()


class RspClient:
	def __init__(self, sock: socket.socket):
		self.sock = sock

	def _read_exact(self, length: int) -> bytes:
		buf = bytearray()
		while len(buf) < length:
			chunk = self.sock.recv(length - len(buf))
			if not chunk:
				raise RuntimeError("unexpected EOF")
			buf.extend(chunk)
		return bytes(buf)

	def send_packet(self, payload: bytes) -> None:
		self.sock.sendall(b"$" + payload + checksum(payload))
		ack = self._read_exact(1)
		if ack != b"+":
			raise RuntimeError(f"unexpected ack: {ack!r}")

	def recv_packet(self) -> bytes:
		while True:
			ch = self._read_exact(1)
			if ch == b"$":
				break
			if ch in (b"+", b"-"):
				continue
			raise RuntimeError(f"unexpected prefix byte: {ch!r}")

		payload = bytearray()
		while True:
			ch = self._read_exact(1)
			if ch == b"#":
				break
			payload.extend(ch)

		got = self._read_exact(2)
		expected = f"{sum(payload) & 0xff:02x}".encode()
		if got.lower() != expected:
			self.sock.sendall(b"-")
			raise RuntimeError(
				f"checksum mismatch: got {got!r}, expected {expected!r}"
			)

		self.sock.sendall(b"+")
		return bytes(payload)


def escape_binary(data: bytes) -> bytes:
	out = bytearray()
	for value in data:
		if value in (ord("#"), ord("$"), ord("}"), ord("*")):
			out.append(ord("}"))
			out.append(value ^ 0x20)
		else:
			out.append(value)
	return bytes(out)


def wait_for_port(port: int, timeout: float) -> socket.socket:
	deadline = time.time() + timeout
	last_error = None
	while time.time() < deadline:
		try:
			sock = socket.create_connection(("127.0.0.1", port), timeout=1)
			sock.settimeout(5)
			return sock
		except OSError as exc:
			last_error = exc
			time.sleep(0.1)
	raise RuntimeError(f"failed to connect to GDB stub: {last_error}")


def stop_process(proc: subprocess.Popen) -> None:
	if proc.poll() is not None:
		return
	proc.terminate()
	try:
		proc.wait(timeout=5)
	except subprocess.TimeoutExpired:
		proc.kill()
		proc.wait(timeout=5)


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--lkvm", required=True)
	parser.add_argument("--guest", required=True)
	parser.add_argument("--port", type=int, required=True)
	args = parser.parse_args()

	if not os.path.exists("/dev/kvm"):
		print("SKIP: /dev/kvm is unavailable")
		return 0

	proc = subprocess.Popen(
		[
			os.path.abspath(args.lkvm),
			"run",
			"--gdb",
			str(args.port),
			"--gdb-wait",
			os.path.abspath(args.guest),
		],
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
	)

	try:
		sock = wait_for_port(args.port, 10)
		client = RspClient(sock)

		client.send_packet(b"qSupported:multiprocess+")
		reply = client.recv_packet().decode()
		assert "PacketSize=" in reply
		assert "qXfer:features:read+" in reply

		client.send_packet(b"?")
		reply = client.recv_packet().decode()
		assert reply.startswith("T")

		client.send_packet(b"qXfer:features:read:target.xml:0,80")
		reply = client.recv_packet().decode()
		assert reply[0] in ("m", "l")
		assert "<target" in reply[1:]

		client.send_packet(b"g")
		reply = client.recv_packet().decode()
		assert len(reply) > 32
		assert len(reply) % 2 == 0
		regs = bytes.fromhex(reply)
		rip = int.from_bytes(regs[16 * 8:16 * 8 + 8], "little")

		client.send_packet(f"Z0,{rip:x},1".encode())
		reply = client.recv_packet().decode()
		assert reply == "OK"

		client.send_packet(f"z0,{rip:x},1".encode())
		reply = client.recv_packet().decode()
		assert reply == "OK"

		payload = bytes([0x23, 0x24, 0x7D, 0x2A, 0x55])
		addr = 0x200000
		binary = escape_binary(payload)
		client.send_packet(
			f"X{addr:x},{len(payload):x}:".encode() + binary
		)
		reply = client.recv_packet().decode()
		assert reply == "OK"

		client.send_packet(f"m{addr:x},{len(payload):x}".encode())
		reply = client.recv_packet().decode()
		assert reply == payload.hex()

		client.send_packet(b"D")
		reply = client.recv_packet().decode()
		assert reply == "OK"
		sock.close()
		print("PASS: x86 GDB stub smoke test")
		return 0
	finally:
		stop_process(proc)


if __name__ == "__main__":
	sys.exit(main())
