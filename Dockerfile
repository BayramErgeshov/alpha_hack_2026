# Alfa News Insights backend: build on Debian 12 (GCC 12, CMake 3.25, libcurl 7.88), run as non-root.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make libcurl4-openssl-dev python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY data ./data
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DALFA_BUILD_EXAMPLES=OFF \
    && cmake --build build --parallel \
    && ./build/backend_tests

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libcurl4 curl && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 app \
    && mkdir -p /app/work && chown 10001:10001 /app/work
WORKDIR /app
COPY --from=build /src/build/alpha_backend /app/alpha_backend
COPY data /app/data
COPY ui /app/ui
USER 10001:10001
ENV ALFA_HOST=0.0.0.0 ALFA_PORT=8080 ALFA_DATA_DIR=/app/data/input ALFA_ANALYTICS_DIR=/app/data/analytics \
    ALFA_NEWS_SEED=/app/data/news/seed.json ALFA_WORK_DIR=/app/work ALFA_STATIC_DIR=/app/ui ALFA_NEWS_REFRESH_MINUTES=30
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=40s CMD curl -fsS http://127.0.0.1:8080/health || exit 1
ENTRYPOINT ["/app/alpha_backend"]
