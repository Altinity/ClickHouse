

# docker build -t altinityinfra/binary-builder .
ARG FROM_TAG=latest
FROM altinityinfra/test-util:$FROM_TAG as cctools
# The cctools are built always from the altinityinfra/test-util:latest and cached inline
# Theoretically, it should improve rebuild speed significantly

ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# DO NOT PUT ANYTHING BEFORE THE NEXT TWO `RUN` DIRECTIVES
# THE MOST HEAVY OPERATION MUST BE THE FIRST IN THE CACHE
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# libtapi is required to support .tbh format from recent MacOS SDKs
RUN git clone https://github.com/tpoechtrager/apple-libtapi.git \
    && cd apple-libtapi \
    && git checkout 15dfc2a8c9a2a89d06ff227560a69f5265b692f9 \
    && INSTALLPREFIX=/cctools ./build.sh \
    && ./install.sh \
    && cd .. \
    && rm -rf apple-libtapi

# NOTE(vnemkov): Not required for arm64 only build, reenable for arm64 builds.
# # Build and install tools for cross-linking to Darwin (x86-64)
# RUN git clone --depth 1 https://github.com/tpoechtrager/cctools-port.git \
#     && cd cctools-port/cctools \
#     && ./configure --prefix=/cctools --with-libtapi=/cctools \
#         --target=x86_64-apple-darwin \
#     && make install -j$(nproc) \
#     && cd ../.. \
#     && rm -rf cctools-port

# # Build and install tools for cross-linking to Darwin (aarch64)
# RUN git clone --depth 1 https://github.com/tpoechtrager/cctools-port.git \
#     && cd cctools-port/cctools \
#     && ./configure --prefix=/cctools --with-libtapi=/cctools \
#         --target=aarch64-apple-darwin \
#     && make install -j$(nproc) \
#     && cd ../.. \
#     && rm -rf cctools-port

# !!!!!!!!!!!
# END COMPILE
# !!!!!!!!!!!

FROM altinityinfra/test-util:$FROM_TAG
ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}

# Install CMake 3.20+ for Rust compilation
# Used https://askubuntu.com/a/1157132 as reference
RUN apt purge cmake --yes
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'
RUN apt update && apt install cmake --yes


# Install Docker
ENV DOCKER_CHANNEL stable
RUN curl -fsSL https://download.docker.com/linux/ubuntu/gpg | apt-key add - \
    && add-apt-repository "deb https://download.docker.com/linux/ubuntu $(lsb_release -c -s) ${DOCKER_CHANNEL}" \
    && apt-get update \
    && env DEBIAN_FRONTEND=noninteractive apt-get install --yes \
        docker-ce \
    && rm -rf \
        /var/lib/apt/lists/* \
        /var/cache/debconf \
        /tmp/* \
    && apt-get clean

RUN dockerd --version; docker --version


# Rust toolchain and libraries
ENV RUSTUP_HOME=/rust/rustup
ENV CARGO_HOME=/rust/cargo
ENV PATH="/rust/cargo/bin:${PATH}"
RUN curl https://sh.rustup.rs -sSf | bash -s -- -y && \
    chmod 777 -R /rust && \
    rustup toolchain install nightly-2023-07-04 && \
    rustup default nightly-2023-07-04 && \
    rustup component add rust-src && \
    rustup target add aarch64-unknown-linux-gnu && \
    rustup target add x86_64-apple-darwin && \
    rustup target add x86_64-unknown-freebsd && \
    rustup target add aarch64-apple-darwin && \
    rustup target add powerpc64le-unknown-linux-gnu

# NOTE: Seems like gcc-11 is too new for ubuntu20 repository
# A cross-linker for RISC-V 64 (we need it, because LLVM's LLD does not work):
RUN apt-get update \
    && apt-get install --yes \
        binutils-riscv64-linux-gnu \
        build-essential \
        g++-11 \
        gcc-11 \
        gcc-aarch64-linux-gnu \
        libc6 \
        libc6-dev \
        libc6-dev-arm64-cross \
        python3-boto3 \
        yasm \
        zstd \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists

# Download toolchain and SDK for Darwin
RUN curl -sL -O https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX11.0.sdk.tar.xz

# Download and install mold 2.0 for s390x build
RUN curl -Lo /tmp/mold.tar.gz "https://github.com/rui314/mold/releases/download/v2.0.0/mold-2.0.0-x86_64-linux.tar.gz" \
    && mkdir /tmp/mold \
    && tar -xzf /tmp/mold.tar.gz -C /tmp/mold \
    && cp -r /tmp/mold/mold*/* /usr \
    && rm -rf /tmp/mold \
    && rm /tmp/mold.tar.gz

# Architecture of the image when BuildKit/buildx is used
ARG TARGETARCH
ARG NFPM_VERSION=2.20.0

RUN arch=${TARGETARCH:-amd64} \
    && curl -Lo /tmp/nfpm.deb "https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION}/nfpm_${arch}.deb" \
    && dpkg -i /tmp/nfpm.deb \
    && rm /tmp/nfpm.deb

ARG GO_VERSION=1.19.10
# We need go for clickhouse-diagnostics
RUN arch=${TARGETARCH:-amd64} \
    && curl -Lo /tmp/go.tgz "https://go.dev/dl/go${GO_VERSION}.linux-${arch}.tar.gz" \
    && tar -xzf /tmp/go.tgz -C /usr/local/ \
    && rm /tmp/go.tgz

ENV PATH="$PATH:/usr/local/go/bin"
ENV GOPATH=/workdir/go
ENV GOCACHE=/workdir/

ARG CLANG_TIDY_SHA1=c191254ea00d47ade11d7170ef82fe038c213774
RUN curl -Lo /usr/bin/clang-tidy-cache \
        "https://raw.githubusercontent.com/matus-chochlik/ctcache/$CLANG_TIDY_SHA1/clang-tidy-cache" \
    && chmod +x /usr/bin/clang-tidy-cache

COPY --from=cctools /cctools /cctools
RUN mkdir /workdir && chmod 777 /workdir
WORKDIR /workdir
COPY build.sh /
COPY daemon.json /etc/docker/

CMD ["bash", "-c", "/build.sh 2>&1"]
