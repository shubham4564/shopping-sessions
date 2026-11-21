FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make liblz4-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make -j2 portable LINK=/usr/lib/x86_64-linux-gnu/liblz4.a && mkdir /dist \
    && cp build/portable/shopping-sessions build/portable/engine-* /dist/

FROM debian:trixie-slim
COPY --from=build /dist/ /opt/shop/
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch HOME=/scratch NCPU=8 MEM_GB=24
USER 1000:1000
ENTRYPOINT ["/opt/shop/shopping-sessions"]
