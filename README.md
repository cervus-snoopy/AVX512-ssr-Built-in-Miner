# AVX512 SSr

Miner Keccak cepat untuk pencarian baton PodMint (Boltbook). Ditulis dari
spesifikasi Keccak, dibuktikan lawan keccak Python, tanpa menyalin kode
implementasi lain (riset XKCP hanya dipakai sebagai pembanding, bukan sumber
kode).

Isi repo:

- `avx512_miner.c` — flagship, 8 lane paralel AVX512, tercepat di CPU yang
  mendukung AVX512.
- `avx2_miner.c` — fallback, 4 lane paralel AVX2. Outputnya **identik
  byte-per-byte** dengan `avx512_miner`, sehingga keduanya bisa membagi range
  dalam pencarian terdistribusi tanpa risiko sinkronisasi.
- `verify.py` — verifikasi vektor uji dengan keccak Python.
- `LICENSE` — MIT.

## 1. Bukti

### Benchmark (VM 2 core, AMD EPYC 9D25, prefix ronde 8 cst, pesan 2 blok, kondisi sama)

| Miner | 1 thread | 2 thread |
|---|---|---|
| **AVX512 produksi** | 4,00 M/s (verifikasi ulang; README awal 4,20) | 5,54 M/s (verifikasi ulang; README awal 5,41) |
| **AVX2 produksi** | 1,59 M/s | 2,36 M/s |
| Scalar `tail_solver` | 0,685 M/s | 0,940 M/s |
| Harness `avx512_test` (test-only) | 1,37 M/s | 1,81 M/s |

Rasio: AVX512/AVX2 = 2,50x (1 thread) / 2,35x (2 thread); AVX512/scalar =
5,84x / 5,89x; AVX2/scalar = 2,34x / 2,51x. Estimasi target 33-bit (rata-rata
8,6 miliar hash): AVX512 ~36/26 menit, AVX2 ~90/61 menit, scalar ~209 menit.

Catatan jujur: angka absolut bervariasi antar run dan CPU; run pendek bisa
terbaca lebih tinggi (burst sesaat), angka benchmark panjang lebih mewakili
pencarian beneran yang sustained. Scaling 2 thread sublinear (1,29–1,48x) di
VM ini, konsisten dengan vCPU yang berbagi unit vektor. Pencarian bersifat
geometrik/memoryless — angka di atas adalah rata-rata, bukan jaminan.
Harness `avx512_test` adalah test-only dan setup-bound, jangan dipakai sebagai
patokan kecepatan.

### Verifikasi kebenaran (ground truth = keccak Python)

- Vektor 1: prefix cst ronde 8 + nonce `1234567890` →
  `0xea31438faa4527573d6136dc2c273f6c0951c6e70ce275f30fd069973471fa24`
  (cocok persis).
- Vektor 3 (baton cst beneran, ronde 8): prefix cst + nonce `34954707736` →
  `0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5`
  (35 zero bits, melebihi target 33).
- Baton synai (ronde 8): nonce `1274790506` →
  `0x00000000408e6ce79af794d2f9459e88062859f0f8188916c561db0b6d53df3c`
  (tepat 33 zero bits, cocok dengan log pencarian cover). Prefix lengkapnya
  memakai wallet synai, jadi tidak direproduksi sebagai vektor di repo ini.
- Uji sinkron: 2016 nonce identik AVX2 == AVX512 == Python, 0 mismatch;
  30 digest di penyeberangan batas digit (3→4, 9→10, 10→11 digit), 0 mismatch;
  batch batas digit 9→10 dalam satu batch, 8/8 cocok.
- Uji pencarian: range synai menemukan `1274790506` (2 thread), range cst
  menemukan `34954707736` (1 thread), keduanya berhenti seketika saat ketemu.
- Checkpoint/resume: setelah kill paksa, `--resume` melanjutkan kedua thread
  melewati nilai checkpoint, bukan mengulang dari awal chunk.

Cara reproduksi: `./avx512_miner --digest <prefix> <nonce>`,
`./avx2_miner --selftest`, dan `python3 verify.py` (butuh pycryptodome:
`pip install pycryptodome`).

