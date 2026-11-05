# Stable ABI benchmark

## Result

The stable ABI wheel keeps compression, CRC32, bounded decompression and gzip reads at baseline speed and makes unbounded decompression slower. Against the original code at commit `39cec01898a4a44ac988a40e98fd8d8cbbcaacd9`, those five operation groups have medians between 0.994× and 1.012×, and none of their 80 cases differs from the baseline by more than the baseline-again control does. Unbounded decompression pays one extra copy of its output: 17 of its 48 cases are slower than the control's largest deviation, the worst being raw deflate decompression of 1 MiB + 137 bytes of zeros at 2.857×, and at 16 MiB + 137 bytes the three unbounded decoders take 1.045-1.200× the baseline time. These calls also hold one extra output-sized allocation while they finish. The repairs made after the first candidate (commit `6488941`) changed no timing measurably.

## What the candidate changes

Regular CPython builds use the Python 3.11 Limited API, which has no `_PyBytes_Resize()`. These builds therefore grow unbounded output in a `PyMem_Malloc()` and `PyMem_Realloc()` buffer and copy it once into the result with `PyBytes_FromStringAndSize()`, as the maintainer prescribed in [xopen#182](https://github.com/pycompression/xopen/issues/182#issuecomment-6633029582). A call that allocates its whole output limit at once and fills it returns that `bytes` object directly, without the copy. The helpers are `output_allocate()`, `output_resize()` and `finish_output()` in [`zlib_ngmodule.c`](../../src/zlib_ng/zlib_ngmodule.c).

Calls that copy their output once on stable ABI builds:

- `zlib_ng.compress()`, `zlib_ng.decompress()` and the output of `compressobj()`.
- `decompressobj().decompress()` without `max_length`, or with a `max_length` above 16 KiB. With a larger limit the call starts from a 16 KiB buffer, grows it natively and copies the result, so a call that fills its limit briefly holds about twice the limit. The initial allocation was deliberately left at 16 KiB rather than raised to the limit, which would change memory use for callers that pass large limits and receive short results.
- `_ZlibDecompressor.decompress()` without `max_length`.

Calls that avoid the copy: `_ZlibDecompressor.decompress(data, max_length)`, which allocates `max_length` at once; `gzip_ng` file reads such as `read(size)` and `readinto()`, whose internal reader fills fixed-size chunks or the caller's buffer; and `decompressobj().decompress(data, max_length)` with `max_length` up to 16 KiB. A result shorter than its allocation is still copied. The tests `test_filled_bounded_decompressor_output_is_not_copied` and `test_filled_gzip_reader_chunk_is_not_copied` in [`tests/test_stable_abi.py`](../../tests/test_stable_abi.py) use `tracemalloc` to check that a filled bounded result is not copied.

The memory cost follows from the source, not from a benchmark: while `finish_output()` copies, the native buffer and the result coexist, so peak memory rises by one output-sized allocation over the baseline's in-place resize.

Free-threaded CPython and PyPy keep version-specific full-API wheels and the original `_PyBytes_Resize()` path, so they pay no copy.

## Final-wheel comparison

The comparison times installed baseline and candidate wheels built with the same interpreter, compiler and pinned zlib-ng archive; [build.json](build.json) records the versions, the candidate's C source hash and the wheel and extension hashes. Each of nine randomized rounds runs the baseline, a second baseline process (the control) and the candidate on identical inputs. The 128 cases are four inputs (FASTQ, a broad standard-library Python source corpus, seeded random bytes and zeros) × four output sizes (1 KiB, 64 KiB, 1 MiB + 137 bytes and 16 MiB + 137 bytes) × the eight operations below. The input hashes are recorded, and each result is checked against the original bytes or the standard library's zlib outside timing. Times are warm thread CPU time on one pinned CPU. No build, test or other benchmark ran during timing, but desktop processes did, as `machine_note` in build.json records.

| Operation | Call timed |
|---|---|
| Level-1 compression | `zlib_ng.compress(data, 1)` |
| zlib decompression | `zlib_ng.decompress(data)` |
| Raw deflate decompression | `zlib_ng.decompress(data, wbits=-15)` |
| Streaming decompression | `decompressobj().decompress(data)` followed by `flush()` |
| Bounded decompression | `_ZlibDecompressor(wbits=-15).decompress(data, 65536)` until end of stream |
| gzip read | `gzip_ng.open(...).read(131072)` until end of file |
| gzip readinto | `readinto()` into a 131072-byte `bytearray` until end of file |
| CRC32 | `zlib_ng.crc32(data)` |

A case's ratio is the median over the nine rounds of candidate time divided by the same round's baseline time; above 1× is slower. The control ratio is computed the same way for the second baseline process. Its absolute deviation from 1× has a 95th percentile of 4.781% (inclusive method, `statistics.quantiles(method="inclusive")`) and a maximum of 9.892% over the 128 cases. A difference inside that spread is not a measurable difference.

| Operation | Median ratio (16 cases) | Range of case ratios |
|---|---:|---:|
| Level-1 compression | 1.008× | 0.971-1.051× |
| zlib decompression | 1.049× | 0.897-2.316× |
| Raw deflate decompression | 1.036× | 0.882-2.857× |
| Streaming decompression | 1.061× | 0.902-2.259× |
| Bounded decompression | 1.012× | 0.941-1.097× |
| gzip read | 0.994× | 0.983-1.034× |
| gzip readinto | 0.999× | 0.974-1.024× |
| CRC32 | 1.003× | 0.974-1.019× |

All 17 cases slower than the control maximum are unbounded decoders:

| Input and output size | zlib | Raw deflate | Streaming |
|---|---:|---:|---:|
| Zeros, 1 MiB + 137 bytes | 2.316× | 2.857× | 2.259× |
| Random, 1 MiB + 137 bytes | 1.513× | 1.828× | 1.522× |
| Random, 64 KiB | 1.225× | 1.273× | 1.220× |
| Zeros, 16 MiB + 137 bytes | 1.153× | 1.196× | 1.196× |
| Random, 16 MiB + 137 bytes | 1.143× | 1.200× | 1.080× (inside the control) |
| Python source, 1 MiB + 137 bytes | 1.111× | 1.130× | 1.119× |

Faster than the control's 95th percentile: zeros at 64 KiB through the three unbounded decoders (0.882-0.902×, beyond the control maximum for two of the three) and FASTQ at 1 MiB + 137 bytes through the same decoders (0.926-0.939×, inside the control maximum). Bounded decompression of 16 MiB + 137 bytes of zeros (0.941×) is not counted, because its own control ratio is 0.916×. Both speed-ups are unexplained. A 64 KiB output exactly fills the third buffer size (16, 32, then 64 KiB), where the end-of-stream stop described below avoids the baseline's final growth, but in the prototype study's controlled variant the stop saved time at 64 KiB for random input (the original code without it ran 1.385-1.821×), not for zeros (inside the control), so it does not account for this pattern. 1 MiB + 137 bytes is not a buffer boundary.

[results.json](results.json) holds every timing sample, iteration count and interpreter path; [results.csv](results.csv) holds the per-case ratios. `python benchmark_scripts/benchmark_abi3.py --library zlib_ng --summarize benchmark_scripts/abi3_results/results.json` regenerates the control spread, the operation table and the slowest and fastest cases.

## Repair check

[repair-check.json](repair-check.json) repeats the same protocol with the first candidate (commit `6488941`) as baseline and control and the repaired candidate as candidate. The eight operation medians lie between 0.992× and 1.009× and the case ratios between 0.897× and 1.072×, against a control 95th percentile of 4.009% and maximum of 16.555%: no measurable timing change. The first candidate's own comparison, which this one replaces, had a control 95th percentile of 2.884% and a maximum of 5.314%. Interpretation: the larger spread in both new runs comes from the machine state noted above, not from the code.

## Design relative to the maintainer's guidance

The maintainer's prescription in [xopen#182](https://github.com/pycompression/xopen/issues/182#issuecomment-6633029582) and [python-isal#240](https://github.com/pycompression/python-isal/issues/240) is to use the 3.11 Limited API, replace `_PyBytes_Resize()` with `PyMem_Malloc()` and `PyMem_Resize()` buffers, create the result with `PyBytes_FromStringAndSize()` at the end, and accept the performance cost. The candidate follows it, with these departures:

- **Filled bounded output is returned directly.** A call that knows its output limit in advance allocates the result as a `bytes` object and, when it is filled, returns it without the copy and its memory. These are the calls that avoid the copy, listed above.
- **End-of-stream stop.** Output growth stops when zlib-ng reports the end of the stream, even if the buffer is exactly full. `test_stream_end_at_full_output_does_not_grow` shows the saved growth: about 16 KiB of traced peak for a 16384-byte stream. On irregular sizes it changes no timing measurably: in the prototype study below, the original code without the stop has a median of 1.010× against the same code with it over the 24 large decoder cases, inside that study's control.
- **Codec state allocator.** zlib-ng calls its allocation callbacks without an attached thread state. `PyMem_RawMalloc()` is valid there but enters the stable ABI only in 3.13, so Limited-API builds use the C library's `malloc()` and `free()`, and `tracemalloc` does not see codec state on those builds. Full-API builds keep `PyMem_RawMalloc()` and `PyMem_RawFree()`.
- **PyPy keeps its static types.** The PyPy 3.11.17 headers define `Py_TPFLAGS_IMMUTABLETYPE` and `Py_TPFLAGS_DISALLOW_INSTANTIATION`, but the PyPy build keeps its existing static types to preserve existing behavior; heap types are used on CPython only.
- **Free-threaded CPython keeps the full API.** A `cp311` stable ABI wheel cannot load on free-threaded builds. [PEP 803](https://peps.python.org/pep-0803/) defines an `abi3t` stable ABI for free-threaded builds from Python 3.15, which a later change could adopt.
- **CPython types** are heap types created from specifications. `compressobj()` and `decompressobj()` take their types from module state, which owns, traverses and clears them. The process-global `zlib_ng.error` is unchanged, so this adds no subinterpreter isolation.

## Alternative output strategies

A separate prototype study, run against the original code before the final candidate existed, compared four ways of growing unbounded output. It is design evidence only. It used prototype binaries, a different reference and a different Python source input: a source sample repeated to size with a 29,521-byte period, shorter than deflate's 32 KiB window, so it compresses unrepresentatively well. Its ratios must not be compared with the final-wheel tables. Its timings and metadata are committed; its harness, binaries and input files are not.

Each variant adds the end-of-stream stop and one output strategy to the original code. Nine repeats time it against the original code with the stop alone, on the four inputs at four output sizes through three decoders: `decompress()`, `_ZlibDecompressor(wbits=-15).decompress()` without a limit, and `decompressobj().decompress()`, plus 24 compression cases. The variant IDs in the data map to these names:

| Variant ID | Name in this report |
|---|---|
| `baseline` | original code without the end-of-stream stop |
| `baseline-eof` | reference: original code with the stop |
| `eof-control` | control: a second process of the reference |
| `buffers-eof` | native buffer with a final copy (selected) |
| `segmented-eof` | joined segments: output collected in segments and joined at the end |
| `bytesio-eof` | eager BytesIO: output written to a `BytesIO` from the start |
| `bytesio-hybrid-eof` | delayed BytesIO: a `BytesIO` created on the first growth |

The control's absolute deviation over all 72 cases has an inclusive 95th percentile of 8.195% and a maximum of 8.739%. Large means 1 MiB + 137 and 16 MiB + 137 bytes; small means 1 KiB and 64 KiB.

| Strategy | Large decoder cases (24): median | Range | Small decoder cases (24): median | Range |
|---|---:|---:|---:|---:|
| Native buffer (selected) | 1.181× | 0.888-1.911× | 1.016× | 0.821-1.519× |
| Joined segments | 0.998× | 0.392-1.651× | 1.107× | 0.978-1.723× |
| Eager BytesIO | 1.373× | 1.049-1.880× | 1.703× | 1.006-4.752× |
| Delayed BytesIO | 1.368× | 1.139-1.921× | 1.057× | 0.887-2.009× |

- **Joined segments** are faster than the native buffer on large outputs (median 0.998× against 1.181×, a gap wider than the control, and faster in 16 of the 24 cases) and slower on small ones (1.107× against 1.016×). On speed for large outputs, the evidence does not favour the native buffer. Their slowest case, 1.651× for `_ZlibDecompressor` on 16 MiB + 137 bytes, uses the repeated Python input, which says little about real files. The native buffer was kept because it is the design the maintainer prescribed and has less machinery (no segment list and no join pass); the large-output gain of segments remains open for a later change.
- **Delayed BytesIO** beats the native buffer at 1 MiB + 137 bytes in 9 of 12 decoder cases, but loses all 12 at 16 MiB + 137 bytes and has a 1.368× median over the large cases. **Eager BytesIO** is slower still on small outputs, up to 4.752×. Both add Python-level calls and writable buffer exports, so neither was adopted.

[mitigation-confirmation-summary.csv](mitigation-confirmation-summary.csv) holds the per-case ratios of every variant, [mitigation-summary.csv](mitigation-summary.csv) the per-decoder medians over the large cases, [mitigation-confirmation.json](mitigation-confirmation.json) the raw repeats and [mitigation-confirmation-metadata.json](mitigation-confirmation-metadata.json) the study's settings.

## Validation and limits

- One cp311 stable ABI wheel, built with `Py_LIMITED_API=0x030B0000`, passes `abi3audit --strict` with no violations and a computed baseline of 3.11. The same wheel installs on CPython 3.11.15, 3.12.14, 3.13.12, 3.14.4, 3.15.0a8 (alpha) and 3.15.0; smoke checks against standard-library oracles pass on all six, and `tests/test_stable_abi.py` passes on each: 44 passed and 10 skipped (no `_testcapi`), or 52 passed and 2 skipped on 3.14.4.
- Full test suite on the installed wheels, each including the standard-library compliance tests:
  - CPython 3.14.4 with `PYTHONDEVMODE=1`: 14,617 passed and 4 skipped (compliance alone: 138 passed and 2 skipped).
  - Free-threaded CPython 3.13.11 (uv build) with `PYTHON_GIL` unset, the GIL staying off after import: 14,611 passed and 10 skipped. Eight skips need `_testcapi`, which that build lacks, one is a 4 GiB bigmem test, and one needs `sys.gettotalrefcount`.
  - PyPy 3.11.17 (PyPy 8.0.1): 14,594 passed and 27 skipped.
- `test_full_api_codec_state_uses_the_raw_python_allocator` skips on the stable ABI wheel by design, because codec state uses `malloc()` there.
- flake8, mypy (17 files), actionlint, `git diff --check`, strict Sphinx and `twine check` pass.
- `test_import_keeps_the_gil_disabled` checks, in a fresh process with `PYTHON_GIL` unset, that importing the extension leaves the GIL disabled on free-threaded builds.
- Timings come from one Linux x86_64 machine (Intel Core i7-3770, no AVX2); allocator behavior on other platforms can differ.
- Not run locally: the CI jobs for macOS Intel and ARM, Windows x64 and ARM64 (including the new `windows-11-arm` jobs), the release job's `abi3audit` step, and the AddressSanitizer and ThreadSanitizer jobs.

## Reproduce

Build and install the baseline and candidate wheels in separate CPython 3.11 environments with the same compiler and the same zlib-ng archive, built with the CMake options in [build.json](build.json); both source trees need their pinned submodule initialized. Then run:

```sh
python benchmark_scripts/benchmark_abi3.py --library zlib_ng \
    --baseline /tmp/before/bin/python --candidate /tmp/after/bin/python \
    --rounds 9 --cpu 1 --output /tmp/results.json > /tmp/results.log
```

Use the same driver Python version to reproduce the standard-library source input hashes, and keep builds and other benchmarks idle during timing. The harness keeps virtual-environment interpreter paths and removes `PYTHONPATH` from its workers.
