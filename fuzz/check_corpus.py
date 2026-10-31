#!/usr/bin/env python3
"""Differential check of the control line parser and the endpoint file reader against a reference written in Python.

The libFuzzer harnesses check memory safety and invariants; they cannot say whether a line *should* have been accepted.
This script can: it implements the rules of CONTRACT.md section 8 ("Syntax of a line", the field tables) and 6.2 (the
endpoint file) a second time on top of Python's json module (somebody else's JSON reader), and compares the verdict of the
library, via fuzz/build/verdict, for

  * every vector of tests/vectors/link (this validates the reference itself first),
  * every file of the corpus directories given on the command line (the inputs libFuzzer grew),
  * a large number of cases generated here by mutating the vectors (structure aware: numbers, escapes, nesting, duplicate keys).

    python3 fuzz/check_corpus.py [--cases N] [--seed S] [CORPUS_DIR ...]

Exit status 1 and a list of disagreements (hex of the case, both verdicts) if the library and the reference differ.
The rules the reference applies are exactly those stated in tests/vectors/link/README.md, GAPS.md and the vectors; where
those are silent the reference has no rule, and the case is not compared (it is counted as "undecided").
"""
import argparse
import json
import os
import random
import subprocess
import sys

sys.set_int_max_str_digits(0)  # a 5000 digit integer is valid JSON

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "..", "tests", "vectors", "link")
VERDICT = os.path.join(HERE, "build", "verdict")

MAX_LINE = 65536
MAX_NEST = 127


class Reject(Exception):
    pass


class Flt(float):
    """A JSON number that is not an unsigned integer literal (fraction, exponent, or -0)."""


def _parse_int(s):
    if s == "-0":
        return Flt(0.0)
    return int(s)


def _parse_float(s):
    v = float(s)
    if v in (float("inf"), float("-inf")):
        raise Reject("number out of range")
    return Flt(v)


def _bad_const(s):
    raise Reject("constant " + s)


def _check_strings(v, depth=0):
    if depth > MAX_NEST + 2:
        raise Reject("nesting")
    if isinstance(v, str):
        for ch in v:
            o = ord(ch)
            if o == 0:
                raise Reject("NUL")
            if 0xD800 <= o <= 0xDFFF:
                raise Reject("lone surrogate")
    elif isinstance(v, list):
        for x in v:
            _check_strings(x, depth + 1)
    elif isinstance(v, dict):
        for k, x in v.items():
            _check_strings(k, depth + 1)
            _check_strings(x, depth + 1)


def _hook(pairs):
    for k, v in pairs:  # the overridden values have to be valid too: look at every pair
        _check_strings(k)
        _check_strings(v)
    return dict(pairs)


def nesting_levels(text):
    depth = best = 0
    in_str = esc = False
    for ch in text:
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch in "[{":
            depth += 1
            best = max(best, depth)
        elif ch in "]}":
            depth -= 1
    return best


def json_object(raw):
    """raw: bytes of one JSON text. Returns the dict, or raises Reject."""
    try:
        text = raw.decode("utf-8")  # strict: no surrogates, no overlong forms
    except UnicodeDecodeError:
        raise Reject("not UTF-8")
    if text.startswith("﻿"):
        raise Reject("BOM")
    # JSON whitespace is space, tab, LF, CR: Python's decoder skips exactly those
    if nesting_levels(text) > MAX_NEST:
        raise Reject("nesting")
    try:
        v = json.loads(text, parse_int=_parse_int, parse_float=_parse_float, parse_constant=_bad_const, object_pairs_hook=_hook)
    except Reject:
        raise
    except (ValueError, RecursionError):
        raise Reject("syntax")
    if not isinstance(v, dict):
        raise Reject("not an object")
    return v


def is_uint(v, bits):
    return type(v) is int and 0 <= v < (1 << bits)


def need_u(o, k, bits):
    if k not in o or not is_uint(o[k], bits):
        raise Reject("field " + k)
    return o[k]


def need_s(o, k):
    if k not in o or type(o[k]) is not str:
        raise Reject("field " + k)
    return o[k]


def opt_s(o, k):
    if k not in o or o[k] is None:
        return None
    if type(o[k]) is not str:
        raise Reject("field " + k)
    return o[k]