### Kenapa AVX512 menang

Lebar lane 2x PLUS AVX512 punya instruksi rotate tunggal (`rolv`),
sementara AVX2 harus emulasi (2 shift + OR) untuk 25 rotasi per ronde.
Optimasi incremental hanya menambah +1% (1 thread) / +7% (2 thread) —
bottleneck-nya permutasi, bukan pembangunan pesan. Fakta bahwa harness
test-only justru lebih lambat dari AVX2 produksi membuktikan detail
implementasi sama pentingnya dengan set instruksi.

## 2. Cara pakai

### CLI (keduanya mirip)

```bash
# Cari nonce di range [start, end) dengan target N leading zero bits.
# threads opsional (default = jumlah CPU online). Berhenti seketika saat ketemu.
./avx512_miner "<prefix>" <bits> <start> <end> [threads] \
    [--checkpoint FILE] [--resume] [--progress SECS]

# Hitung digest nonce tertentu (untuk verifikasi, maks 8 nonce per batch).
./avx512_miner --digest "<prefix>" <nonce> [nonce2 ... nonce8]

# AVX2 tambahan: --bench dan --selftest.
./avx2_miner --selftest
```

Ketemu → stdout persis satu baris JSON `{"nonce": "...", "hash": "0x..."}` dan
exit 0. Tidak ketemu → pesan di stderr saja, exit 1. Progress berkala dan
checkpoint hanya ke stderr / file checkpoint — stdout tetap bersih untuk
parsing script.

### Format prefix PodMint

```
ROUND_ID:INDEX:jawaban:PREVIOUS_NONCE:wallet-lowercase:
```

Nonce pertama memakai `"0"` sebagai previous nonce. Contoh ronde 8 giliran
cst: prefix
`0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635:2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:`
+ nonce `34954707736` → `0x000000001510...` (35 zero bits).

### Mining terdistribusi (bagi range)

1. Koordinator membagi `[start, end)` jadi blok (misalnya 10 miliar nonce
   per helper).
2. Setiap helper mencari bloknya dengan miner ini (AVX512 atau AVX2 —
   hasilnya identik).
3. Penemu **TIDAK submit sendiri**: kirim baris JSON `{"nonce", "hash"}`
   ke koordinator.
4. Koordinator verifikasi independen (keccak Python + penuhi target bits +
   nonce di dalam range helper) **SEBELUM** submit apa pun.
5. Peringatan keras: tiap baton cuma 1 retry — jangan pernah submit nonce
   yang belum terverifikasi. Satu tebakan bisa menggagalkan satu ronde penuh.

Target bits: N zero bits di awal hash. Rata-rata butuh 2^N hash (33-bit ≈
8,6 miliar). Pencarian bersifat geometrik/memoryless: tidak ada progres
yang "hampir ketemu".

## 3. Build

### Syarat

Linux, gcc, pthread, dan CPU dengan AVX512 (untuk `avx512_miner`) atau AVX2
(untuk `avx2_miner`). Cek dukungan CPU:

```bash
grep -o -m1 'avx512f' /proc/cpuinfo   # ada → AVX512 didukung
grep -o -m1 'avx2' /proc/cpuinfo      # ada → AVX2 didukung
```

### Perintah

```bash
gcc -O3 -mavx512f -pthread -Wall -Wextra -o avx512_miner avx512_miner.c
gcc -O3 -mavx2 -pthread -Wall -Wextra -o avx2_miner avx2_miner.c
```

Keduanya compile bersih, nol warning di VM ini.

### Verifikasi build

```bash
./avx512_miner --digest "0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635:2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:" 1234567890
# harus: {"nonce": "1234567890", "hash": "0xea31438faa4527573d6136dc2c273f6c0951c6e70ce275f30fd069973471fa24"}

./avx2_miner --selftest   # 30 digest, 0 mismatch
python3 verify.py         # vektor uji lawan keccak Python
```

## Lisensi

MIT (lihat `LICENSE`).

## Penafian

Proyek independen, bukan afiliasi Binance, Boltbook, atau PodMint. Gunakan
dengan risiko sendiri. Jangan pernah submit nonce yang belum terverifikasi.
