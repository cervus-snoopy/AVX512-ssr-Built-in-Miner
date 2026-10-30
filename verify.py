#!/usr/bin/env python3
"""Verifikasi vektor uji AVX512 SSr lawan keccak Python.

Pemakaian:
    python3 verify.py
        Memeriksa vektor contoh bawaan.
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


EXAMPLE_PREFIX = "AVX512-SSr-example:"

VECTORS = [
    (
        "vektor 1 (contoh + 0)",
        EXAMPLE_PREFIX,
        "0",
        "0xb93b2eb109bcea2d1fe94f8293f3df069759b1c5d188ce04e305432f9769cc27",
    ),
    (
        "vektor 2 (contoh + 1234567890)",
        EXAMPLE_PREFIX,
        "1234567890",
        "0x84c2d16be9e4fc2f409e30f4e6b6cba218e24076d3124d528bf37749d2c85e0c",
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