def role(o, k):
    if k not in o or o[k] not in ("host", "guest") or type(o[k]) is not str:
        raise Reject("role")
    return o[k]


def ref_control(line):
    """-> message dict (strings as hex of UTF-8), or None for a refused line. line ends with its '\\n'."""
    h = lambda s: s.encode("utf-8").hex()
    try:
        if len(line) > MAX_LINE:
            raise Reject("too long")
        body = line
        while body and body[-1:] in (b"\n", b"\r"):
            body = body[:-1]
        if len(body) >= MAX_LINE:
            raise Reject("too long")
        o = json_object(body)
        t = o.get("type")
        if type(t) is not str:
            raise Reject("type")
        if t == "hello":
            m = {"type": "hello", "role": role(o, "role"), "game": h(need_s(o, "game")),
                 "protocol": need_u(o, "protocol", 16), "min_protocol": need_u(o, "min_protocol", 16),
                 "udp_port": need_u(o, "udp_port", 16), "heartbeat_ms": need_u(o, "heartbeat_ms", 32),
                 "timeout_ms": need_u(o, "timeout_ms", 32)}
            if o.get("session") is not None:
                m["session"] = need_u(o, "session", 32)
            tok = opt_s(o, "token")
            if tok is not None:
                m["token"] = h(tok)
            return m
        if t == "spawn":
            m = {"type": "spawn", "entity": need_u(o, "entity", 32), "provider": h(need_s(o, "provider")), "asset": h(need_s(o, "asset"))}
            nm = opt_s(o, "name")
            if nm is not None:
                m["name"] = h(nm)
            return m
        if t == "despawn":
            return {"type": "despawn", "entity": need_u(o, "entity", 32)}
        if t == "input":
            m = {"type": "input", "focus": role(o, "focus")}
            r = opt_s(o, "reason")
            if r is not None:
                m["reason"] = h(r)
            return m
        if t == "frame_channel":
            return {"type": "frame_channel", "path": h(need_s(o, "path")), "width": need_u(o, "width", 32),
                    "height": need_u(o, "height", 32), "slots": need_u(o, "slots", 32)}
        if t == "bye":
            r = opt_s(o, "reason")
            return {"type": "bye", "reason": h(r or "")}
        return {"type": "unknown", "unknown_type": h(t)}
    except Reject:
        return None


def ref_endpoint(raw):
    try:
        if len(raw) > 4096:
            raise Reject("size")
        body = raw
        if body.endswith(b"\n"):
            body = body[:-1]
            if body.endswith(b"\r"):
                body = body[:-1]
        if b"\n" in body:
            raise Reject("newline")
        o = json_object(body)
        port = o.get("control_port")
        tok = o.get("token")
        if not is_uint(port, 16) or port < 1:
            raise Reject("port")
        if type(tok) is not str or not (1 <= len(tok.encode("utf-8")) <= 256):
            raise Reject("token")
        return {"port": port, "token": tok.encode("utf-8").hex()}
    except Reject:
        return None


# ---------------------------------------------------------------------------------------------- running the library

