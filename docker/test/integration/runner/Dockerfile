# docker build -t clickhouse/integration-tests-runner .
FROM ubuntu:20.04

# ARG for quick switch to a given ubuntu mirror
ARG apt_archive="http://archive.ubuntu.com"

RUN sed -i "s|http://archive.ubuntu.com|$apt_archive|g" /etc/apt/sources.list

RUN apt-get update \
    && env DEBIAN_FRONTEND=noninteractive apt-get install --yes \
    adduser \
    ca-certificates \
    bash \
    btrfs-progs \
    e2fsprogs \
    iptables \
    xfsprogs \
    tar \
    pigz \
    wget \
    git \
    iproute2 \
    cgroupfs-mount \
    python3-pip \
    tzdata \
    libicu-dev \
    bsdutils \
    curl \
    python3-pika \
    liblua5.1-dev \
    luajit \
    libssl-dev \
    libcurl4-openssl-dev \
    gdb \
    software-properties-common \
    libkrb5-dev \
    krb5-user \
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
    PyMySQL~=1.1.0 \
    aerospike~=11.1.0 \
    asyncio~=3.4.3\
    avro~=1.10.2 \
    azure-storage-blob~=12.19.0\
    cassandra-driver~=3.28.0\
    confluent-kafka~=1.9.2 \
    delta-spark~=2.3.0 \
    dict2xml~=1.7.3 \
    dicttoxml~=1.7.16 \
    docker~=6.1.3 \
    docker-compose~=1.29.2 \
    grpcio~=1.59.3 \
    grpcio-tools~=1.59.3 \
    kafka-python~=2.0.2 \
    kazoo~=2.9.0 \
    lz4~=4.3.2 \
    meilisearch~=0.18.3 \
    minio~=7.2.0 \
    nats-py~=2.6.0 \
    protobuf~=4.25.1 \
    psycopg2-binary~=2.9.6 \
    pyhdfs~=0.3.1 \
    pymongo~=3.11.0 \
    pyspark~=3.3.2 \
    pytest~=7.4.3 \
    pytest-order~=1.0.0 \
    pytest-random~=0.2 \
    pytest-repeat~=0.9.3 \
    pytest-timeout~=2.2.0 \
    pytest-xdist~=3.5.0 \
    pytz \
    pyyaml~=5.3.1 \
    redis~=5.0.1 \
    requests==2.31.0 \
    requests-kerberos \
    requests~=2.31.0 \
    tzlocal~=2.1 \
    retry~=0.9.2 \
    bs4~=0.0.1 \
    lxml~=4.9.3 \
    urllib3~=2.1.0

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
