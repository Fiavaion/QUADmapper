"""models_dir(): $QM_MODELS_DIR, then the installed ai/models beside the sidecar
folder (ADR-057), then %LOCALAPPDATA%/QUADmapper/models. GPU-free."""

import os

import models


def _fake_install(tmp_path, monkeypatch, with_models):
    sidecar_dir = tmp_path / "ai" / "sidecar"
    sidecar_dir.mkdir(parents=True)
    if with_models:
        (tmp_path / "ai" / "models").mkdir()
    monkeypatch.setattr(models, "__file__", str(sidecar_dir / "models.py"))
    return tmp_path / "ai" / "models"


def test_override_wins_over_bundled_models(tmp_path, monkeypatch):
    _fake_install(tmp_path, monkeypatch, with_models=True)
    monkeypatch.setenv("QM_MODELS_DIR", str(tmp_path / "elsewhere"))
    assert models.models_dir() == str(tmp_path / "elsewhere")


def test_bundled_models_used_when_present_and_no_override(tmp_path, monkeypatch):
    bundled = _fake_install(tmp_path, monkeypatch, with_models=True)
    monkeypatch.delenv("QM_MODELS_DIR", raising=False)
    assert models.models_dir() == str(bundled)


def test_local_app_data_default_without_bundled_models(tmp_path, monkeypatch):
    _fake_install(tmp_path, monkeypatch, with_models=False)
    monkeypatch.delenv("QM_MODELS_DIR", raising=False)
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "lad"))
    assert models.models_dir() == os.path.join(str(tmp_path / "lad"), "QUADmapper", "models")
