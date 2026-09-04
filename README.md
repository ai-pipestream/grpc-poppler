# grpc-poppler

**License: GPL-3.0-or-later.** This service links Poppler (GPL-2-or-later),
so the combined work is GPL; the GPL dependency of the parsing fleet lives
in this one container and nowhere else. It ships as an optional compose
profile and is excluded from default release artifacts: it exists as the
extraction-quality reference and the differential leg, not as part of the
default stack.

Implements the fleet's common `PdfBackendService` contract
(`ai.pipestream.parse.pdf.v1`, from the pinned parser-protos commit).
The tier 0 floor mirrors the exact poppler-cpp usage of gRParse's
in-process path: `load_from_raw_data`, `text_list(text_list_include_font)`
word boxes, BGR24 rasters at a requested DPI, quarter-turn page geometry,
and the arm64 serialization gate. On top of the floor, the cpp surface
fills document metadata (info keys plus the XMP packet), permission bits
for encrypted documents, the outline, embedded files, and the document
font table. Annotations, form fields, and the structure tree need
poppler's glib surface and are reported unsupported by this build, each
with the reason; deep graphics resources are not poppler's to give.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # links the host poppler-cpp
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
GRPC_POPPLER_PORT=50053 ./build/grpc_poppler
```

Health and server reflection are enabled; `Probe`, `Parse`, and `Render`
are the service surface.

## Compose profile

`compose.yml` defines the service under the `differential` profile, so a
plain `docker compose up` never starts it:

```bash
docker compose --profile differential up grpc-poppler
```
