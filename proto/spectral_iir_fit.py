"""Stage 2A only: test the bounded cut-only fitter before any scheduling work.

The source target is sampled independently on <=8193 bins and interpolated in
log frequency. Bell tails can overcut a protected gap even when every section
is stable and individually attenuating; fitting error therefore stays signed.
Run: ./proto/.venv/bin/python proto/spectral_iir_fit.py
"""
from dataclasses import dataclass

import numpy as np
from scipy import signal
from scipy.optimize import minimize_scalar

from matched_kernel import ivantsov_2nd

FS = 48000.0
SIGMA = 2.0283139124815617  # MatchedCoeffs.h verified recipe constant.
LIMIT = 0.25


@dataclass(frozen=True)
class Section:
    f: float
    q: float
    d: float
    kind: str = "bell"

    def sos(self):
        if self.d == 0.0:
            return np.array([1., 0., 0., 1., 0., 0.])
        b0, b1, b2, a1, a2 = ivantsov_2nd(
            self.kind, FS, self.f, self.q, -self.d, SIGMA)
        return np.array([b0, b1, b2, 1., a1, a2])


def response(section, f):
    """Explicit coefficient polynomial; SciPy is reserved as an oracle."""
    b0, b1, b2, _, a1, a2 = section.sos()
    z = np.exp(-2j * np.pi * np.asarray(f) / FS)
    return -20 * np.log10(np.abs((b0 + b1*z + b2*z*z) /
                                (1 + a1*z + a2*z*z)))


def bank_response(sections, f, flat=0.):
    return flat + sum((response(s, f) for s in sections),
                      start=np.zeros_like(np.asarray(f), dtype=float))


class Target:
    def __init__(self, name, analytic, protected=None):
        self.name = name
        self.analytic = analytic
        self.f = np.r_[0., np.geomspace(20., FS/2, 8191), FS/2]
        self.f = np.unique(self.f)
        self.r = np.maximum(np.asarray(analytic(self.f)), 0.)
        self.protected = protected
        assert len(self.f) <= 8193 and np.all(np.isfinite(self.r))
        assert np.min(self.r) >= -1e-9 and np.max(self.r) <= 40. + 1e-8

    def at(self, f):
        f = np.asarray(f)
        # Endpoints stay explicit: log(0) is never evaluated.
        out = np.interp(np.log(np.maximum(f, 20.)), np.log(self.f[1:]), self.r[1:])
        return np.where(f == 0., self.r[0], out)


def peaks(f, r):
    """One source scan, insertion-limited best 64; midpoint for plateaus."""
    best = []
    i = 1
    while i < len(r)-1:
        j = i
        while j+1 < len(r)-1 and r[j+1] == r[i]:
            j += 1
        if r[i] > r[i-1] and r[j] > r[j+1]:
            mid = (i+j)//2
            item = (float(r[mid]), float(f[mid]), mid)
            pos = 0
            while pos < len(best) and (-best[pos][0], best[pos][1]) <= (-item[0], item[1]):
                pos += 1
            if pos < 64:
                best.insert(pos, item)
                del best[64:]
        i = j+1
    return best


def half_crossings(f, r, idx):
    level = r[idx]/2
    lo = hi = idx
    while lo > 0 and r[lo-1] > level:
        lo -= 1
    while hi+1 < len(r) and r[hi+1] > level:
        hi += 1

    def crossing(i, j):
        if f[i] <= 0. or r[i] == r[j]:
            return float(f[j])
        alpha = (level-r[i])/(r[j]-r[i])
        return float(np.exp(np.log(f[i]) + alpha*np.log(f[j]/f[i])))

    left = f[0] if lo == 0 else crossing(lo-1, lo)
    right = f[-1] if hi == len(r)-1 else crossing(hi, hi+1)
    return max(float(left), 20.), min(float(right), 20000.)


def fitting_grid(target):
    f = list(np.geomspace(20., 20000., 513)) + [0., FS/2]
    for _, center, i in peaks(target.f, target.r):
        left, right = half_crossings(target.f, target.r, i)
        f.extend((center, left, right))
    f = np.unique(f)
    assert len(f) <= 705
    return f


def solve_depth(kind, center, q, desired):
    """Exactly eight bisections; test monotonicity before trusting the solve."""
    ds = np.linspace(0., 40., 33)
    attenuation = np.array([response(Section(center, q, d, kind), center) for d in ds])
    assert np.all(np.diff(attenuation) >= -1e-8), (kind, center, q)
    low, high = 0., 40.
    for _ in range(8):
        middle = (low+high)/2
        if response(Section(center, q, middle, kind), center) < desired:
            low = middle
        else:
            high = middle
    return (low+high)/2


