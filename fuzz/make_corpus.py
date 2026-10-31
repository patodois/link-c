#!/usr/bin/env python3
"""Writes the seed corpus of every harness from the shared vectors in tests/vectors/link (synthetic data only).

    python3 fuzz/make_corpus.py OUTDIR      ->  OUTDIR/<target>/<seed files>
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "..", "tests", "vectors", "link")


def load(name):
    with open(os.path.join(VEC, name), encoding="ascii") as f:
        return json.load(f)["cases"]


def put(out, target, name, data):
    d = os.path.join(out, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name.replace("/", "_")), "wb") as f:
        f.write(data)


def subst(s):
    """The session vectors' variables, with fixed values."""
    for k, v in (("${peer_udp_port}", "40000"), ("${ut_udp_port}", "40001"), ("${session}", "4660")):
        s = s.replace(k, v)
    return s


def main(out):
    # datagrams -> fuzz_wire (the datagram itself) and fuzz_udp (records of: flags, length, bytes)
    seq = b""
    for c in load("datagrams.json"):
        b = bytes.fromhex(c["hex"])
        put(out, "fuzz_wire", c["id"], b)
        if len(b) <= 0xFFFF:
            rec = bytes([1 | 2]) + struct.pack("<H", len(b)) + b
            put(out, "fuzz_udp", c["id"], b"\x00" + rec)
            if c["expect"] in ("decode", "accept", "ignore") and len(seq) < 60000:
                seq += rec
    put(out, "fuzz_udp", "all_valid_in_a_row", b"\x00" + seq)
    put(out, "fuzz_udp", "all_valid_in_a_row_small_limit", b"\x01" + seq)

    # control lines -> fuzz_control (the line), fuzz_session (set-up byte + stream)
    for c in load("control.json"):
        b = bytes.fromhex(c["line_hex"]) if "line_hex" in c else c["line"].encode("utf-8", "surrogatepass")
        put(out, "fuzz_control", c["id"], b)
        put(out, "fuzz_control", c["id"] + ".nl", b + b"\n")
        for setup in (0, 4, 2, 6):
            put(out, "fuzz_session", "%s.%d" % (c["id"], setup), bytes([setup, 255 if setup == 0 else 1]) + b + b"\n")

    # whole scripted sessions: what the raw peer writes on the control channel, per case
    for c in load("sessions.json"):
        role = c["role_under_test"]
        stream = b""
        for st in c["steps"]:
            if st["op"] == "send_line":
                stream += subst(st["text"]).encode("utf-8", "surrogatepass")
            elif st["op"] == "send_hex":
                stream += bytes.fromhex(st["hex"])
            elif st["op"] == "send_range":
                for i in range(st["from"], min(st["to"], st["from"] + 300) + 1):
                    stream += subst(st["template"]).replace("${i}", str(i)).encode("utf-8", "surrogatepass")
            elif st["op"] == "send_fill":
                if st["count"] <= 70000:
                    stream += (st.get("prefix", "") + st["fill"] * st["count"] + st.get("suffix", "")).encode("utf-8", "surrogatepass")
        if not stream or len(stream) > 400000:
            continue
        token = "token" in c.get("config", {})
        setup = (2 if role == "guest" else 0) | (1 if token else 0)
        put(out, "fuzz_session", c["id"], bytes([setup, 12]) + stream)
        put(out, "fuzz_session", c["id"] + ".live", bytes([setup | 4, 40]) + stream)

    # endpoint files -> fuzz_endpoint (a mode selector byte, then the file); selector 0 is mode 0600
    modes = {"0600": 0, "0400": 1, "0644": 2, "0640": 3, "0602": 4}
    for c in load("endpoints.json"):
        if "text" in c:
            b = c["text"].encode("utf-8", "surrogatepass")
        elif "text_hex" in c:
            b = bytes.fromhex(c["text_hex"])
        else:
            continue
        put(out, "fuzz_endpoint", c["id"], bytes([modes.get(c.get("mode", "0600"), 0)]) + b)

    # pixel files -> fuzz_pixel (the whole file)
    pdir = os.path.join(VEC, "pixelfiles")
    for n in sorted(os.listdir(pdir)):
        with open(os.path.join(pdir, n), "rb") as f:
            put(out, "fuzz_pixel", n, f.read())
    for c in load("pixelfiles.json"):
        r = c.get("recipe")
        if r and r["length"] <= 1 << 20:
            b = bytearray(r["length"])
            h = bytes.fromhex(r["header_hex"])
            b[: len(h)] = h
            put(out, "fuzz_pixel", c["id"], bytes(b))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
