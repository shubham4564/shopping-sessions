FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make liblz4-1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make -j2 portable && mkdir -p /dist/lib \
    && cp build/portable/shopping-sessions build/portable/engine-* /dist/ \
    && cp -L /usr/lib/x86_64-linux-gnu/liblz4.so.1 /dist/lib/

FROM debian:bookworm-slim
COPY --from=build /dist/ /opt/shop/
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch HOME=/scratch NCPU=8 MEM_GB=24
USER 1000:1000
ENTRYPOINT ["/opt/shop/shopping-sessions"]
