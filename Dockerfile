FROM gcc:14.3.0-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15 AS build
WORKDIR /build
COPY src/ ./src/
RUN g++ -O3 -std=c++20 -Wall -Wextra -Wpedantic -ffp-contract=off -static \
    src/main.cpp -o /minivectordb && mkdir /data

FROM scratch
COPY --from=build /minivectordb /minivectordb
COPY --from=build /data /data
ENTRYPOINT ["/minivectordb"]
