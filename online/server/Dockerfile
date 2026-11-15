# shadNet server for the GODZILLA PC port, built from source in a Debian container.
# The same image runs on this PC for testing and on the cloud server later.

FROM debian:trixie AS build
ARG SHADNET_COMMIT=b38cf17
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build g++ \
        qt6-base-dev qt6-httpserver-dev qt6-websockets-dev libqt6sql6-sqlite \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone https://github.com/shadps4-emu/shadNet.git . \
    && git checkout "$SHADNET_COMMIT" \
    && git submodule update --init --recursive --depth 1
RUN cmake -S . -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel
# Record which Debian packages the built program links against, for the slim runtime image.
RUN ldd build/shadnet | awk '/=> \//{print $3}' | xargs -r realpath | xargs -r dpkg -S 2>/dev/null \
      | cut -d: -f1 | sort -u > /runtime-packages.txt \
    && cat /runtime-packages.txt

FROM debian:trixie
COPY --from=build /runtime-packages.txt /tmp/runtime-packages.txt
RUN apt-get update \
    && apt-get install -y --no-install-recommends $(cat /tmp/runtime-packages.txt) libqt6sql6-sqlite \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages.txt
COPY --from=build /src/build/shadnet /opt/shadnet/shadnet
COPY --from=build /src/clientversions.cfg /opt/shadnet/defaults/clientversions.cfg
COPY shadnet.cfg worlds.cfg scoreboards.cfg /opt/shadnet/defaults/
COPY entrypoint.sh /entrypoint.sh
RUN sed -i 's/\r$//' /entrypoint.sh /opt/shadnet/defaults/* && chmod +x /entrypoint.sh \
    && useradd --system --home /data shadnet && mkdir -p /data && chown shadnet /data
USER shadnet
VOLUME /data
# 31313 game protocol (TCP), 31314 signaling (UDP), 31315 web API (TCP)
EXPOSE 31313/tcp 31314/udp 31315/tcp
ENTRYPOINT ["/entrypoint.sh"]
