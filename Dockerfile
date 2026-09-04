# GPL-3.0-or-later image: links Poppler. Off the default release path;
# built only for the differential profile.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    libpoppler-cpp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY test ./test
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpoppler-cpp2 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/grpc_poppler /usr/local/bin/grpc_poppler
ENV GRPC_POPPLER_PORT=50053
EXPOSE 50053
ENTRYPOINT ["/usr/local/bin/grpc_poppler"]
