# Controlled comparison only: the successful reference uses the same toolchain
# and runtime as our candidate, rather than its original Alpine/GCC 14 image.
FROM gcc:13.5.0-bookworm@sha256:8c0fe724784c6d617cb582f79ddaf5dd2233aaba2fb7a14e0b9d03348fea8088 AS build
WORKDIR /src
COPY include ./include
COPY src ./src
RUN --network=none g++ -Iinclude -std=c++20 -O3 -DNDEBUG -ffp-contract=off -pthread \
    src/*.cpp src/*/*.cpp -static-libstdc++ -static-libgcc -o /reference
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
WORKDIR /app
COPY --from=build /reference /app/minivectordb
RUN mkdir -p /data
ENTRYPOINT ["/app/minivectordb","--protocol","jsonl"]
CMD ["--data-dir","/data"]