def fit(target, k):
    f = fitting_grid(target)
    r = target.at(f)
    flat = float(np.min(target.r))  # Whole target, never just the cut mask.
    a = np.full_like(f, flat)
    sections = []
    # Two reserved shelf identities count toward K even when unused.
    for endpoint, kind in ((0, "low_shelf"), (-1, "high_shelf")):
        if r[endpoint]-a[endpoint] > 0.05:
            inward = np.arange(len(f)) if endpoint == 0 else np.arange(len(f)-1, -1, -1)
            residual = r-a
            below = inward[residual[inward] <= residual[endpoint]/2]
            center = float(np.clip(f[below[0]] if len(below) else 20000., 20., 20000.))
            # Shelf midpoint response solves twice the required endpoint reduction.
            d = solve_depth(kind, center, 1/np.sqrt(2), residual[endpoint]/2)
            s = Section(center, 1/np.sqrt(2), d, kind)
            sections.append(s)
            a += response(s, f)
    active = (f >= 20.) & (f <= 20000.)
    for _ in range(k-2):
        residual = r-a
        idx = np.where(active)[0][np.argmax(residual[active])]
        if residual[idx] <= .05:
            break
        left, right = half_crossings(f, residual, idx)
        bw = max(np.log2(right/left), 1e-12)
        q0 = np.clip(1/(2*np.sinh(np.log(2)*bw/2)), .25, 128.)
        center = float(f[idx])
        probes = np.clip(center*2.**(bw*np.array([0., -.25, .25, -.5, .5, -1., 1., -2., 2.])), 20., 20000.)
        base = bank_response(sections, probes, flat)
        candidates = []
        for q in np.unique(np.clip([q0/np.sqrt(2), q0, q0*np.sqrt(2)], .25, 128.)):
            d = solve_depth("bell", center, q, float(residual[idx]))
            s = Section(center, float(q), d)
            error = base + response(s, probes) - target.at(probes)
            score = np.sum(error**2 + 3*np.maximum(error, 0.)**2)
            candidates.append((float(score), float(q), d, s))
        winner = min(candidates, key=lambda x: x[:3])[3]
        sections.append(winner)
        a += response(winner, f)
    dropped = sum(float((target.at(p)-bank_response(sections, p, flat))) > .05
                  for _, p, _ in peaks(target.f, target.r))
    return sections, flat, len(f), dropped


def independent_check(sections, f, flat):
    if not sections:
        return np.full_like(f, flat), 1., 1., 0.
    sos = np.array([s.sos() for s in sections])
    _, h = signal.sosfreqz(sos, worN=2*np.pi*f/FS)
    scipy_a = flat-20*np.log10(np.abs(h))
    agreement = float(np.max(np.abs(scipy_a-bank_response(sections, f, flat))))
    assert agreement <= 1e-10, agreement
    pm = zm = 1.
    for row in sos:
        b0, b1, b2, _, a1, a2 = row
        assert np.all(np.isfinite(row)) and b0 > 0
        poles = np.roots([1., a1, a2])
        zeros = np.roots([b0, b1, b2])
        pm, zm = min(pm, 1-max(abs(poles))), min(zm, 1-max(abs(zeros)))
        for c1, c2 in ((a1, a2), (b1/b0, b2/b0)):
            assert min(1+c1+c2, 1-c1+c2, 1-c2) > 0.
    assert pm > 0 and zm > 0, (pm, zm)
    return scipy_a, pm, zm, agreement


def validation_grid(sections):
    f = list(np.linspace(0., FS/2, 65537))
    for s in sections:
        lo, hi = max(20., s.f/2), min(FS/2, s.f*2)
        f.extend(np.geomspace(lo, hi, 257))
        for sign in (-1., 1.):
            result = minimize_scalar(lambda x: sign*float(bank_response(sections, x)),
                                     bounds=(lo, hi), method="bounded",
                                     options={"maxiter": 64, "xatol": 1e-8})
            f.append(float(result.x))
    return np.unique(f)


