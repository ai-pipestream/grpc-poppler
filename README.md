# grpc-poppler

**License: GPL-3.0-or-later.** This service links Poppler, whose code
inherited from xpdf is licensed under the GPL version 2 or version 3 only
(Poppler's later code is GPL-2.0-or-later), and gRPC, protobuf and abseil
under Apache-2.0, which combines with the GPL version 3 but not version 2;
so the combined work is GPL-3.0. The image carries this repository's
LICENSE and Poppler's COPYING and COPYING3 under `/usr/share/doc`, and its
`org.opencontainers.image.source` label names this repository as the
source. This is meant to be the parsing fleet's one GPL container; gRParse
still links poppler-cpp in-process until its move to the backend contract
(milestone M6) lands. It ships as an optional compose profile and is
excluded from default release artifacts: it exists as the
extraction-quality reference and the differential leg, not as part of the
default stack.

Implements the fleet's common `PdfBackendService` contract
(`ai.protomolt.parse.pdf.v1`, from the pinned parser-protos commit).
The tier 0 floor mirrors the exact poppler-cpp usage of gRParse's
in-process path: `load_from_raw_data`, `text_list(text_list_include_font)`
word boxes, BGR24 rasters at a requested DPI, page geometry, and the arm64
serialization gate. `text_list` measures its word boxes in the frame
poppler lays the page out in for display (the page's /Rotate applied,
origin at the top-left corner of the CropBox); the service maps them back
into the contract's page space, PDF user space before /Rotate with the
CropBox origin included (the space of `PageInfo.crop_box`, the widget rects
and grpc-pdfium's boxes), starts each word's quad at its lower-left corner
in its reading direction, and reports the page's real /Rotate (0, 90, 180
or 270). On top of the floor, the cpp surface
fills document metadata (info keys plus the XMP packet), permission bits
for encrypted documents, the outline, and the document font table.
AcroForm widgets (form fields) come from poppler's core API, which the cpp
wrapper does not expose: `src/poppler_forms.cpp` opens a core document over
the same bytes and reports each widget with its inherited field type, /Ff
flags and tooltip and the widget's own /AS appearance state. Embedded files
come from the core API too (`src/poppler_attachments.cpp`): the cpp wrapper
cuts a UTF-16 file name at its first NUL byte and inflates a whole payload
before handing it out, while the core file spec gives the raw name, decoded
here as a PDF text string, and the stream, decoded block by block under a
size cap (see Resource limits). The poppler build installs the core headers
for this (`ENABLE_UNSTABLE_API_ABI_HEADERS`); the library is the one
poppler-cpp already links. Annotations and the structure tree are still
reported unsupported, each with the reason; deep graphics resources are not
poppler's to give.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # links the poppler-cpp pkg-config finds
cmake --build build -j
ctest --test-dir build --output-on-failure
```

A local build links whatever `poppler-cpp` and `poppler` pkg-config find,
and needs poppler's core headers installed (a distro `libpoppler-private-dev`
or a poppler built with `ENABLE_UNSTABLE_API_ABI_HEADERS=ON`); the image
builds poppler 26.08.0 from the pinned tarball instead (see Docker below).

## Run

```bash
GRPC_POPPLER_PORT=50071 ./build/grpc_poppler
```

The default port is 50071 (the fleet assigns each service its own default;
`GRPC_POPPLER_PORT` overrides). Health and server reflection are enabled;
`Probe`, `Parse`, `Render`, and `GetServiceInfo` are the service surface.
`GetServiceInfo` reports the same backend name and engine version `Probe`
carries, the build version (`-DGRPC_POPPLER_BUILD_VERSION` at configure
time, the image tag in published images, `dev` otherwise), and the fleet's
`UiInfo` advertisement.

## Content-addressed documents

The contract lets a client upload the PDF bytes once and address them by
hash on later calls: a request with `data` empty and `PdfDocument.sha256`
set is a cache lookup. On a miss the service answers
`LOAD_STATUS_BYTES_REQUIRED` (typed, never a gRPC error) and the client
retries exactly once with the bytes; a `data` plus `sha256` request whose
bytes do not hash to the given value gets `LOAD_STATUS_HASH_MISMATCH`.
On `Render`, load failures of any kind arrive as a single
`RenderResponse.head` message and the stream ends.

The bytes live in an in-process LRU cache (grpc-poppler is single-process,
so the cache sits beside the RPC surface; on grpc-pdfium it belongs in the
front process, which owns the client-facing wire). Two env knobs bound it:

| Env var | Default | Meaning |
|---|---|---|
| `GRPC_POPPLER_CACHE_MAX_DOCUMENTS` | `8` | most documents held at once |
| `GRPC_POPPLER_CACHE_MAX_BYTES` | `2147483648` (2 GiB) | total cached bytes |

Setting either to `0` disables caching; a document larger than the byte
ceiling is never stored. SHA-256 comes from the boringssl `crypto` target
the gRPC build already compiles; there is no new dependency.

## Resource limits

One request cannot make the service decode without bound. The defaults
(`ResourceLimits`, `src/poppler_service_impl.h`) also keep every message
inside the server's 520 MiB message limit:

| Bound | Default | Past it |
|---|---|---|
| Attachment data (`include_attachment_data`) | 256 MiB decoded, per attachment | the attachment is listed without `data`; the trailer carries a `ParseWarning` naming it |
| Render `dpi` | 1200 | `INVALID_ARGUMENT`, as are zero, negative, NaN and infinite values |
| Pixels per rendered page | 150 million (a 450 MB BGR raster) | `RESOURCE_EXHAUSTED`, checked from the page size before splash allocates |

A page poppler cannot render ends the Render stream with `INTERNAL` naming
the page, rather than being left out of the stream.

A set `PageRange` must have `end` greater than `begin` and a `begin` below
2^31 (poppler indexes pages with an int); anything else is
`INVALID_ARGUMENT`. An `end` past the document stops at its last page.

## Docker

```bash
docker build -t grpc-poppler .
docker run --rm --read-only -p 50071:50071 grpc-poppler
scripts/smoke-test.sh grpc-poppler     # boot-proof a built image
```

The build stage (a Debian trixie toolchain) builds poppler 26.08.0 from the
sha256-pinned tarball (cpp frontend and splash renderer only, the same
version and option set gRParse's own images vendor, so the two poppler
paths stay comparable to the pixel), compiles the service against it, and
runs the full ctest suite as the image gate. A distro poppler is not used:
it predates the 26.06 thread-safety fixes in annots loading and would
differ from the path this service is the reference for.

The runtime stage is the hardened `dhi.io/debian-base:trixie-debian13`
base: glibc and nothing else, no package manager, no ldconfig, and the
service runs as uid 65532 out of the box, so no `--user` flag is needed.
The binary's whole shared-library closure beyond glibc (poppler, freetype,
fontconfig, jpeg, openjpeg, lcms2, libstdc++ and their dependencies) is
staged from the build stage into `/usr/local/lib` on `LD_LIBRARY_PATH` by
`scripts/stage-runtime-libs.sh`, which fails the build if anything would
resolve from outside it; the image is also held to exactly one libpoppler
major in the load set. The Liberation and DejaVu fonts ride along as
poppler's base-14 substitutes (non-embedded Helvetica/Times/Courier text
would otherwise come out blank), with fontconfig's configuration and a
prebuilt font cache so a read-only container never writes one. The base
is swappable with `--build-arg GRPC_POPPLER_RUNTIME_IMAGE=<image>` for any
image whose glibc is 2.41 or newer.

`scripts/smoke-test.sh IMAGE` is the boot gate CI and the publish workflow
run before any push: the library closure resolves inside the image (the
dynamic loader reports it, since the base has no `ldd`), the server
reaches its "listening on" line under `--read-only --cap-drop ALL`,
every process runs as uid 65532, and the license texts and the source
label are in place.

## Compose profile

`compose.yml` defines the service under the `differential` profile, so a
plain `docker compose up` never starts it:

```bash
docker compose --profile differential up grpc-poppler
```

## Image publishing

`.github/workflows/publish.yml` republishes `docker.io/pipestreamai/grpc-poppler:latest`
on every push to `main` and adds a `:<version>` tag via `workflow_dispatch`.
The image is a linux/amd64 + linux/arm64 manifest list: the amd64 leg builds
on GitHub-hosted runners, the arm64 leg natively on GitHub's hosted arm64
runner (Docker Hub auth is the `DOCKER_USER` / `DOCKER_TOKEN` org secrets).
Each leg builds with provenance and SBOM attestations, pushes by digest only,
and boot-proofs its own digest with `scripts/smoke-test.sh` before the
`publish` job assembles the passing digests into the tags with
`scripts/publish-manifests.sh` (which fails unless the index lists exactly
both platforms with one attestation manifest each). The workflow passes the
version tag as the `GRPC_POPPLER_BUILD_VERSION` build arg so `GetServiceInfo`
reports it. `.github/workflows/ci.yml` builds the image on push and PR (the
build stage runs the full ctest suite) and boot-proofs the runtime image
with `scripts/smoke-test.sh`; the publish workflow runs the same smoke test
before it pushes.
