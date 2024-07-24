# docker build -t clickhouse/integration-tests-runner .
FROM ubuntu:20.04

# ARG for quick switch to a given ubuntu mirror
ARG apt_archive="http://archive.ubuntu.com"

RUN sed -i "s|http://archive.ubuntu.com|$apt_archive|g" /etc/apt/sources.list

RUN apt-get update \
    && env DEBIAN_FRONTEND=noninteractive apt-get install --yes \
    adduser='3.*' \
    ca-certificates \
    bash='5.*' \
    btrfs-progs='5.4.*' \
    e2fsprogs='1.45.*' \
    iptables='1.8.*' \
    xfsprogs='5.3.*' \
    tar \
    pigz='2.4*' \
    wget \
    git \
    iproute2='5.5.*' \
    cgroupfs-mount \
    python3-pip \
    tzdata \
    libicu-dev='66.*' \
    bsdutils='1:2.34*' \
    curl \
    python3-pika='0.11.*' \
    liblua5.1-dev \
    luajit='2.1.*' \
    libssl-dev='1.1.*' \
    libcurl4-openssl-dev='7.68.*' \
    gdb \
    software-properties-common \
    libkrb5-dev='1.17*' \
    krb5-user='1.17*' \
    g++ \
    && rm -rf \
        /var/lib/apt/lists/* \
        /var/cache/debconf \
        /tmp/* \
    && apt-get clean

ENV TZ=Etc/UTC
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

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

RUN python3 -m pip install --no-cache-dir \
    PyMySQL \
    aerospike~=4.0 \
    avro~=1.10 \
    asyncio~=3.4 \
    cassandra-driver~=3.28 \
    confluent-kafka~=1.5 \
    dict2xml~=1.7 \
    dicttoxml~=1.7 \
    docker~=6.1 \
    docker-compose~=1.29 \
    grpcio~=1.56 \
    grpcio-tools~=1.56 \
    kafka-python~=2.0 \
    kazoo~=2.9 \
    lz4~=4.3 \
    minio~=7.1 \
    nats-py~=2.3 \
    protobuf~=4.24 \
    psycopg2-binary~=2.8 \
    pymongo~=3.11 \
    pytest~=7.4 \
    pytest-order~=1.0 \
    pytest-timeout~=2.1 \
    pytest-xdist~=3.3 \
    pytest-repeat~=0.9 \
    pytz \
    redis~=4.6 \
    tzlocal~=2.1 \
    urllib3~=1.25 \
    requests-kerberos~=0.14 \
    pyhdfs~=0.3 \
    azure-storage-blob~=12.17 \
    meilisearch~=0.18

COPY modprobe.sh /usr/local/bin/modprobe
COPY dockerd-entrypoint.sh /usr/local/bin/
COPY compose/ /compose/
COPY misc/ /misc/

RUN set -x \
  && addgroup --system dockremap \
    && adduser --system dockremap \
  && adduser dockremap dockremap \
  && echo 'dockremap:165536:65536' >> /etc/subuid \
    && echo 'dockremap:165536:65536' >> /etc/subgid

EXPOSE 2375
ENTRYPOINT ["dockerd-entrypoint.sh"]
CMD ["sh", "-c", "pytest $PYTEST_OPTS"]