class Verdict:
    def __init__(self, mode):
        self.p = subprocess.Popen([VERDICT, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def ask(self, b):
        self.p.stdin.write(b.hex() + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        return r

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def control_cases_from_vectors():
    with open(os.path.join(VEC, "control.json"), encoding="ascii") as f:
        for c in json.load(f)["cases"]:
            b = bytes.fromhex(c["line_hex"]) if "line_hex" in c else c["line"].encode("utf-8", "surrogatepass")
            yield c["id"], b + b"\n", c["expect"]


def endpoint_cases_from_vectors():
    with open(os.path.join(VEC, "endpoints.json"), encoding="ascii") as f:
        for c in json.load(f)["cases"]:
            if "text" in c:
                b = c["text"].encode("utf-8", "surrogatepass")
            elif "text_hex" in c:
                b = bytes.fromhex(c["text_hex"])
            else:
                continue
            if c.get("mode", "0600") != "0600" and c.get("mode") != "0400":
                continue
            yield c["id"], b, c["expect"]


def same_control(lib, ref):
    if ref is None:
        return not lib["ok"]
    if not lib["ok"]:
        return False
    return lib["msg"] == ref


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("corpus", nargs="*")
    a = ap.parse_args()
    if not os.path.exists(VERDICT):
        sys.exit("build fuzz/build/verdict first: make -C fuzz build/verdict")
    bad = []

    # 1. the reference against the vectors
    vc = Verdict("control")
    for cid, b, expect in control_cases_from_vectors():
        ref = ref_control(b)
        want_reject = "reject" in expect if isinstance(expect, dict) else expect == "reject"
        if (ref is None) != want_reject:
            print("REFERENCE DISAGREES WITH VECTOR", cid, ref)
            return 2
        lib = vc.ask(b)
        if not same_control(lib, ref):
            bad.append(("vector " + cid, b, lib, ref))
    ve = Verdict("endpoint")
    for cid, b, expect in endpoint_cases_from_vectors():
        ref = ref_endpoint(b)
        if (ref is not None) != (expect == "ok" or (isinstance(expect, dict) and "ok" in expect) or (isinstance(expect, str) and expect.startswith("ok"))):
            print("REFERENCE DISAGREES WITH ENDPOINT VECTOR", cid, ref)
            return 2

    # 2. the corpora grown by the fuzzers
    n_corpus = 0
    for d in a.corpus:
        for name in sorted(os.listdir(d)):
            with open(os.path.join(d, name), "rb") as f:
                b = f.read()
            n_corpus += 1
            if "fuzz_endpoint" in d:
                if len(b) < 1:
                    continue
                body = b[1:]
                ref = ref_endpoint(body)
                lib = ve.ask(body)
                if (ref is None) != (not lib["ok"]) or (ref is not None and (lib["port"] != ref["port"] or lib["token"] != ref["token"])):
                    bad.append(("corpus " + name, body, lib, ref))
            else:
                k = b.find(b"\n")
                line = b + b"\n" if k < 0 else b[: k + 1]
                ref = ref_control(line)
                lib = vc.ask(line)
                if not same_control(lib, ref):
                    bad.append(("corpus " + name, line, lib, ref))

    # 3. generated cases
    rnd = random.Random(a.seed)
    seeds = [b[:-1] for _, b, _ in control_cases_from_vectors() if b"\n" not in b[:-1]]
    eseeds = [b for _, b, _ in endpoint_cases_from_vectors() if len(b) < 600]
    alphabet = [b"0", b"1", b"-", b"+", b".", b"e", b"E", b"\\u0000", b"\\ud800", b"\\udc00", b"\\ud83d\\ude00", b"\\n", b"\\\\", b"\\/",
                b'"', b",", b":", b"{", b"}", b"[", b"]", b" ", b"\t", b"\r", b"\x0b", b"\x0c", b"\xc2\xa0", b"\xef\xbb\xbf", b"null", b"true",
                b"false", b"1e400", b"1E+2", b"7.0", b"-0", b"01", b"4294967295", b"4294967296", b"65535", b"65536", b"18446744073709551615",
                b"18446744073709551616", b'"type"', b'"host"', b'"guest"', b'"token"', b'"reason"', b'"name"', b'"session"', b"\xc3\xa9", b"\xff"]
    done = 0
    undec = 0
    for i in range(a.cases):
        base = rnd.choice(seeds)
        b = bytearray(base)
        for _ in range(rnd.randint(1, 3)):
            op = rnd.randrange(6)
            pos = rnd.randrange(len(b) + 1)
            if op == 0:
                b[pos:pos] = rnd.choice(alphabet)
            elif op == 1 and b:
                del b[min(pos, len(b) - 1): min(pos, len(b) - 1) + rnd.randint(1, 3)]
            elif op == 2 and b:
                p = min(pos, len(b) - 1)
                b[p: p + rnd.randint(1, 4)] = rnd.choice(alphabet)
            elif op == 3:
                depth = rnd.choice([1, 2, 126, 127, 128, 129, 200])
                b[pos:pos] = b'"x":' + b"[" * depth + b"]" * depth + b","
            elif op == 4:
                # duplicate a key: copy a "k":v pair somewhere
                s = bytes(b)
                j = s.find(b'":')
                if j > 0:
                    st = s.rfind(b'"', 0, j)
                    en = s.find(b",", j)
                    if st >= 0 and en > 0:
                        b[pos:pos] = s[st: en + 1]
            else:
                b[pos:pos] = b" " * rnd.randint(1, 3)
        line = bytes(b)
        if b"\n" in line:
            line = line.split(b"\n")[0]
        line += b"\n"
        ref = ref_control(line)
        lib = vc.ask(line)
        done += 1
        if not same_control(lib, ref):
            bad.append(("generated", line, lib, ref))
        if i % 4 == 0 and eseeds:
            e = bytearray(rnd.choice(eseeds))
            pos = rnd.randrange(len(e) + 1)
            e[pos:pos] = rnd.choice(alphabet)
            e = bytes(e)
            ref = ref_endpoint(e)
            lib = ve.ask(e)
            if (ref is None) != (not lib["ok"]) or (ref is not None and (lib["port"] != ref["port"] or lib["token"] != ref["token"])):
                bad.append(("generated endpoint", e, lib, ref))
    # 4. numbers: a grammar of JSON numbers (valid and not), alone in an unknown field and in the integer fields
    limit = str(2**1024 - 2**970)

    def rand_number():
        r = rnd.random()
        if r < 0.35:  # around the limit of the doubles, written in the different ways the grammar allows
            d = list(limit)
            k = rnd.randrange(len(d))
            if rnd.random() < 0.7:
                d[k] = str(rnd.randrange(10)) if rnd.random() < 0.3 else d[k]
            if rnd.random() < 0.5:
                d = d[: rnd.randint(1, len(d))]
            digits = "".join(d)
            if rnd.random() < 0.5:
                digits += "0" * rnd.randint(0, 3) + rnd.choice(["", "1", "9"])
            point = rnd.randint(0, len(digits))
            exp = 308 - (point - 1) if point else 309
            m = digits[:point] + ("." + digits[point:] if point < len(digits) else "")
            if point == 0:
                m = "0." + digits
            e = exp + rnd.randint(-1, 1) if rnd.random() < 0.8 else rnd.randint(-5, 5)
            return ("-" if rnd.random() < 0.2 else "") + m + rnd.choice("eE") + rnd.choice(["", "+", "-"]) .replace("-", "" if e >= 0 else "-") + str(abs(e)).rjust(rnd.randint(1, 8), "0" if rnd.random() < 0.2 else "1")
        ip = rnd.choice(["0", "00", "7", "10", "123456789012345678901234567890", str(rnd.randrange(1 << rnd.randint(1, 70))), ""])
        fp = rnd.choice(["", ".", ".0", ".5", ".000000000000000000000000000000000001", "." + "9" * rnd.randint(0, 40)])
        ep = rnd.choice(["", "e0", "e5", "E+5", "e-5", "e308", "e309", "e-400", "e400", "e99999999999999999999", "e-99999999999999999999", "e", "e+", "e--1"])
        return rnd.choice(["", "-", "-", "+"]) + ip + fp + ep

    for i in range(a.cases // 4):
        num = rand_number()
        field = rnd.choice(["x", "x", "entity", "protocol", "session"])
        if field == "x":
            line = ('{"type":"bye","x":%s}\n' % num).encode()
        elif field == "entity":
            line = ('{"type":"despawn","entity":%s}\n' % num).encode()
        elif field == "protocol":
            line = ('{"type":"hello","role":"host","game":"g","protocol":%s,"min_protocol":1,"udp_port":1,"heartbeat_ms":250,"timeout_ms":1}\n' % num).encode()
        else:
            line = ('{"type":"hello","role":"host","game":"g","protocol":1,"min_protocol":1,"udp_port":1,"heartbeat_ms":250,"timeout_ms":1,"session":%s}\n' % num).encode()
        ref = ref_control(line)
        lib = vc.ask(line)
        done += 1
        if not same_control(lib, ref):
            bad.append(("generated number", line, lib, ref))
    vc.close()
    ve.close()
    print("compared: %d vectors-and-corpus files %d, generated control lines %d, undecided %d" % (len(seeds), n_corpus, done, undec))
    seen = set()
    for what, b, lib, ref in bad:
        key = (what.split(" ")[0], b)
        if key in seen:
            continue
        seen.add(key)
        print("DISAGREEMENT %s\n  case %r\n  hex %s\n  library   %s\n  reference %s" % (what, b[:300], b.hex()[:600], lib, ref))
    print("%d disagreement(s)" % len(seen))
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
