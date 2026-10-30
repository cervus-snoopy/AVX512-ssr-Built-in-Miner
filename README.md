# AVX512 SSr

## Description

AVX512 SSr is a fast parallel Keccak-256 miner, built and tested as original
work. It is a basic framework: you can edit it, extend it, and use it as a
miner for anything. For additional tools, please adapt it yourself. Mining
becomes faster.

Miner Keccak-256 paralel cepat untuk pencarian nonce: diberi sebuah string
prefix, program mencari nonce di dalam sebuah range sehingga
`keccak256(prefix + nonce)` memiliki minimal N bit nol di awal
(leading zero bits). Tersedia dua implementasi dengan output yang identik:
AVX512 (8 lane paralel) dan AVX2 (4 lane paralel, fallback untuk CPU tanpa
AVX512). Karena outputnya identik byte-per-byte, keduanya bisa membagi satu
range pencarian tanpa risiko sinkronisasi.

## Isi repo

- `avx512_miner.c` — implementasi utama AVX512, tercepat di CPU yang
  mendukung AVX512.
- `avx2_miner.c` — fallback AVX2, output identik dengan `avx512_miner`.
- `verify.py` — verifikasi vektor uji dengan keccak Python.
- `LICENSE` — MIT.

## Cara kerja

- Masukan: string prefix + nonce desimal. Hash: Keccak-256.
- Target: N leading zero bits. Rata-rata butuh 2^N hash (contoh: 33-bit
  ≈ 8,6 miliar hash). Pencarian bersifat geometrik/memoryless: tidak ada
  progres yang "hampir ketemu".
- Ketemu → stdout tepat satu baris JSON `{"nonce": "...", "hash": "0x..."}`
  dan exit 0. Tidak ketemu → hanya pesan di stderr, exit 1. Progress berkala
  dan checkpoint hanya ke stderr / file checkpoint — stdout tetap bersih
  untuk parsing script.

## Build

Syarat: Linux, gcc, pthread, dan CPU dengan AVX512 (untuk `avx512_miner`)
atau AVX2 (untuk `avx2_miner`). Cek dukungan CPU:

```bash
grep -o -m1 'avx512f' /proc/cpuinfo   # ada → AVX512 didukung
grep -o -m1 'avx2' /proc/cpuinfo      # ada → AVX2 didukung
```

Perintah:

```bash
gcc -O3 -mavx512f -pthread -Wall -Wextra -o avx512_miner avx512_miner.c
gcc -O3 -mavx2 -pthread -Wall -Wextra -o avx2_miner avx2_miner.c
```

Keduanya compile bersih, nol warning di mesin pengujian.

## Cara pakai

```bash
# Cari nonce di range [start, end) dengan target N leading zero bits.
# threads opsional (default = jumlah CPU online). Berhenti seketika saat ketemu.
./avx512_miner "<prefix>" <bits> <start> <end> [threads] \
    [--checkpoint FILE] [--resume] [--progress SECS]

# Hitung digest nonce tertentu (untuk verifikasi, maks 8 nonce per batch).
./avx512_miner --digest "<prefix>" <nonce> [nonce2 ... nonce8]

# AVX2 tambahan: --bench dan --selftest.
./avx2_miner --selftest "<prefix>" <start> <end>
```

## Verifikasi

```bash
./avx512_miner --digest "<prefix>" <nonce>   # bandingkan dengan implementasi Keccak-256 referensi
./avx2_miner --selftest "<prefix>" <start> <end>  # dump digest untuk dibandingkan dengan keccak Python
python3 verify.py                            # vektor uji lawan keccak Python (butuh pycryptodome: pip install pycryptodome)
```

Uji sinkron yang sudah dijalani: 2016 nonce, output AVX2 == AVX512 ==
implementasi referensi Python, 0 mismatch; digest di penyeberangan batas
digit (3→4, 9→10, 10→11 digit), 0 mismatch. Checkpoint/resume: setelah kill
paksa, `--resume` melanjutkan kedua thread melewati nilai checkpoint, bukan
mengulang dari awal chunk.

## Benchmark (pengukuran contoh)

VM 2 core, pesan 2 blok, kondisi sama:

| Miner | 1 thread | 2 thread |
|---|---|---|
| **AVX512** | 4,00 M/s | 5,54 M/s |
| **AVX2** | 1,59 M/s | 2,36 M/s |
| Skalar pembanding | 0,685 M/s | 0,940 M/s |

Rasio: AVX512/AVX2 ≈ 2,50x (1 thread) / 2,35x (2 thread); AVX512/skalar
≈ 5,8x / 5,9x. Penyebab: lebar lane 2x PLUS AVX512 punya instruksi rotate
tunggal (`rolv`), sementara AVX2 harus emulasi rotate (2 shift + OR) untuk
25 rotasi per putaran permutasi.

Catatan jujur: angka absolut bervariasi antar run dan CPU; run pendek bisa
terbaca lebih tinggi (burst sesaat), angka benchmark panjang lebih mewakili
pencarian beneran yang sustained. Scaling 2 thread sublinear di VM ini,
konsisten dengan vCPU yang berbagi unit vektor. Angka di atas adalah
rata-rata, bukan jaminan.

## Pencarian terdistribusi (bagi range)

1. Koordinator membagi `[start, end)` jadi blok (misalnya 10 miliar nonce
   per helper).
2. Setiap helper mencari bloknya dengan miner ini (AVX512 atau AVX2 —
   hasilnya identik).
3. Penemu **tidak langsung bertindak** atas temuannya: kirim baris JSON
   `{"nonce", "hash"}` ke koordinator.
4. Koordinator verifikasi independen (keccak Python + penuhi target bits +
   nonce di dalam range helper) **SEBELUM** memakai hasil itu.
5. Peringatan keras: jangan pernah memakai nonce yang belum terverifikasi.
   Satu hasil yang salah bisa merusak keseluruhan pekerjaan.

## Lisensi

MIT (lihat `LICENSE`).

## Penafian

Proyek independen. Gunakan dengan risiko sendiri. Jangan pernah memakai
nonce yang belum terverifikasi.
