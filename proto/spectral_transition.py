"""Stage 2 gate for the phase-limited FIR transition revision.

Implements docs/soothe3-transition-revision.md without queued subdivisions:
one latest target is acquired per actual Quality hop and at most one new IR is
synthesized. This is OUR transition approximation, not soothe equivalence.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys

import numpy as np
from scipy import optimize, signal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spectral_parity as sp  # noqa: E402


PHI_LIMIT = 2.0 * np.arccos(10.0 ** (-0.04 / 20.0))
PHASE_ERROR_MARGIN = 1.0e-2
ADMISSION_PHASE_LIMIT = PHI_LIMIT - PHASE_ERROR_MARGIN


@dataclass(frozen=True)
class LogEndpoint:
    ell: np.ndarray
    taps: np.ndarray
    reduction_db: np.ndarray
    effective_cap_db: float


@dataclass(frozen=True)
class Admission:
    endpoint: LogEndpoint
    alpha: float
    grid_phase_distance: float


def causal_log_endpoint(freq_hz: np.ndarray, reduction_db: np.ndarray,
                        effective_cap_db: float, fs: float, design_fft: int,
                        taps: int) -> LogEndpoint:
    """Return retained causal log spectrum and its truncated realized FIR."""
    reduction = sp._target_on_design_grid(freq_hz, reduction_db, fs, design_fft)
    reduction = np.minimum(reduction, effective_cap_db)
    log_half = np.log(10.0 ** (-reduction / 20.0))
    log_full = np.concatenate((log_half, log_half[-2:0:-1]))
    cepstrum = np.fft.ifft(log_full).real
    causal = np.zeros(design_fft)
    causal[0] = cepstrum[0]
    causal[1:design_fft // 2] = 2.0 * cepstrum[1:design_fft // 2]
    causal[design_fft // 2] = cepstrum[design_fft // 2]
    ell = np.fft.fft(causal)
    ell[0] = complex(ell[0].real, 0.0)
    ell[design_fft // 2] = complex(ell[design_fft // 2].real, 0.0)
    full = np.fft.ifft(np.exp(ell)).real
    return LogEndpoint(ell, full[:taps].copy(), reduction,
                       float(effective_cap_db))


def admit_one_hop(current: LogEndpoint, target: LogEndpoint,
                  phase_limit: float = PHI_LIMIT) -> Admission:
    """One phase-slew update; never wraps phase or queues intermediate IRs."""
    delta = target.ell - current.ell
    distance = float(np.max(np.abs(delta.imag)))
    alpha = 1.0 if distance == 0.0 else min(1.0, phase_limit / distance)
    ell = current.ell + alpha * delta
    ell[0] = complex(ell[0].real, 0.0)
    ell[len(ell) // 2] = complex(ell[len(ell) // 2].real, 0.0)
    full = np.fft.ifft(np.exp(ell)).real
    reduction = ((1.0 - alpha) * current.reduction_db
                 + alpha * target.reduction_db)
    cap = ((1.0 - alpha) * current.effective_cap_db
           + alpha * target.effective_cap_db)
    endpoint = LogEndpoint(ell, full[:len(current.taps)].copy(), reduction, cap)
    return Admission(endpoint, alpha, distance)


def exact_effective_cap_overshoot(A: np.ndarray, B: np.ndarray,
                                  cap_current_db: float,
                                  cap_next_db: float) -> tuple[float, float]:
    """Analytically minimize cap-normalized squared blend magnitude over t.

    Returns maximum cap overshoot in dB and the global minimizing blend weight.
    """
    A = np.asarray(A, dtype=np.complex128)
    B = np.asarray(B, dtype=np.complex128)
    D = B - A
    qa = np.abs(D) ** 2
    qb = 2.0 * np.real(np.conj(A) * D)
    qc = np.abs(A) ** 2
    cap_delta = cap_next_db - cap_current_db
    lam = (np.log(10.0) / 10.0) * cap_delta

    def normalized(t):
        q = qa * t * t + qb * t + qc
        return q * 10.0 ** ((cap_current_db + t * cap_delta) / 10.0)

    best = np.minimum(normalized(0.0), normalized(1.0))
    best_t = np.where(normalized(0.0) <= normalized(1.0), 0.0, 1.0)
    aa = lam * qa
    bb = 2.0 * qa + lam * qb
    cc = qb + lam * qc
    scale = np.maximum.reduce((np.abs(aa), np.abs(bb), np.abs(cc),
                               np.ones_like(aa)))
    quadratic = np.abs(aa) > 1e-14 * scale
    discriminant = bb * bb - 4.0 * aa * cc
    for sign in (-1.0, 1.0):
        root = np.full_like(qa, np.nan)
        valid_disc = quadratic & (discriminant >= 0.0)
        root[valid_disc] = (-bb[valid_disc] + sign * np.sqrt(discriminant[valid_disc])) / (2.0 * aa[valid_disc])
        valid = valid_disc & (root >= 0.0) & (root <= 1.0)
        value = normalized(np.where(valid, root, 0.0))
        improve = valid & (value < best)
        best[improve] = value[improve]
        best_t[improve] = root[improve]
    linear = ~quadratic & (np.abs(bb) > 1e-14 * scale)
    root = np.full_like(qa, np.nan)
    root[linear] = -cc[linear] / bb[linear]
    valid = linear & (root >= 0.0) & (root <= 1.0)
    value = normalized(np.where(valid, root, 0.0))
    improve = valid & (value < best)
    best[improve] = value[improve]
    best_t[improve] = root[improve]

    index = int(np.argmin(best))
    overshoot = max(0.0, float(-10.0 * np.log10(max(best[index], 1e-300))))
    return overshoot, float(best_t[index])


def endpoint_response(endpoint: LogEndpoint, fs: float,
                      wor_n: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    freq, response = signal.freqz(endpoint.taps, worN=wor_n, fs=fs)
    reduction = -20.0 * np.log10(np.maximum(np.abs(response), 1e-300))
    return freq, response, reduction


def pair_metrics(current: LogEndpoint, admitted: LogEndpoint,
                 fs: float, wor_n: int) -> dict[str, float]:
    freq, A, red_a = endpoint_response(current, fs, wor_n)
    _, B, red_b = endpoint_response(admitted, fs, wor_n)
    target_a = np.interp(freq, np.linspace(0.0, fs / 2.0,
                                           len(current.reduction_db)),
                         current.reduction_db)
    target_b = np.interp(freq, np.linspace(0.0, fs / 2.0,
                                           len(admitted.reduction_db)),
                         admitted.reduction_db)
    phase = np.abs(np.angle(B * np.conj(A)))
    cap_overshoot, cap_t = exact_effective_cap_overshoot(
        A, B, current.effective_cap_db, admitted.effective_cap_db)
    return {
        "phase_rad": float(np.max(phase)),
        "endpoint_cap_db": max(float(np.max(red_a - current.effective_cap_db)),
                               float(np.max(red_b - admitted.effective_cap_db))),
        "endpoint_ripple_db": max(float(np.max(-red_a)), float(np.max(-red_b))),
        "response_error_db": max(float(np.max(np.abs(red_a - target_a))),
                                 float(np.max(np.abs(red_b - target_b)))),
        "all_t_cap_db": cap_overshoot,
        "all_t_cap_t": cap_t,
    }


def refine_pair_metrics(current: LogEndpoint, admitted: LogEndpoint,
                        fs: float, wor_n: int) -> dict[str, float]:
    """Locally refine dense-grid extrema for actual phase and magnitude."""
    freq, A, red_a = endpoint_response(current, fs, wor_n)
    _, B, red_b = endpoint_response(admitted, fs, wor_n)
    grid = np.linspace(0.0, fs / 2.0, len(current.reduction_db))
    target_a = np.interp(freq, grid, current.reduction_db)
    target_b = np.interp(freq, grid, admitted.reduction_db)
    n_a = np.arange(len(current.taps))
    n_b = np.arange(len(admitted.taps))

    def response(taps, indices, hz):
        return np.dot(taps, np.exp(-2j * np.pi * hz * indices / fs))

    def local_max(values, evaluator):
        k = int(np.argmax(values))
        lo = freq[max(0, k - 1)]
        hi = freq[min(len(freq) - 1, k + 1)]
        if hi == lo:
            return float(values[k])
        result = optimize.minimize_scalar(lambda hz: -evaluator(hz),
                                          bounds=(lo, hi), method="bounded",
                                          options={"xatol": 1e-8})
        return max(float(values[k]), float(-result.fun))

    phase_values = abs(np.angle(B * np.conj(A)))
    phase = local_max(phase_values, lambda hz: abs(np.angle(
        response(admitted.taps, n_b, hz)
        * np.conj(response(current.taps, n_a, hz)))))

    cap_a_values = red_a - current.effective_cap_db
    cap_b_values = red_b - admitted.effective_cap_db
    cap_a = local_max(cap_a_values, lambda hz: (
        -20.0 * np.log10(max(abs(response(current.taps, n_a, hz)), 1e-300))
        - current.effective_cap_db))
    cap_b = local_max(cap_b_values, lambda hz: (
        -20.0 * np.log10(max(abs(response(admitted.taps, n_b, hz)), 1e-300))
        - admitted.effective_cap_db))

    ripple_a = local_max(-red_a, lambda hz: 20.0 * np.log10(max(
        abs(response(current.taps, n_a, hz)), 1e-300)))
    ripple_b = local_max(-red_b, lambda hz: 20.0 * np.log10(max(
        abs(response(admitted.taps, n_b, hz)), 1e-300)))

    error_a_values = abs(red_a - target_a)
    error_b_values = abs(red_b - target_b)
    error_a = local_max(error_a_values, lambda hz: abs(
        -20.0 * np.log10(max(abs(response(current.taps, n_a, hz)), 1e-300))
        - np.interp(hz, grid, current.reduction_db)))
    error_b = local_max(error_b_values, lambda hz: abs(
        -20.0 * np.log10(max(abs(response(admitted.taps, n_b, hz)), 1e-300))
        - np.interp(hz, grid, admitted.reduction_db)))
    return {"phase_rad": phase, "endpoint_cap_db": max(cap_a, cap_b),
            "endpoint_ripple_db": max(ripple_a, ripple_b),
            "response_error_db": max(error_a, error_b)}


def test_oracle() -> dict[str, float]:
    rng = np.random.default_rng(778)
    max_scipy = max_dense = max_cosine_violation = max_bound_overshoot = 0.0
    for cap_a in (0.0, 6.0, 12.0, 20.0, 40.0):
        for cap_b in (0.0, 6.0, 12.0, 20.0, 40.0):
            for trial in range(8):
                mag_a = 10.0 ** (-rng.uniform(0.0, cap_a + 0.01) / 20.0)
                mag_b = 10.0 ** (-rng.uniform(0.0, cap_b + 0.01) / 20.0)
                phase_a = rng.uniform(-np.pi, np.pi)
                separation = (-PHI_LIMIT, PHI_LIMIT, 0.0,
                              rng.uniform(-PHI_LIMIT, PHI_LIMIT))[trial % 4]
                if trial < 2:  # exact endpoint floors + phase extremes
                    mag_a = 10.0 ** (-(cap_a + 0.01) / 20.0)
                    mag_b = 10.0 ** (-(cap_b + 0.01) / 20.0)
                A = mag_a * np.exp(1j * phase_a)
                B = mag_b * np.exp(1j * (phase_a + separation))
                exact, _ = exact_effective_cap_overshoot(
                    np.array([A]), np.array([B]), cap_a, cap_b)
                max_bound_overshoot = max(max_bound_overshoot, exact)

                def objective(t):
                    blend = (1.0 - t) * A + t * B
                    cap = (1.0 - t) * cap_a + t * cap_b
                    return abs(blend) ** 2 * 10.0 ** (cap / 10.0)

                result = optimize.minimize_scalar(objective, bounds=(0.0, 1.0),
                                                  method="bounded",
                                                  options={"xatol": 1e-14})
                candidates = [objective(0.0), objective(1.0), result.fun]
                reference = max(0.0, -10.0 * np.log10(min(candidates)))
                max_scipy = max(max_scipy, abs(exact - reference))
                ts = np.linspace(0.0, 1.0, 100_001)
                dense_blend = (1.0 - ts) * A + ts * B
                dense_caps = (1.0 - ts) * cap_a + ts * cap_b
                dense_min = float(np.min(abs(dense_blend) ** 2
                                         * 10.0 ** (dense_caps / 10.0)))
                dense = max(0.0, -10.0 * np.log10(dense_min))
                max_dense = max(max_dense, abs(exact - dense))
                for t in np.linspace(0.0, 1.0, 101):
                    lhs = abs((1.0 - t) * A + t * B)
                    rhs = ((1.0 - t) * mag_a + t * mag_b) * np.cos(PHI_LIMIT / 2.0)
                    max_cosine_violation = max(max_cosine_violation, rhs - lhs)
    assert max_scipy < 1e-9 and max_dense < 1e-8
    assert max_cosine_violation <= 1e-12
    assert max_bound_overshoot <= 0.05 + 1e-12
    return {"oracle_scipy_db": max_scipy, "oracle_dense_db": max_dense,
            "cosine_bound_violation": max_cosine_violation,
            "maximum_bound_overshoot_db": max_bound_overshoot,
            "phi_limit_rad": PHI_LIMIT}


def test_falling_cap() -> dict[str, float]:
    fs = 48_000.0
    failures = {}
    corrected = {}
    for hop in (256, 128, 64):
        q = np.arange(hop)
        request = 6.0 + 34.0 * np.exp(-q / 960.0)
        next_cap = float(6.0 + 34.0 * np.exp(-hop / 960.0))
        t = 0.5 - 0.5 * np.cos(np.pi * q / hop)
        gain = ((1.0 - t) * 10.0 ** (-40.0 / 20.0)
                + t * 10.0 ** (-next_cap / 20.0))
        reduction = -20.0 * np.log10(gain)
        failures[hop] = float(np.max(reduction - request))
        effective = (1.0 - t) * 40.0 + t * next_cap
        corrected[hop] = float(np.max(reduction - effective))
    expected = {256: 0.6508654586, 128: 0.3904173546, 64: 0.2152790070}
    reproduction = max(abs(failures[h] - expected[h]) for h in expected)
    assert reproduction < 1e-9
    assert max(corrected.values()) <= 0.05

    # Repeated falling requests: phase-free alpha=1, newest request each hop,
    # no queue. Effective cap is distinct during fades and converges.
    cap = 40.0
    max_corrected = 0.0
    max_request_gap = 0.0
    for step in range(1, 161):
        request = 6.0 + 34.0 * np.exp(-(step * 64) / 960.0)
        A = np.array([10.0 ** (-cap / 20.0) + 0j])
        B = np.array([10.0 ** (-request / 20.0) + 0j])
        over, _ = exact_effective_cap_overshoot(A, B, cap, request)
        max_corrected = max(max_corrected, over)
        max_request_gap = max(max_request_gap, cap - request)
        cap = request
    settling_error = abs(cap - 6.0)
    assert max_corrected <= 0.05 and max_request_gap > 0.0
    assert settling_error < 0.001
    return {"reproduction_db": reproduction,
            "failure_h256_db": failures[256], "failure_h128_db": failures[128],
            "failure_h64_db": failures[64],
            "corrected_max_db": max(max(corrected.values()), max_corrected),
            "requested_effective_gap_db": max_request_gap,
            "fixed_target_settling_db": settling_error}


def make_targets(fs: float, M: int) -> dict[str, np.ndarray]:
    f = np.fft.rfftfreq(M, 1.0 / fs)
    gaussian = lambda f0, width, depth: depth * np.exp(
        -0.5 * ((f - f0) / (width / 2.355)) ** 2)
    return {
        "unity": np.zeros_like(f),
        "low_shelf40": 40.0 / (1.0 + (f / 1000.0) ** 4),
        "high_shelf40": 40.0 / (1.0 + (10000.0 / np.maximum(f, 1e-9)) ** 4),
        "notch54": gaussian(3000.0, 54.0, 40.0),
        "broad": gaussian(3000.0, 500.0, 20.0),
        "harmonics": np.minimum(40.0, sum(
            gaussian(k * 440.0, 60.0 + 4 * k, 16.0 / np.sqrt(k))
            for k in range(1, 18))),
        "halfbin_a": gaussian(3000.0 + 0.5 * fs / M, 54.0, 40.0),
        "halfbin_b": gaussian(5000.0 - 0.5 * fs / M, 54.0, 40.0),
    }


def test_fir_admission() -> dict[str, float]:
    worst = {"phase_rad": 0.0, "endpoint_cap_db": 0.0,
             "endpoint_ripple_db": 0.0, "response_error_db": 0.0,
             "all_t_cap_db": 0.0}
    min_alpha = 1.0
    max_moving_target_error_db = 0.0
    max_effective_cap_lag_db = 0.0
    synthesized = acquired = 0
    failure = None
    nominal_phase_excess = 0.0
    worst_pairs = {}
    for fs in (44_100.0, 48_000.0, 96_000.0, 192_000.0):
        M, L = sp.capacity_for_sample_rate(fs)
        f = np.fft.rfftfreq(M, 1.0 / fs)
        targets = make_targets(fs, M)
        # Opposing/per-channel and moving targets; alternating shelves exercises
        # immediate replacement of a previous direction at every actual hop.
        sequence = [("unity", 40.0), ("low_shelf40", 40.0),
                    ("high_shelf40", 40.0), ("notch54", 6.0),
                    ("notch54", 12.0), ("notch54", 20.0),
                    ("notch54", 40.0), ("broad", 20.0),
                    ("harmonics", 40.0), ("halfbin_a", 40.0),
                    ("halfbin_b", 40.0), ("low_shelf40", 40.0),
                    ("high_shelf40", 40.0), ("low_shelf40", 40.0),
                    ("unity", 6.0)]
        for hop in (256, 128, 64):
            # Diagnostic pre-pass with the rejected nominal grid limit. It is
            # separate from the one-synthesis-per-hop candidate below.
            nominal_current = causal_log_endpoint(
                f, targets["unity"], 40.0, fs, M, L)
            for name, requested_cap in sequence:
                nominal_target = causal_log_endpoint(
                    f, targets[name], requested_cap, fs, M, L)
                nominal = admit_one_hop(nominal_current, nominal_target,
                                        PHI_LIMIT)
                nominal_metrics = pair_metrics(
                    nominal_current, nominal.endpoint, fs,
                    max(65_537, 4 * M + 1))
                nominal_phase_excess = max(
                    nominal_phase_excess,
                    nominal_metrics["phase_rad"] - PHI_LIMIT)
                nominal_current = nominal.endpoint

            current = causal_log_endpoint(f, targets["unity"], 40.0, fs, M, L)
            for name, requested_cap in sequence:
                target = causal_log_endpoint(
                    f, targets[name], requested_cap, fs, M, L)
                acquired += 1
                admission = admit_one_hop(current, target,
                                          ADMISSION_PHASE_LIMIT)
                synthesized += 1
                min_alpha = min(min_alpha, admission.alpha)
                max_moving_target_error_db = max(
                    max_moving_target_error_db,
                    float(np.max(abs(admission.endpoint.reduction_db
                                     - target.reduction_db))))
                max_effective_cap_lag_db = max(
                    max_effective_cap_lag_db,
                    abs(admission.endpoint.effective_cap_db
                        - target.effective_cap_db))
                metrics = pair_metrics(current, admission.endpoint, fs,
                                       max(65_537, 4 * M + 1))
                for key in worst:
                    if metrics[key] > worst[key]:
                        worst[key] = metrics[key]
                        if key != "all_t_cap_db":
                            worst_pairs[key] = (current, admission.endpoint, fs,
                                                max(65_537, 4 * M + 1))
                if (metrics["endpoint_cap_db"] > 0.01
                        or metrics["endpoint_ripple_db"] > 0.01
                        or metrics["response_error_db"] > 0.25
                        or metrics["phase_rad"] > PHI_LIMIT
                        or metrics["all_t_cap_db"] > 0.05):
                    failure = (fs, hop, f"{name}/cap{requested_cap:g}",
                               admission.alpha, metrics)
                    break
                current = admission.endpoint
            if failure:
                break
        if failure:
            break
    # Refine only the pairs that set a global dense phase/magnitude maximum;
    # this covers local extrema without turning every target into an optimizer.
    for key, args in worst_pairs.items():
        refined = refine_pair_metrics(*args)
        worst[key] = max(worst[key], refined[key])
    assert synthesized == acquired
    assert nominal_phase_excess > 0.0
    assert nominal_phase_excess < PHASE_ERROR_MARGIN, (
        f"nominal phase excess {nominal_phase_excess} exceeds fixed reserve "
        f"{PHASE_ERROR_MARGIN}")
    if failure:
        fs, hop, name, alpha, metrics = failure
        raise AssertionError(f"admission failed fs={fs} H={hop} target={name} "
                             f"alpha={alpha}: {metrics}")
    assert worst["endpoint_cap_db"] <= 0.01
    assert worst["endpoint_ripple_db"] <= 0.01
    assert worst["response_error_db"] <= 0.25
    assert worst["phase_rad"] <= PHI_LIMIT
    assert worst["all_t_cap_db"] <= 0.05
    # Fixed-target settling follows the predicted ceil(D0/limit), without a
    # queued path. Each hop synthesizes once from the same retained target.
    fs = 48_000.0
    M, L = sp.capacity_for_sample_rate(fs)
    f = np.fft.rfftfreq(M, 1.0 / fs)
    targets = make_targets(fs, M)
    fixed_current = causal_log_endpoint(f, targets["low_shelf40"], 40.0,
                                        fs, M, L)
    fixed_target = causal_log_endpoint(f, targets["high_shelf40"], 40.0,
                                       fs, M, L)
    initial_distance = float(np.max(abs((fixed_target.ell - fixed_current.ell).imag)))
    predicted_hops = int(np.ceil(initial_distance / ADMISSION_PHASE_LIMIT))
    fixed_hops = 0
    while True:
        step = admit_one_hop(fixed_current, fixed_target,
                             ADMISSION_PHASE_LIMIT)
        fixed_hops += 1
        fixed_current = step.endpoint
        if step.alpha == 1.0:
            break
        assert fixed_hops <= predicted_hops
    fixed_error = float(np.max(abs(fixed_current.reduction_db
                                   - fixed_target.reduction_db)))
    assert fixed_hops == predicted_hops and fixed_error < 1e-12

    return {**worst, "phase_margin_rad": PHI_LIMIT - worst["phase_rad"],
            "nominal_phase_excess_rad": nominal_phase_excess,
            "fixed_admission_reserve_rad": PHASE_ERROR_MARGIN,
            "min_alpha": min_alpha,
            "moving_target_error_db": max_moving_target_error_db,
            "effective_cap_lag_db": max_effective_cap_lag_db,
            "target_age_at_acquisition_hops": 0.0,
            "fixed_target_predicted_hops": float(predicted_hops),
            "fixed_target_actual_hops": float(fixed_hops),
            "fixed_target_final_error_db": fixed_error,
            "targets_acquired": float(acquired), "irs_synthesized": float(synthesized)}


def test_small_fir_homotopy() -> dict[str, float]:
    old = np.array([0.80, 0.10, 0.015])
    new = np.array([0.78, 0.11, 0.020])
    w = np.linspace(0.0, np.pi, 65537)
    _, A = signal.freqz(old, worN=w)
    _, B = signal.freqz(new, worN=w)
    phase = float(np.max(abs(np.angle(B * np.conj(A)))))
    assert old[0] > 0.0 and new[0] > 0.0 and phase < PHI_LIMIT
    max_root = 0.0
    scipy_root_error = 0.0
    min_magnitude = np.inf
    for t in np.linspace(0.0, 1.0, 1001):
        h = (1.0 - t) * old + t * new
        roots = np.sort_complex(np.roots(h))
        scipy_roots, _, _ = signal.tf2zpk(h, [1.0])
        scipy_root_error = max(scipy_root_error,
                               float(np.max(abs(roots - np.sort_complex(scipy_roots)))))
        max_root = max(max_root, float(np.max(abs(roots))))
        _, H = signal.freqz(h, worN=w[::256])
        min_magnitude = min(min_magnitude, float(np.min(abs(H))))
    assert scipy_root_error < 1e-12 and max_root < 1.0 and min_magnitude > 0.0
    return {"phase_rad": phase, "max_root_radius": max_root,
            "scipy_root_error": scipy_root_error,
            "minimum_unit_circle_magnitude": min_magnitude}


def test_history_activation() -> dict[str, float]:
    rng = np.random.default_rng(991)
    P, L = sp.PARTITION, 8 * sp.PARTITION
    old = rng.normal(0.0, 0.03, L) * np.exp(-np.arange(L) / 80.0)
    new = rng.normal(0.0, 0.03, L) * np.exp(-np.arange(L) / 70.0)
    # Non-host-aligned callback boundaries, while admission is internal-P aligned
    # after enough blocks to wrap the J+1 history ring.
    max_error = max_endpoint = max_activation = 0.0
    for H in (256, 128, 64):
        start = 12 * P
        x = np.r_[rng.normal(0.0, 0.2, start + H + 333), np.zeros(L - 1)]
        old_bank = sp.CausalPartitionedFir(old, P)
        new_bank = sp.CausalPartitionedFir(old, P)
        full_new = sp.CausalPartitionedFir(new, P)
        chunks = [17, 91, 3, 205, 64, 11, 129, 7, 333, 2, 700, 401, 1000]
        y = np.empty_like(x); reference = np.empty_like(x)
        old_direct = sp.direct_fir(x, old)
        new_direct = sp.direct_fir(x, new)
        at = 0
        activated = False
        for size in chunks + [len(x)]:
            if at >= len(x):
                break
            stop = min(len(x), at + size)
            # Split a host chunk at the internal admission boundary; production
            # does the same with its P counter, independent of callback edges.
            boundaries = [at]
            if at < start < stop:
                boundaries.append(start)
            boundaries.append(stop)
            for lo, hi in zip(boundaries[:-1], boundaries[1:]):
                if lo == start and not activated:
                    new_bank.replace_kernel_at_boundary(new)
                    activated = True
                old_piece = old_bank.process(x[lo:hi])
                new_piece = new_bank.process(x[lo:hi])
                full_piece = full_new.process(x[lo:hi])
                if activated:
                    max_activation = max(max_activation,
                                         float(np.max(abs(new_piece - full_piece))))
                indices = np.arange(lo, hi)
                q = indices - start
                t = np.where(q < 0, 0.0, np.where(q >= H, 1.0,
                             0.5 - 0.5 * np.cos(np.pi * q / H)))
                y[lo:hi] = (1.0 - t) * old_piece + t * new_piece
                reference[lo:hi] = ((1.0 - t) * old_direct[lo:hi]
                                    + t * new_direct[lo:hi])
            at = stop
        error = float(np.max(np.abs(y - reference)))
        endpoint_error = max(abs(y[start] - old_direct[start]),
                             abs(y[start + H] - new_direct[start + H]))
        max_error = max(max_error, error)
        max_endpoint = max(max_endpoint, endpoint_error)
        assert activated and error < 1e-10 and endpoint_error < 1e-10
    assert max_activation < 1e-10
    return {"history_direct_error": max_error,
            "new_bank_overlap_error": max_activation,
            "boundary_endpoint_error": max_endpoint,
            "extra_buffer_delay": 0.0}


def run_all_gates() -> dict[str, dict[str, float]]:
    tests = [("test 1 exact oracle and cosine", test_oracle),
             ("test 2 falling effective cap", test_falling_cap),
             ("test 3 finite-FIR admission", test_fir_admission),
             ("test 4 history activation", test_history_activation),
             ("test 5 small-FIR homotopy", test_small_fir_homotopy)]
    results = {}
    for name, test in tests:
        try:
            metrics = test()
            results[name] = metrics
            print(name + ": PASS (" + ", ".join(
                f"{key}={value:.12g}" for key, value in metrics.items()) + ")")
        except Exception as exc:
            print(f"{name}: FAIL ({exc})")
            print("\nVERDICT: FAIL -- no transition fixture export or C++ port authorized")
            raise
    print("\nVERDICT: PASS -- revised phase-limited transition numeric gates pass; "
          "C++ complete-path CPU remains a separate Stage 3 gate")
    return results


if __name__ == "__main__":
    run_all_gates()
