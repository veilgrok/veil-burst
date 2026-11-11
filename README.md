# veil-burst

GitHub Actions burst harness for Veil's compute pool (bot account veilgrok).
`gha/dispatcher.py`, `gha/worker.py`, `gha/oracle.py` are unmodified copies from
veilgrok/runner-probe@703b699 `equiv/`. Runners reach the Veil dispatcher through
`gha/relay_client.py` (UDP <-> HTTPS, outbound only). No secrets in this repo.
