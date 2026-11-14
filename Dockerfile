FROM gcc:13.5.0-bookworm@sha256:8c0fe724784c6d617cb582f79ddaf5dd2233aaba2fb7a14e0b9d03348fea8088 AS build
WORKDIR /build
COPY Makefile ./
COPY src/ ./src/
COPY third_party/ ./third_party/
RUN --network=none make -j2 LDFLAGS='-static-libstdc++ -static-libgcc' \
    && g++ --version > compiler-version.txt \
    && sha256sum minivectordb > binary.sha256

FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
WORKDIR /app
COPY --from=build /build/minivectordb /app/minivectordb
COPY third_party/json.LICENSE.MIT /app/licenses/nlohmann-json.MIT
COPY third_party/rapidjson.LICENSE /app/licenses/RapidJSON-LICENSE.txt
COPY third_party/GCC-RUNTIME-EXCEPTION-3.1.txt /app/licenses/GCC-RUNTIME-EXCEPTION-3.1.txt
COPY --from=build /usr/share/common-licenses/GPL-3 /app/licenses/GPL-3
COPY --from=build /build/compiler-version.txt /build/binary.sha256 /app/build-info/
RUN --network=none mkdir -p /data
VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
