FROM python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir --no-deps pyarrow==20.0.0
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make native && mkdir /dist && cp build/shopping-sessions /dist/ \
    && cp build/lib/libarrow.so.2000 build/lib/libparquet.so.2000 /dist/

FROM debian:bookworm-slim
COPY --from=build /dist/shopping-sessions /opt/shop/shopping-sessions
COPY --from=build /dist/libarrow.so.2000 /dist/libparquet.so.2000 /opt/shop/lib/
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch HOME=/scratch NCPU=8 MEM_GB=24
USER 1000:1000
ENTRYPOINT ["/opt/shop/shopping-sessions"]
