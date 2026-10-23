# Hugging Face Space (Docker SDK): builds the service straight from the GitHub repository.
# Space secret ALFA_LLM_API_KEY holds the organizers' key; it never goes into git.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git cmake g++ make libcurl4-openssl-dev python3 && rm -rf /var/lib/apt/lists/*
ARG REPO=https://github.com/BayramErgeshov/alpha_hack_2026
ARG REF=main
RUN git clone "$REPO" /src && cd /src && git checkout "$REF"
WORKDIR /src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DALFA_BUILD_EXAMPLES=OFF \
    && cmake --build build --parallel 2 \
    && ./build/backend_tests

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libcurl4 curl \
    && rm -rf /var/lib/apt/lists/* && useradd -m -u 1000 user && mkdir -p /app/work && chown 1000:1000 /app/work
WORKDIR /app
COPY --from=build /src/build/alpha_backend /app/alpha_backend
COPY --from=build /src/data /app/data
COPY --from=build /src/ui /app/ui
USER 1000
ENV ALFA_HOST=0.0.0.0 ALFA_PORT=7860 ALFA_DATA_DIR=/app/data/input ALFA_ANALYTICS_DIR=/app/data/analytics \
    ALFA_NEWS_SEED=/app/data/news/seed.json ALFA_WORK_DIR=/app/work ALFA_STATIC_DIR=/app/ui \
    ALFA_NEWS_REFRESH_MINUTES=30 ALFA_LLM_DAILY_TOKEN_LIMIT=300000
EXPOSE 7860
ENTRYPOINT ["/app/alpha_backend"]
