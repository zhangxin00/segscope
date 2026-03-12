FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
     ca-certificates \
     gnupg \
     lsb-release \
     wget \
     build-essential \
     cmake \
     ninja-build \
  && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
     | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg \
  && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] https://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-18 main" \
     > /etc/apt/sources.list.d/llvm.list \
  && apt-get update \
  && apt-get install -y --no-install-recommends \
     clang-18 \
     llvm-18 \
     llvm-18-dev \
     lld-18 \
  && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18 \
    CXX=clang++-18

WORKDIR /work
COPY . /work
