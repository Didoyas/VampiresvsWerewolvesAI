"""Starting a run from weights that were already trained."""

import pytest

torch = pytest.importorskip("torch")

from config import PRESETS
from vvw.network import PolicyValueNet


def _smoke_network():
    net = PRESETS["smoke"].network
    return PolicyValueNet(
        channels=net.channels, blocks=net.blocks, head_hidden=net.head_hidden
    )


def _save_best_shaped(path):
    """A `best.pt` as `_evaluate_and_promote` writes it: no optimiser state."""
    torch.save(
        {
            "network": _smoke_network().state_dict(),
            "step": 500_096,
            "evaluation": {"games": 40, "mean_margin": 3.075},
        },
        path,
    )


def test_best_pt_carries_no_optimiser_state(tmp_path):
    """Which is why --resume cannot read one, and --init-from exists."""
    seed = tmp_path / "best.pt"
    _save_best_shaped(seed)
    assert set(torch.load(seed, map_location="cpu")) == {
        "network",
        "step",
        "evaluation",
    }


def test_init_from_loads_the_weights_and_skips_distillation(tmp_path, monkeypatch):
    """The weights already contain the warm start; redoing it would undo
    whatever PPO learned on top of the heuristic."""
    import train

    seed = tmp_path / "best.pt"
    _save_best_shaped(seed)
    expected = torch.load(seed, map_location="cpu")["network"]

    calls = []
    monkeypatch.setattr(train, "distill", lambda *a, **k: calls.append(1))

    loaded = {}
    original = PolicyValueNet.load_state_dict

    def spy(self, state, *args, **kwargs):
        loaded["state"] = state
        return original(self, state, *args, **kwargs)

    monkeypatch.setattr(PolicyValueNet, "load_state_dict", spy)

    assert (
        train.main(
            [
                "--preset", "smoke",
                "--init-from", str(seed),
                "--output", str(tmp_path / "run"),
                "--total-steps", "200",
                "--distill-iterations", "25",
            ]
        )
        == 0
    )
    assert calls == [], "distillation must not run on top of trained weights"
    assert loaded["state"].keys() == expected.keys()
    for name, tensor in expected.items():
        assert torch.equal(loaded["state"][name].cpu(), tensor)


def test_without_init_from_distillation_still_runs(tmp_path, monkeypatch):
    import train

    calls = []
    monkeypatch.setattr(train, "distill", lambda *a, **k: calls.append(1))
    assert (
        train.main(
            [
                "--preset", "smoke",
                "--output", str(tmp_path / "run"),
                "--total-steps", "200",
                "--distill-iterations", "25",
            ]
        )
        == 0
    )
    assert calls == [1], "a fresh network still needs the warm start"


# ---- what `best.pt` is chosen on -------------------------------------------


def test_the_evaluation_reports_a_bounded_score(tmp_path):
    """Raw per-game margins reach +/-294 on a wipe-out, so a 120-game mean
    is largely decided by which evaluation drew the blow-out."""
    from dataclasses import replace

    from config import PRESETS
    from train import evaluate_against_heuristic

    cfg = replace(PRESETS["smoke"])
    report = evaluate_against_heuristic(_smoke_network(), cfg, maps=2)
    assert "mean_normalised_margin" in report
    assert "outcome_score" in report
    assert -1.0 <= report["mean_normalised_margin"] <= 1.0
    assert -1.0 <= report["outcome_score"] <= 1.0


def test_best_pt_follows_the_bounded_score_not_the_raw_margin(tmp_path, monkeypatch):
    """The two can disagree, and when they do the bounded one decides."""
    import train
    from config import PRESETS

    reports = [
        # A huge raw margin from one blow-out, but losing on balance.
        {"games": 40, "mean_margin": 100.0, "mean_normalised_margin": -0.5},
        # Modest creature counts, but ahead in game after game.
        {"games": 40, "mean_margin": 0.0, "mean_normalised_margin": +0.5},
    ]
    monkeypatch.setattr(
        train, "evaluate_against_heuristic", lambda *a, **k: dict(reports.pop(0))
    )

    network, history, best = _smoke_network(), [], float("-inf")
    for step in (1_000, 2_000):
        best = train._evaluate_and_promote(
            network, PRESETS["smoke"], tmp_path, step, history, best, "eval"
        )

    saved = torch.load(tmp_path / "best.pt", map_location="cpu")
    assert saved["step"] == 2_000, "the blow-out evaluation should not have won"
    assert best == pytest.approx(0.5)