def run_case(target, k):
    sections, flat, count, dropped = fit(target, k)
    f = validation_grid(sections)
    raw, _, _, _ = independent_check(sections, f, flat)
    scale = 1.
    if np.max(raw) > 40.05 or np.min(raw) < -.01:
        for scale in (.875, .75, .625, .5, .375, .25, .125, 0.):
            trial = [Section(s.f, s.q, s.d*scale, s.kind) for s in sections]
            trial_a = bank_response(trial, f, flat)
            if np.max(trial_a) <= 40.05 and np.min(trial_a) >= -.01:
                sections = trial
                break
    a, pm, zm, agreement = independent_check(sections, f, flat)
    r = target.at(f)
    error = a-r
    # RMS on the fixed linear dense grid, so adding local probes cannot bias it.
    uniform = np.linspace(0., FS/2, 65537)
    uniform_error = bank_response(sections, uniform, flat)-target.at(uniform)
    protected = target.protected(f) if target.protected else r <= .01
    peak = int(np.argmax(a))
    target_peak = int(np.argmax(r))
    left, right = half_crossings(f, a, peak)
    target_left, target_right = half_crossings(f, r, target_peak)
    row = dict(name=target.name, k=k, n=len(sections), samples=count,
               error=float(np.max(abs(error))), rms=float(np.sqrt(np.mean(uniform_error**2))),
               error_hz=float(f[np.argmax(abs(error))]),
               overcut=float(np.max(np.maximum(error[protected], 0.))) if np.any(protected) else 0.,
               max_cut=float(np.max(a)), cap=float(max(0., np.max(a)-40.)),
               ripple=float(max(0., -np.min(a))),
               peak_hz=float(f[peak]), target_peak_hz=float(f[target_peak]),
               width_hz=right-left, target_width_hz=target_right-target_left,
               pole=pm, zero=zm, agreement=agreement, scale=scale, dropped=int(dropped),
               within=[float(np.mean(abs(error) <= t)*100) for t in (.5, 1., 2.)],
               within_counts=[int(np.sum(abs(error) <= t)) for t in (.5, 1., 2.)],
               validation_samples=len(f),
               source_error=float(np.max(abs(target.analytic(f)-r))))
    row["pass"] = row["error"] <= LIMIT and row["cap"] <= .05 and row["ripple"] <= .01
    print(f"{target.name:23} K{k:2d} active={len(sections):2d} max={row['error']:.6f} "
          f"RMS={row['rms']:.6f} protected={row['overcut']:.6f} "
          f"capExcess={max(0.,row['cap']):.6f} ripple={row['ripple']:.6f} "
          f"scale={scale} [{'PASS' if row['pass'] else 'FAIL'}]", flush=True)
    return row


def gaussian(f, center, fwhm, depth):
    return depth*np.exp(-4*np.log(2)*((f-center)/fwhm)**2)


def targets():
    controls = [Section(1000., 4., 8.)]
    four = [Section(f, q, d) for f, q, d in
            ((120., 6., 4.), (700., 8., 6.), (3000., 10., 5.), (12000., 5., 3.))]
    return [Target("matched-single", lambda f: bank_response(controls, f)),
            Target("matched-four", lambda f: bank_response(four, f)),
            Target("Gaussian54-20", lambda f: gaussian(f, 3000., 54., 20.)),
            Target("Gaussian54-40", lambda f: gaussian(f, 3000., 54., 40.)),
            Target("pair80-100", lambda f: gaussian(f, 80., 8., 15.)+gaussian(f, 100., 8., 15.),
                   lambda f: (f >= 87.) & (f <= 93.)),
            Target("disjoint-gaps", lambda f: np.where((abs(f-1000.) <= 100.) | (abs(f-1600.) <= 100.),
                   gaussian(f, 1000., 65., 20.)+gaussian(f, 1600., 65., 20.), 0.),
                   lambda f: (f >= 1150.) & (f <= 1450.))]


def main():
    print("test 1: verified recipe + independent SOS, roots, Schur and dense response")
    # Independent analog transfer in the low-frequency limit. Matched digital
    # response is deliberately not asserted equal to RBJ near Nyquist.
    f = np.geomspace(10., 1000., 1001)
    s = 1j*f/100.
    gain = 10.**(-8./20.)
    analog = (s*s + np.sqrt(gain)/4.*s + 1.) / (s*s + s/(4.*np.sqrt(gain)) + 1.)
    analog_error = float(np.max(abs(response(Section(100., 4., 8.), f)
                                 + 20*np.log10(abs(analog)))))
    assert analog_error < .005, analog_error
    print(f"low-frequency analog limit: max error={analog_error:.9f} dB [PASS]")
    rows = []
    for target in targets():
        for k in (16, 24):
            rows.append(run_case(target, k))
        if target.name == "Gaussian54-20" and not rows[-1]["pass"]:
            print("DECISIVE required-target failure; stopping before more material or rates.")
            break
    controls_pass = all(r["pass"] for r in rows if r["name"].startswith("matched-"))
    failed = [r for r in rows if not r["name"].startswith("matched-") and not r["pass"]]
    print("SUMMARY")
    print(f"Positive controls: {'PASS' if controls_pass else 'FAIL: bounded fitter not validated'}")
    print("VERDICT: " + ("REJECT specified fitter/bank candidate at unchanged 0.25 dB gate"
          if controls_pass and failed else "FITTER FAILURE; no architecture-impossibility claim"
          if not controls_pass else "initial cases pass; static matrix remains required"))
    for row in rows:
        print(repr(row))


if __name__ == "__main__":
    main()
