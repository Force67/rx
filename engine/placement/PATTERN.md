# Ordered-dither placement pattern

The GENERATE stage of GPU-based procedural placement turns a continuous 2D
density field into discrete object positions by **ordered dithering**. Instead of a Bayer
matrix over pixels, we use an explicit list of `N = 256` 2D sample positions in
a unit tile that is repeated across the world. Each position `i` carries an
implicit threshold from its rank in the list:

```
threshold(i) = (i + 0.5) / N
```

A sample point spawns an object iff `density(position) > threshold(i)`. Because
thresholds are the sorted ranks, a density level `d` activates exactly the
**prefix** of the pattern whose thresholds fall below `d`.

## The two rules

1. **Evenly distributed thresholds.** Rank order *is* threshold order:
   thresholds are `(i + 0.5)/N`, spread uniformly across `(0, 1)`.

2. **Maximal successive spacing.** Consecutive positions (in threshold order)
   are placed as far apart as possible, so that *every* prefix — the points
   active at a given density — is an evenly spaced, blue-noise-like set. Low
   density yields few, well-separated points; density `1.0` activates all 256,
   still well spaced.

## Toroidal metric

The tile repeats edge-to-edge, so all distances are **toroidal** on the unit
square (each axis wraps: `d = min(|Δ|, 1 - |Δ|)`). This makes the pattern tile
seamlessly with no clumping or gaps across tile boundaries.

## Algorithm

Greedy farthest-point ordering (`pattern_gen.cc`):

1. Build a candidate pool of `4N = 1024` points via stratified jitter — one
   jittered point per cell of a 32×32 grid.
2. Pick the first point from the PRNG. Then repeatedly select the candidate
   whose **minimum toroidal distance to the already-selected set** is largest,
   and mark it used. An incremental per-candidate "best distance so far" cache
   keeps this at `O(N · pool)`.

Fully deterministic: a self-contained fixed-seed PCG32 (no `std::random_device`).
Rerunning produces byte-identical output.

## Regenerate

```sh
g++ -std=c++20 -O2 pattern_gen.cc -o pattern_gen
./pattern_gen placement_pattern.h      # overwrites the checked-in header
```

then re-run `placement_test` (test/placement_test.cc), which asserts the
pattern's guarantees.

## Achieved quality

Minimum toroidal spacing of each prefix against the ideal grid spacing
`sqrt(1/k)`; the pass bar is `0.5 × sqrt(1/k)`.

| prefix k | min dist | ideal   | ratio |
|---------:|---------:|--------:|------:|
| 2        | 0.69535  | 0.70711 | 0.983 |
| 4        | 0.48118  | 0.50000 | 0.962 |
| 8        | 0.33450  | 0.35355 | 0.946 |
| 16       | 0.22439  | 0.25000 | 0.898 |
| 32       | 0.15534  | 0.17678 | 0.879 |
| 64       | 0.09637  | 0.12500 | 0.771 |
| 128      | 0.06430  | 0.08839 | 0.727 |
| 256      | 0.04389  | 0.06250 | 0.702 |

Every prefix sits at 70–98% of ideal spacing — comfortably above the 50% bar.
Coverage: all 4×4 cells hold 8–24 of the 256 points (target 16), and 2–14 of
the first 128 points (target 8).
