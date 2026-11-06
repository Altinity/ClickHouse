# docker build -t clickhouse/python-bottle .
# Helper docker container to run python bottle apps

# NOTE (strtgbb): python:3 moves, so this image rebuilt from scratch on every run
FROM python:3@sha256:640956cce7e0dee7998246e103e830404e8677b8c2cdff603fff3962aa9a526d
RUN python -m pip install bottle
