# syntax=docker/dockerfile:1.26
# GPL-3.0-or-later image: links Poppler. Off the default release path;
# built only for the differential profile. System poppler is a documented
# decision: this service is the extraction-quality reference of the PDF
# backend fleet, so it tracks the distro's poppler on purpose.
#
# Multi-stage in the family's C++ shape: the build stage compiles and runs
# the full ctest suite (the tests gate the image), the runtime stage carries
# only the binary and the poppler shared libraries.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    libpoppler-cpp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY test ./test

# Reported by GetServiceInfo; the publish workflow passes the version tag.
ARG GRPC_POPPLER_BUILD_VERSION=dev

# The cache id encodes every ABI-sensitive dependency; bump it when gRPC or
# the toolchain moves.
RUN --mount=type=cache,id=grpc-poppler-trixie-grpc1.83.1,target=/build \
    cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_POPPLER_BUILD_VERSION="${GRPC_POPPLER_BUILD_VERSION}" \
    && cmake --build /build -j"$(nproc)" \
    && ctest --test-dir /build --output-on-failure \
    && mkdir -p /out && cp /build/grpc_poppler /out/

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpoppler-cpp2 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/grpc_poppler /usr/local/bin/grpc_poppler
ENV GRPC_POPPLER_PORT=50071
EXPOSE 50071
ENTRYPOINT ["/usr/local/bin/grpc_poppler"]
