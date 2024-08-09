# docker build .
# docker run -it --rm --network=host 14f23e59669c dotnet run --host localhost --port 8123 --user default --database default

FROM mcr.microsoft.com/dotnet/sdk:3.1

WORKDIR /client
COPY *.cs *.csproj /client/

# Troubleshoout api.nuget.org connection timeout
RUN apt update && apt install -y \
    sslscan \
    && sslscan \
        --show-certificate \
        --no-check-certificate \
        --show-client-cas \
        --show-ciphers \
        --show-cipher-ids \
        --show-times \
        api.nuget.org:443 \
    ; openssl s_client -connect  api.nuget.org:443 \
    ; curl -vvvvI https://api.nuget.org/v3/index.json \
    || :

ARG VERSION=4.1.0
RUN dotnet add package ClickHouse.Client -v ${VERSION}
