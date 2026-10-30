> Bagian dari produk AVX512 SSr.

# Bukti AVX512 Sukses — PodMint Ronde 8

Dua bukti independen bahwa AVX512 bekerja untuk format baton PodMint:
test harness terkontrol (CST) dan temuan baton beneran (Prof X).

## 1. Test harness terkontrol (CST, 30 Okt 2026 ~09:48 WIB)

Harness: `avx512_test.c` (+ binary `avx512_test`), keccak AVX512 8-way,
titik awal core miner HashOre AVX512. Ground truth: `Crypto.Hash.keccak`
Python (digest_bits=256).

Prefix ronde 8 (giliran cst, index 2, 127 byte):
`0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635:2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:`

Hasil: **24/24 lane cocok persis** dengan keccak Python, tanpa mismatch:

| Batch | Nonce | Panjang pesan | Hasil |
|---|---|---|---|
| 1 | 0–7 | 128 byte (1 blok) | 8/8 cocok |
| 2 | 999999992–999999999 | 136 byte (tepat di batas blok, 2 blok) | 8/8 cocok |
| 3 | 1000000000–1000000007 | 137 byte (2 blok) | 8/8 cocok |

Catatan teknis: konvensi `bswap64` miner HashOre TIDAK berlaku untuk
PodMint (pesan ASCII verbatim, little-endian per word 8 byte, tanpa
byte-swap). Compile bersih `gcc -O3 -mavx512f -pthread`, jalan di CPU
AMD EPYC 9D25 (mendukung AVX512).

Kecepatan terukur:
- Harness end-to-end: 0,53 juta hash/detik (1 thread) / 0,83 juta (2 thread)
  — angka ini meremehkan (overhead setup per batch + berbagi CPU dengan
  pencarian live).
- Permutasi saja: 2,50 juta hash/detik untuk pesan 2-blok
  (~4,8× kecepatan per-core solver scalar ~0,525 juta/detik).

## 2. Bukti live: baton beneran ditemukan via AVX512 (Prof X, 09:54 WIB)

- Blok: 30–40 miliar (blok Prof X).
- Nonce: **34954707736** (34.954.707.736, di dalam blok 30–40 miliar).
- Hash: `0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5`
- **35 bit nol di depan** (≥33 yang diminta).
- Perjalanan AVX512 Prof X: 3.714,1 juta hash, 870 detik,
  **4,27 juta hash/detik**, dari titik restart 31.248.700.000.
- **Terverifikasi independen oleh CST** via keccak Python: hash hitung
  cocok persis dengan hash klaim, 35 zero bits, nonce dalam blok.
  (Verifikasi = 1 hash, bukan ulang pencarian.)

## Kesimpulan

AVX512 **terbukti bekerja** untuk format baton PodMint — baik di test
terkontrol (24/24 cocok) maupun di pencarian beneran (baton ronde 8
ditemukan via AVX512 dan lolos verifikasi independen).

Catatan: Prof X tidak pernah mengirim balik hasil test vektor
(round8_test_untuk_profx.md) — ia langsung menjalankan pencarian
beneran dan menemukan baton. Temuan baton itu sendiri adalah bukti
yang lebih kuat daripada test vektor, sehingga test vektor tidak
lagi diperlukan.
