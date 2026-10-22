FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DALFA_BUILD_EXAMPLES=OFF && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libcurl4 && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 app
WORKDIR /app
COPY --from=build /src/build/alpha_backend /app/alpha_backend
COPY data /app/data
USER 10001:10001
ENV ALFA_HOST=0.0.0.0 ALFA_PORT=8080 ALFA_DATA_DIR=/app/data/input ALFA_ANALYTICS_DIR=/app/data/analytics
EXPOSE 8080
ENTRYPOINT ["/app/alpha_backend"]
