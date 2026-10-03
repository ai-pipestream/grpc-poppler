# Agent rules for grpc-poppler

One of three interchangeable PDF backend services (grpc-pdfium, grpc-qparse,
grpc-poppler) implementing `PdfBackendService`, package
`ai.protomolt.parse.pdf.v1`. gRParse is the client (single target or a comma
list for consensus mode via `GRPARSE_PDF_BACKEND`).

- **The contract lives in the parser-protos repo**
  (`git.rokkon.com/ai-pipestream/parser-protos`, GitHub mirror of the same
  name). This build downloads the proto files from it at the commit pinned
  in `CMakeLists.txt` (`PDF_PROTOS_COMMIT`, per-file sha256). gRParse
  `backends/` carries identical copies. To change the contract: land the
  same bytes in parser-protos and gRParse `backends/`, then advance the pin
  and hashes here. The contract is additive only; never renumber, retype,
  or remove anything.
- **This project is NOT part of the pipestream-ai platform.** Never put the
  contract in, or take dependencies from, `/work/main/pipestream-ai` or the
  `pipestream-protos` repo. (The contract briefly lived there as a
  `pdf-backend` module; reverted 2026-09-04. Do not repeat that.)
- GPL-3 as a combined work with poppler; this stays the one GPL container
  of the fleet, shipped only under the `differential` compose profile and
  excluded from default release artifacts. It exists as the
  extraction-quality reference and the differential leg; keep its tier 0
  behaviour matching gRParse's in-process poppler path to the pixel.
- **Geometry is in the contract's page space**: PDF user space before
  /Rotate, origin bottom-left, with the CropBox origin included (the space
  `PageInfo.crop_box` and the widget rects are reported in, and the one
  grpc-pdfium uses), and `rotation_degrees` is the real /Rotate. poppler-cpp's
  `text_list()` measures boxes in the rotated display frame from the
  CropBox's top-left corner; `TextFrame` (`src/poppler_service_impl.cpp`)
  maps them back. Anything new that carries geometry lands in the same
  space; `test/fixtures/frames.pdf` pins it for every /Rotate and for offset
  CropBoxes.
- **The arm64 gate** (`PopplerGate`, `src/poppler_service_impl.cpp`)
  serializes poppler calls on arm64. A handler holds it while poppler code
  runs and never across a network write: every stream write goes through
  `WriteUngated`, so a slow or stalled client cannot hold every other
  request behind it. Poppler objects are declared after the gate, so they
  are destroyed with it held. Page loops stop when the call is cancelled.
- **The content-addressed handshake** (`PdfDocument.sha256`) is served from
  an in-process LRU byte cache (`src/document_cache.h`), resolved once for
  all three RPCs in `ResolveDocumentBytes` (`src/poppler_service_impl.cpp`).
  Cache misses and hash mismatches are typed `LoadStatus` verdicts
  (`BYTES_REQUIRED` on `Probe` capabilities / `Parse` header / `Render`
  head), never gRPC errors; empty `data` with no `sha256` is
  INVALID_ARGUMENT. Bounds: `GRPC_POPPLER_CACHE_MAX_DOCUMENTS` (default 8)
  and `GRPC_POPPLER_CACHE_MAX_BYTES` (default 2 GiB). SHA-256 is boringssl's
  `SHA256` (`src/sha256.cpp`), linked from the `crypto` target gRPC already
  builds; do not add another crypto dependency.
- **Default port is 50071** (`GRPC_POPPLER_PORT` overrides). The PDF backend
  fleet owns 50069 (grpc-pdfium), 50070 (grpc-qparse), 50071 (grpc-poppler);
  the 50051 to 50053 defaults these services started with collide with
  gRParse, grPOIc, and grpc-libreoffice.
- **GetServiceInfo is implemented** and carries the fleet's `UiInfo` block
  (title "Poppler", path `/ui/poppler`, description noting there is no web
  UI yet). `backend_name` and `engine_version` reuse the exact identity
  strings `Probe` reports (`src/poppler_service_impl.cpp`, `EngineVersion`);
  `build_version` is the `GRPC_POPPLER_BUILD_VERSION` compile define
  (configure with `-DGRPC_POPPLER_BUILD_VERSION=...`; the Dockerfile takes
  it as a build arg and the publish workflow passes the version tag, `dev`
  is the fallback).
- **The image**: multi-stage on a Debian trixie toolchain. The build
  stage builds poppler 26.08.0 from the sha256-pinned tarball (cpp
  frontend and splash only, gRParse's version and option set, never the
  distro's), compiles against it (`PKG_CONFIG_PATH=/opt/poppler/lib/pkgconfig`)
  and runs the full ctest suite as the gate. The runtime is the hardened
  `dhi.io/debian-base:trixie-debian13` base (glibc only, no package
  manager, no ldconfig, uid 65532) carrying the binary, the staged
  shared-library closure (`scripts/stage-runtime-libs.sh`, held to one
  libpoppler major) under `/usr/local/lib` on `LD_LIBRARY_PATH`, the
  Liberation/DejaVu fonts, fontconfig's configuration (Debian keeps the
  conf.d targets under `/usr/share/fontconfig`, so that tree travels
  too) and a prebuilt font cache. The build stage must stay on a glibc no
  newer than the runtime base's (2.41). `GRPC_POPPLER_RUNTIME_IMAGE`
  swaps the base. `scripts/smoke-test.sh IMAGE` is the boot gate (closure,
  boot to listening under the hardened flags, uid, license texts and the
  source label).
- **Publishing**: `.github/workflows/publish.yml` builds, smoke-tests, and
  only then pushes `docker.io/pipestreamai/grpc-poppler:latest` on every
  push to `main` and a `:<version>` tag via `workflow_dispatch`; auth is the
  `DOCKER_USER` / `DOCKER_TOKEN` org secrets. Each architecture (linux/amd64
  on GitHub-hosted runners, linux/arm64 natively on GitHub's hosted arm64
  runner) builds with provenance and SBOM attestations, pushes by digest
  only, and smoke-tests its own digest; `scripts/publish-manifests.sh`
  assembles the passing digests into the tags and verifies the index lists
  exactly both platforms with one attestation manifest per platform.
  `.github/workflows/ci.yml` builds the image (the Dockerfile's build stage
  runs the full ctest suite) and runs the same smoke test.
