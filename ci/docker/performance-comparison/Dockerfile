# docker build -t altinityinfra/performance-comparison .

ARG FROM_TAG=latest
FROM altinityinfra/test-base:$FROM_TAG

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends \
            bash \
            curl \
            dmidecode \
            g++ \
            git \
            gnuplot \
            imagemagick \
            libc6-dbg \
            moreutils \
            ncdu \
            numactl \
            p7zip-full \
            parallel \
            psmisc \
            python3 \
            python3-dev \
            python3-pip \
            rsync \
            tree \
            tzdata \
            vim \
            wget \
            rustc \
            cargo \
            ripgrep \
            zstd \
            unzip \
    && apt-get purge --yes python3-dev g++ \
    && apt-get autoremove --yes \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /var/cache/debconf /tmp/*

COPY requirements.txt /
RUN pip3 --no-cache-dir install -r requirements.txt

COPY --from=altinityinfra/cctools:7c093c1cde57d744be57 /opt/gdb /opt/gdb
ENV PATH="/opt/gdb/bin:${PATH}"

# aws cli to acquire secrets and params from ssm
RUN curl "https://awscli.amazonaws.com/awscli-exe-linux-$(uname -m).zip" -o "awscliv2.zip" \
    && unzip -q awscliv2.zip && ./aws/install \
    && rm -rf /awscliv2.zip /aws
