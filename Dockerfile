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

FROM scratch
COPY --from=builder /opt/fimd/dockerbuild/fimd /
# glog requires /tmp to exist as log_dir is /tmp by default
COPY --from=builder /tmp /tmp
CMD ["/fimd"]
