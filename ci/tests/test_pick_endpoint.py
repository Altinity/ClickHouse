"""Endpoint picker: Altinity dataset mirror vs Inc public S3."""

import os
import sys
import urllib.error
from email.message import EmailMessage

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

from ci.jobs.scripts import pick_endpoint
from ci.jobs.scripts.pick_endpoint import (
    MIRROR_DATASETS_WEB,
    PUBLIC_DATASETS_WEB,
    PUBLIC_TPCDS,
    pick,
    rewritten_create_sql,
)


def test_picks_faster_mirror(monkeypatch):
    def fake_probe(url, timeout=2.0):
        return 0.05 if url == MIRROR_DATASETS_WEB else 0.40

    monkeypatch.setattr(pick_endpoint, "_probe", fake_probe)
    assert pick("datasets-web") == MIRROR_DATASETS_WEB


def test_public_wins_when_mirror_is_dead(monkeypatch):
    def fake_probe(url, timeout=2.0):
        return None if "dockerhub-proxy" in url else 0.12

    monkeypatch.setattr(pick_endpoint, "_probe", fake_probe)
    assert pick("datasets-web") == PUBLIC_DATASETS_WEB
    assert pick("tpcds") == PUBLIC_TPCDS


def test_mirror_wins_when_public_is_dead(monkeypatch):
    def fake_probe(url, timeout=2.0):
        return 0.08 if "dockerhub-proxy" in url else None

    monkeypatch.setattr(pick_endpoint, "_probe", fake_probe)
    assert pick("tpcds") == pick_endpoint.MIRROR_TPCDS


def test_both_fail_raises(monkeypatch):
    monkeypatch.setattr(pick_endpoint, "_probe", lambda *a, **k: None)
    with pytest.raises(RuntimeError, match="datasets-web"):
        pick("datasets-web")


def test_unknown_pair_raises():
    with pytest.raises(ValueError, match="unknown endpoint pair"):
        pick("not-a-pair")


def test_http_error_counts_as_reachable(monkeypatch):
    def boom(req, timeout=2.0):
        raise urllib.error.HTTPError(
            req.full_url, 403, "Forbidden", hdrs=EmailMessage(), fp=None
        )

    monkeypatch.setattr(pick_endpoint.urllib.request, "urlopen", boom)
    elapsed = pick_endpoint._probe(PUBLIC_DATASETS_WEB, timeout=0.5)
    assert elapsed is not None
    assert elapsed < 0.5


def test_rewritten_create_sql_swaps_only_the_web_base():
    sql = (
        "endpoint = '"
        + PUBLIC_DATASETS_WEB
        + "/store/abc/');\n"
        "endpoint = '"
        + PUBLIC_DATASETS_WEB
        + "/store/def/');\n"
    )
    out = rewritten_create_sql(sql, MIRROR_DATASETS_WEB)
    assert PUBLIC_DATASETS_WEB not in out
    assert out.count(MIRROR_DATASETS_WEB) == 2
    assert rewritten_create_sql(sql, PUBLIC_DATASETS_WEB) == sql


def test_create_sql_defaults_to_public_s3():
    text = open("tests/docker_scripts/create.sql", encoding="utf-8").read()
    assert PUBLIC_DATASETS_WEB in text
    assert "dockerhub-proxy" not in text


def test_create_tpcds_defaults_to_public_s3():
    text = open("tests/docker_scripts/create_tpcds.sh", encoding="utf-8").read()
    assert PUBLIC_TPCDS in text
    assert "dockerhub-proxy" not in text


def test_create_sql_cli(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(pick_endpoint, "pick", lambda name, timeout=2.0: MIRROR_DATASETS_WEB)
    src = tmp_path / "create.sql"
    src.write_text(f"disk = disk(type = web, endpoint = '{PUBLIC_DATASETS_WEB}/store/x/'));\n")
    assert pick_endpoint.main(["create-sql", str(src)]) == 0
    assert MIRROR_DATASETS_WEB in capsys.readouterr().out
