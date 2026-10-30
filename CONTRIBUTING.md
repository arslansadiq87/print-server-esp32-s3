# Contributing

Read README.md and HELP.md before reporting problems. Include reproducible steps, the first relevant error, exact board/printer identity, and redacted logs/counters. Do not attach credentials or personal firmware.

Keep changes focused. Preserve bounded buffering, HTTP text validation, USB task ownership, and HTTP/RAW serialization. Describe changes to network exposure, credential storage, API fields, or compatibility. Update documentation and the source packager allowlist for new files.

Build firmware changes with `pio run -e esp32-s3-n16r8`. State whether hardware was available. Validate HTTP/RAW labels, relevant backpressure behavior, and disconnect recovery on hardware. Inspect physical output; endpoint completion is insufficient.

For RAW helper changes, check `--help` and emitted bytes with a local receiver before sending to hardware. Run `python scripts/package_source.py` before publication. Never commit the local credential header, binaries, or private keys.

An original-source license has not yet been selected. The owner should settle licensing before inviting broad reuse/contributions.
