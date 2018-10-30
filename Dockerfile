FROM ubuntu:bionic as builder
RUN apt-get update && \
  apt-get install -y cmake g++ gcc make git perl tar wget
# do this in a new folder `dockerbuild`; `build` is ignored via .dockerignore
# for builds outside docker, so we need to make sure we name this something
# other than `build`
WORKDIR /opt/fimd/dockerbuild
COPY . /opt/fimd
RUN cmake -DCMAKE_MAKE_PROGRAM=make \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ .. && \
  # try and minimize build times by running in parallel according
  # to the number of cores available on the host machine
  make -j$(nproc --all)
# Include grpc_health_probe for K8s liveness/readiness checks.
RUN GRPC_HEALTH_PROBE_VERSION=v0.1.0-alpha.1 && \
  wget -qO/bin/grpc_health_probe https://github.com/grpc-ecosystem/grpc-health-probe/releases/download/${GRPC_HEALTH_PROBE_VERSION}/grpc_health_probe-linux-amd64 && \
  chmod +x /bin/grpc_health_probe

FROM scratch
COPY --from=builder /opt/fimd/dockerbuild/fimd /
COPY --from=builder /bin/grpc_health_probe /bin/
# glog requires /tmp to exist as log_dir is /tmp by default
COPY --from=builder /tmp /tmp
CMD ["/fimd", "-insecure"]
