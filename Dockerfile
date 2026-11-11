# Fixed official compiler/runtime images; all build instructions run offline.
# Compiler digest: docker-library/repo-info, gcc/remote/13.5.0-bookworm.md.
FROM gcc:13.5.0-bookworm@sha256:b59036579f0622d1b3aa5286aaf2fcef9b9c6f88f31c6762236fc2f136f7f650 AS build

WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN --network=none make -j"$(nproc)" \
    && g++ --version > /compiler-version.txt \
    && dpkg-query -W -f='${binary:Package}=${Version}\n' | LC_ALL=C sort > /build-packages.txt \
    && mkdir -p /runtime-libs \
    && for mvd_library in libstdc++.so.6 libgcc_s.so.1 libgomp.so.1; do \
         mvd_library_path="$(g++ -print-file-name="$mvd_library")"; \
         test "$mvd_library_path" != "$mvd_library"; \
         cp -L "$mvd_library_path" "/runtime-libs/$mvd_library"; \
       done

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

WORKDIR /app
COPY --from=build /src/minivectordb /app/minivectordb
COPY --from=build /runtime-libs/ /app/lib/
COPY --from=build /build-packages.txt /app/build-packages.txt
COPY --from=build /compiler-version.txt /app/compiler-version.txt
COPY --from=build /usr/share/common-licenses/GPL-3 /app/licenses/GPL-3
COPY docker/licenses/GCC-RUNTIME-EXCEPTION-3.1.txt /app/licenses/GCC-RUNTIME-EXCEPTION-3.1.txt
ENV LD_LIBRARY_PATH=/app/lib
RUN --network=none dpkg-query -W -f='${binary:Package}=${Version}\n' | LC_ALL=C sort > /runtime-packages.txt \
    && sha256sum /app/lib/* > /runtime-library-sha256.txt \
    && ldd /app/minivectordb > /runtime-linkage.txt \
    && ! grep -q 'not found' /runtime-linkage.txt \
    && mkdir -p /data

# Only JSONL on stdin/stdout; the evaluator mounts the persistent directory.
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
