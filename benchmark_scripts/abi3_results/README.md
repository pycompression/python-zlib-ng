# Stable ABI benchmark

Regular CPython uses the Python 3.11 Limited API. Unbounded output grows in native storage and is copied once into its final `bytes` object. Full bounded allocations return their filled bytes directly. Free-threaded CPython and PyPy retain the full-API bytes-resize path; PyPy also retains its original static types so they stay immutable.

## Final source comparison

This compares installed baseline and candidate wheels built with the same interpreter, compiler settings and pinned native archive. The baseline is commit `39cec01898a4a44ac988a40e98fd8d8cbbcaacd9`. Candidate C source and wheel hashes are in [build.json](build.json). The benchmark uses the actual production code, not an experimental output strategy.

Nine randomized paired rounds cover four inputs (FASTQ, a broad standard-library Python source corpus, seeded random bytes and zeros), four output sizes (1 KiB, 64 KiB, 1 MiB + 137 bytes and 16 MiB + 137 bytes), and eight operations. Each input hash is recorded. Correctness is checked against the original bytes or standard-library zlib outside timing. Measurements are warm thread CPU time, pinned to one core, with no other investigation build or benchmark running.

The table gives the median candidate/baseline time ratio over the 16 cases in each row, followed by the range of case medians. Lower is faster. The separate baseline-again control has a 95th-percentile absolute median difference of 2.884% and a maximum of 5.314%.

| Operation | Median time ratio | Case range |
|---|---:|---:|
| `compress1` | 1.009 | 0.990–1.023 |
| `decompress` | 1.054 | 0.888–2.296 |
| `raw-decompress` | 1.075 | 0.884–2.810 |
| `stream-decompress` | 1.093 | 0.903–2.265 |
| `bounded-decompress` | 1.003 | 0.966–1.051 |
| `gzip-read128k` | 1.002 | 0.989–1.038 |
| `gzip-readinto128k` | 1.003 | 0.982–1.016 |
| `crc32` | 1.000 | 0.968–1.010 |

The largest losses are real and should not be hidden by the row medians. Decompressing 1 MiB + 137 bytes of zeros takes **2.810×** as long with raw deflate (126.8 to 356.1 µs), **2.296×** with the zlib wrapper (177.4 to 408.3 µs), and **2.265×** through a streaming decoder (181.2 to 410.2 µs). For 16 MiB + 137 byte outputs, unbounded decompression takes 1.046–1.214× as long across the three APIs and four inputs. These paths pay for the final copy and allocation behavior.

Compression, checksums and ordinary 128 KiB gzip reads have medians near 1.00. Bounded decompression has a 1.003 median, with a largest case of 1.051. Differences near the control variation are inconclusive. These results do not promise equal speed for every workload.

[results.json](results.json) contains every timing sample and the interpreter/module paths. [results.csv](results.csv) contains paired case medians. [benchmark_abi3.py](../benchmark_abi3.py) is the reproducible harness.

## Alternatives and design deviations

- The maintainer suggested replacing private bytes resizing with native allocation followed by a final bytes copy. That is the general unbounded-output implementation. Bounded results that fill their allocation keep a direct bytes path; short results are copied. This preserves APIs and avoids an unnecessary copy on the usual fixed-block path.
- EOF stops are applied when the native codec reports stream completion, even if the output buffer is full. This is an independent optimization that also helps the original implementation at exact allocation boundaries. The final primary comparison includes irregular sizes so that a power-of-two-only benefit cannot dominate the conclusion.
- Segmented bytes joined at the end, eager BytesIO, and bytes-to-BytesIO promotion on the first growth were tested as disposable alternatives. In a separate nine-round study of 24 large irregular-output decoder cases, raw buffers had a 1.181 median time ratio, segments 0.998, eager BytesIO 1.373 and the hybrid 1.368, versus the same tuned full-API baseline. Segments ranged from 0.392 to 1.651, so their median concealed opposing data/size effects. For 16 MiB + 137 bytes of the earlier Python-source input, one-shot raw buffers took 1.057× baseline time versus 1.467× for segments. With zeros at the same size, the corresponding ratios were 1.159× versus 0.393×. BytesIO also adds export and factory-lifetime state. None was adopted as a general replacement.
- [mitigation-summary.csv](mitigation-summary.csv) records that separate study. Its earlier Python input repeated the package sources (30,032 bytes), unlike the final broad stdlib corpus. It is design evidence only; its ratios must not be compared with the final source table.
- PyPy and free-threaded CPython keep interpreter-specific full-API wheels. PyPy retains static C types because its headers do not expose the immutable-type flag; replacing them with ordinary heap types would change existing behavior. CPython uses heap type specifications and balanced instance/type references. Factory-type references are owned by module state with traverse/clear cleanup; the existing process-global exception policy is unchanged, so this does not claim stronger subinterpreter isolation. FASTCALL and `PyObject_New` are retained.
- zlib-ng's native callbacks use matched libc allocation/free because they run without the GIL, and `PyMem_RawMalloc/RawFree` did not enter the Stable ABI until Python 3.13. Output scratch buffers still use `PyMem_Malloc/Realloc` while the thread state is attached.

## Validation and limits

The actual ABI3 wheel passes strict `abi3audit`, installs under CPython 3.11–3.15, and passes the new ownership/boundary tests on each. CPython 3.14 passes 14,588 tests including stdlib compliance, with 3 skips. PyPy 3.11 passes 14,576 tests with 15 skips. The final free-threaded wheel passes 14,450 tests with 1 skip (the interpreter lacks the stdlib test package); a fresh normal interpreter process, with `PYTHON_GIL` removed, confirms importing the extension leaves the GIL disabled. Existing sanitizer jobs remain in CI.

Same-wheel CI covers Linux, Intel macOS, ARM macOS and Windows on CPython 3.11–3.15. Windows 3.14t also builds from source to exercise its explicit free-threading macro. An interpreter without the stdlib `test` package skips only the two compliance files. Those new platform jobs, and ASan/TSan, were not run locally. Timing conclusions here apply to this Linux x86_64 host; cross-platform allocator behavior can differ.

## Reproduce

Build and install the baseline and candidate wheels in separate CPython 3.11 environments with the same compiler and zlib-ng revision. Both source trees need their pinned submodule initialized. The recorded native build uses Release/PIC settings; see [build.json](build.json) for versions and hashes. Then run:

```sh
python benchmark_scripts/benchmark_abi3.py --library zlib_ng \
    --baseline /tmp/before/bin/python --candidate /tmp/after/bin/python \
    --rounds 9 --cpu 1 --output /tmp/results.json > /tmp/results.log
```

Use the same driver Python version to reproduce the stdlib-source input hashes. Keep native builds and other benchmarks idle during timing. The harness preserves virtual-environment interpreter paths and removes `PYTHONPATH` from its workers.
