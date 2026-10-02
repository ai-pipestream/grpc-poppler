# syntax=docker/dockerfile:1.27
# GPL-3.0-or-later image: links Poppler. Off the default release path;
# built only for the differential profile. This service is the
# extraction-quality reference of the PDF backend fleet, so its poppler is
# the exact version gRParse's in-process path vendors (26.08.0 from the
# sha256-pinned tarball), not the distro's: a distro poppler predates the
# 26.06 thread-safety fixes in annots loading (upstream 4aca25d6, 2f10803d)
# and would rasterize a page differently from the path it is compared to.
#
# Multi-stage in the family's C++ shape: the build stage builds poppler
# (cpp frontend and splash renderer only), compiles the service against it
# and runs the full ctest suite (the tests gate the image). The runtime
# stage is a hardened, glibc-only base: no package manager, no ldconfig run,
# and no shell needed. The binary's whole shared-library closure beyond
# glibc (poppler, freetype, fontconfig, jpeg, openjpeg, lcms2, libstdc++
# and their dependencies) is staged from the build stage into
# /usr/local/lib by scripts/stage-runtime-libs.sh, which fails the build if
# anything would still resolve from outside it, and the image is held to
# gRParse's invariant of exactly one libpoppler major in the load set.
#
# The build stage is Debian trixie on purpose: the runtime base's glibc is
# 2.41, and a binary linked against a newer glibc refuses to load there.
# The base is swappable for any image whose glibc is 2.41 or newer:
#   --build-arg GRPC_POPPLER_RUNTIME_IMAGE=<image>
ARG GRPC_POPPLER_RUNTIME_IMAGE=dhi.io/debian-base:trixie-debian13

FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config curl xz-utils ninja-build \
    libfreetype-dev libfontconfig-dev libjpeg-dev libopenjp2-7-dev \
    liblcms2-dev libboost-dev \
    && rm -rf /var/lib/apt/lists/*

# Poppler from the pinned tarball, the same version and option set as
# gRParse's own images so the two poppler paths stay comparable to the
# pixel. Only the cpp frontend and the splash renderer are built. The core
# headers are installed too (ENABLE_UNSTABLE_API_ABI_HEADERS): the AcroForm
# widgets are read through the core API, which the cpp frontend does not
# wrap. Installing headers leaves the library itself unchanged.
ARG POPPLER_VERSION=26.08.0
ARG POPPLER_SHA256=dc906e68cea698109706ac6aa3d2c9d4512fcfcac42d90b8afcda486d1b9abd0
RUN curl -fsSL -o /tmp/poppler.tar.xz "https://poppler.freedesktop.org/poppler-${POPPLER_VERSION}.tar.xz" \
 && echo "${POPPLER_SHA256}  /tmp/poppler.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/poppler.tar.xz -C /tmp \
 && cmake -S "/tmp/poppler-${POPPLER_VERSION}" -B /tmp/poppler-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/poppler -DCMAKE_INSTALL_LIBDIR=lib \
      -DENABLE_CPP=ON -DENABLE_QT5=OFF -DENABLE_QT6=OFF -DENABLE_GLIB=OFF -DENABLE_UTILS=OFF \
      -DENABLE_BOOST=ON -DENABLE_NSS3=OFF -DENABLE_GPGME=OFF -DENABLE_LIBCURL=OFF \
      -DENABLE_LIBTIFF=OFF -DENABLE_LIBOPENJPEG=openjpeg2 -DBUILD_CPP_TESTS=OFF \
      -DBUILD_GTK_TESTS=OFF -DBUILD_QT5_TESTS=OFF -DBUILD_QT6_TESTS=OFF -DBUILD_MANUAL_TESTS=OFF \
      -DENABLE_UNSTABLE_API_ABI_HEADERS=ON \
 && cmake --build /tmp/poppler-build --parallel 4 \
 && cmake --install /tmp/poppler-build \
 && rm -rf /tmp/poppler.tar.xz "/tmp/poppler-${POPPLER_VERSION}" /tmp/poppler-build

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY test ./test
COPY scripts ./scripts

# Reported by GetServiceInfo; the publish workflow passes the version tag.
ARG GRPC_POPPLER_BUILD_VERSION=dev

# The cache id encodes every ABI-sensitive dependency; bump it when gRPC,
# poppler, or the toolchain moves. The contract protos are downloaded from
# the pinned parser-protos commit at configure time (sha256-verified), so
# the build needs network access.
# Compile parallelism is bounded: an unbounded build on a shared builder
# starves its neighbours and gets the compiler OOM-killed; 8 jobs is what
# the gRPC compile tolerates beside other builds.
# A second builder sharing this cache mount (a developer build beside a CI
# run, or an interrupted build that left a truncated object behind) gets its
# own tree through --build-arg GRPC_POPPLER_BUILD_CACHE_SCOPE=-<name>.
ARG GRPC_POPPLER_BUILD_CACHE_SCOPE=
RUN --mount=type=cache,id=grpc-poppler-trixie-grpc1.83.1-poppler26.08${GRPC_POPPLER_BUILD_CACHE_SCOPE},target=/build \
    export PKG_CONFIG_PATH=/opt/poppler/lib/pkgconfig \
 && cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_POPPLER_BUILD_VERSION="${GRPC_POPPLER_BUILD_VERSION}" \
    && cmake --build /build --parallel 8 \
    && LD_LIBRARY_PATH=/opt/poppler/lib ctest --test-dir /build --output-on-failure \
    && mkdir -p /out/lib && cp /build/grpc_poppler /out/ \
    && LD_LIBRARY_PATH=/opt/poppler/lib scripts/stage-runtime-libs.sh /out/lib /out/grpc_poppler \
    && test "$(ls /out/lib | grep -cE '^libpoppler\.so\.[0-9]+$')" = 1

# The Liberation fonts are poppler's base-14 substitutes: a PDF that uses
# Helvetica/Times/Courier without embedding them renders blank text without
# a metric-compatible substitute, which starves text extraction and page
# rasters of glyphs. fc-cache prebuilds the fontconfig cache so the
# read-only runtime never tries to write one.
# The font set is pinned rather than inherited: fontconfig substitutes
# whatever it happens to find, so an image that carries a different set
# rasterizes non-embedded text to different pixels than its siblings, and a
# page the images disagree about cannot be compared between them. This
# image asks for the same fonts gRParse's images do, explicitly.
# A cache file is valid only while its font directory's mtime equals the
# one recorded at fc-cache time, so the fonts, fontconfig's configuration
# (Debian keeps the conf.d targets under /usr/share/fontconfig) and the
# cache are staged as one tree and shipped by one COPY: separate COPYs let
# the layer cache pair a reused font layer with a newer cache (its key
# hashes content, not directory mtimes), and the read-only runtime then
# logs "No writable cache directories" and rescans the fonts on first use.
# The directory mtimes are pinned before fc-cache so the cache contents,
# and with them the layer, are the same from one build to the next.
RUN apt-get update && apt-get install -y --no-install-recommends \
    fonts-liberation fonts-dejavu-core fontconfig \
    && rm -rf /var/lib/apt/lists/* \
    && find /usr/share/fonts /usr/local/share/fonts -type d -exec touch -d @1 {} + \
    && fc-cache -f \
    && mkdir -p /out/fonts/usr/share /out/fonts/var/cache /out/fonts/etc \
    && cp -a /usr/share/fonts /usr/share/fontconfig /out/fonts/usr/share/ \
    && cp -a /etc/fonts /out/fonts/etc/ \
    && cp -a /var/cache/fontconfig /out/fonts/var/cache/

# LD_LIBRARY_PATH stands in for ldconfig, and the numeric USER works with or
# without a passwd entry (65532 is the conventional nonroot uid in hardened
# images).
FROM ${GRPC_POPPLER_RUNTIME_IMAGE}
COPY --from=build /out/lib/ /usr/local/lib/
# Fontconfig's configuration, the Liberation and DejaVu fonts, and the
# prebuilt font cache, as the one tree staged above: PDFs with embedded
# fonts never need any of this, but non-embedded base-14 text would
# otherwise come out blank.
COPY --from=build /out/fonts/ /
COPY --from=build /out/grpc_poppler /usr/local/bin/grpc_poppler
ENV GRPC_POPPLER_PORT=50071 \
    LD_LIBRARY_PATH=/usr/local/lib
# Documents stay in memory and nothing is written at startup, so the
# container runs read-only as it is:
#   docker run --rm --read-only -p 50071:50071 grpc-poppler
USER 65532:65532
EXPOSE 50071
ENTRYPOINT ["/usr/local/bin/grpc_poppler"]
