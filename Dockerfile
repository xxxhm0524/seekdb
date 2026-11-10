# MiniVectorDB - immutable base and dated Ubuntu archive, JSONL stdin/stdout.
# The certificate data is verified before extraction; TLS and APT signature
# checks stay enabled while bootstrapping HTTPS access to the snapshot archive.
FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS certificate-data
ADD https://snapshot.ubuntu.com/ubuntu/20261110T160000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.2_all.deb /ca.deb
RUN printf '%s  /ca.deb\n' 'f9fbacd417b515450b831032cc39d18059e9874e67525679a43c16071dabf9a0' | sha256sum -c - \
    && mkdir -p /bootstrap/package \
    && dpkg-deb -x /ca.deb /bootstrap/package \
    && cat /bootstrap/package/usr/share/ca-certificates/mozilla/*.crt > /bootstrap/ca-certificates.crt

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS build
COPY --from=certificate-data /bootstrap/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt

ENV DEBIAN_FRONTEND=noninteractive
RUN printf 'APT::Snapshot "20261110T160000Z";\n' > /etc/apt/apt.conf.d/50mvd-snapshot \
    && apt-get update \
    && apt-cache policy | grep -Fq 'snapshot.ubuntu.com/ubuntu/20261110T160000Z' \
    && apt-get install -y --no-install-recommends build-essential ca-certificates \
    && dpkg-query -W -f='${binary:Package}=${Version}\n' | LC_ALL=C sort > /build-packages.txt \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make -j"$(nproc)"

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
COPY --from=certificate-data /bootstrap/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt

ENV DEBIAN_FRONTEND=noninteractive
RUN printf 'APT::Snapshot "20261110T160000Z";\n' > /etc/apt/apt.conf.d/50mvd-snapshot \
    && apt-get update \
    && apt-cache policy | grep -Fq 'snapshot.ubuntu.com/ubuntu/20261110T160000Z' \
    && apt-get install -y --no-install-recommends libgomp1 libstdc++6 libgcc-s1 ca-certificates \
    && dpkg-query -W -f='${binary:Package}=${Version}\n' | LC_ALL=C sort > /runtime-packages.txt \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/minivectordb /app/minivectordb
COPY --from=build /build-packages.txt /app/build-packages.txt
RUN mkdir -p /data

# The evaluator keeps stdin open (-i) and captures stderr separately.
# stdout carries only newline-delimited JSON protocol messages.
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
