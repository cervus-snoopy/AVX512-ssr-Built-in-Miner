#!/usr/bin/env python3
"""Verifikasi vektor uji AVX512 SSr lawan keccak Python.

Pemakaian:
    python3 verify.py
        Memeriksa vektor bawaan (prefix cst ronde 8 PodMint).
    python3 verify.py <prefix> <nonce> <expected_hash>
        Memeriksa satu vektor bebas.

Butuh pycryptodome: pip install pycryptodome
"""

import sys

try:
    from Crypto.Hash import keccak
except ImportError:
    print("pycryptodome belum terpasang. Jalankan: pip install pycryptodome")
    sys.exit(2)


def keccak_hex(message: str) -> str:
    k = keccak.new(digest_bits=256)
    k.update(message.encode("ascii"))
    return "0x" + k.hexdigest()


def leading_zero_bits(hex_hash: str) -> int:
    n = int(hex_hash, 16)
    if n == 0:
        return 256
    return 256 - n.bit_length()


CST_PREFIX = (
    "0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635"
    ":2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:"
)

VECTORS = [
    (
        "vektor 1 (cst + 1234567890)",
        CST_PREFIX,
        "1234567890",
        "0xea31438faa4527573d6136dc2c273f6c0951c6e70ce275f30fd069973471fa24",
    ),
    (
        "vektor 3 (baton cst beneran, ronde 8)",
        CST_PREFIX,
        "34954707736",
        "0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5",
    ),
]


def check(name: str, prefix: str, nonce: str, expected: str) -> bool:
    got = keccak_hex(prefix + nonce)
    ok = got.lower() == expected.lower()
    print(f"{'PASS' if ok else 'FAIL'} {name}")
    print(f"  prefix+nonce : {prefix}{nonce}")
    print(f"  harapan      : {expected}")
    print(f"  hasil        : {got} ({leading_zero_bits(got)} zero bits)")
    return ok


def main() -> int:
    if len(sys.argv) == 4:
        ok = check("vektor bebas", sys.argv[1], sys.argv[2], sys.argv[3])
        return 0 if ok else 1
    if len(sys.argv) != 1:
        print(__doc__)
        return 2
    results = [check(*v) for v in VECTORS]
    print(f"\n{sum(results)}/{len(results)} vektor lolos")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
