"""Independent prime-count oracle for the lab's 40-interval workload and the mid-run set."""

from __future__ import annotations


def sieve_mask(limit: int) -> bytearray:
    mask = bytearray(b"\x01") * limit
    if limit:
        mask[0] = 0
    if limit > 1:
        mask[1] = 0
    p = 2
    while p * p < limit:
        if mask[p]:
            start = p * p
            mask[start:limit:p] = b"\x00" * len(mask[start:limit:p])
        p += 1
    return mask


def count_primes(lo: int, hi: int) -> int:
    if hi <= lo or hi <= 2:
        return 0 if hi <= 2 and not (lo < 2 <= hi) else int(lo < 2 <= hi)
    limit = int(hi**0.5) + 1
    base = sieve_mask(limit)
    n = hi - lo
    seg = bytearray(b"\x01") * n
    for p in range(2, limit):
        if not base[p]:
            continue
        start = max(p * p, ((lo + p - 1) // p) * p) - lo
        if start < n:
            seg[start:n:p] = b"\x00" * len(seg[start:n:p])
    if lo == 0:
        if n > 0:
            seg[0] = 0
        if n > 1:
            seg[1] = 0
    elif lo == 1 and n > 0:
        seg[0] = 0
    return int(sum(seg))


# Same 40 intervals used in the lab: 250,000 integers each, covering 0..10,000,000.
COMPARE_INTERVALS = [(i * 250_000, (i + 1) * 250_000) for i in range(40)]

# Mid-run set from RELIABILITY.md: 80 tasks, 20 x 1,000,000-wide windows
# covering 50,000,000..70,000,000, each window repeated four times.
DROP_LO = 50_000_000
DROP_INTERVALS = [
    (DROP_LO + (i % 20) * 1_000_000, DROP_LO + (i % 20 + 1) * 1_000_000)
    for i in range(80)
]


def expected_counts(intervals: list[tuple[int, int]]) -> list[int]:
    unique: dict[tuple[int, int], int] = {}
    out: list[int] = []
    for lo, hi in intervals:
        if (lo, hi) not in unique:
            unique[(lo, hi)] = count_primes(lo, hi)
        out.append(unique[(lo, hi)])
    return out


def sanity_check() -> None:
    # pi(10,000,000) = 664,579; pi(250,000) = 22,044. Matches the lab's large-expected.txt.
    got = expected_counts(COMPARE_INTERVALS)
    if got[0] != 22_044 or sum(got) != 664_579:
        raise SystemExit(f"oracle sanity failed: first={got[0]} sum={sum(got)}")


if __name__ == "__main__":
    sanity_check()
    print("oracle_ok", 22044, 664579)
