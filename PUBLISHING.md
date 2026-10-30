# Publish the source on GitHub

The documentation, blank credential template, ignore rules, and source archive builder are prepared. No GitHub upload is performed by these instructions.

## Validate and package

```sh
pio run -e esp32-s3-n16r8
python scripts/raw9100_test.py --help
python scripts/package_source.py
```

Copy the credential template to the ignored local header before building. Empty values allow a safe compilation; deploy with your own random API key for HTTP printing. Validate printing separately on hardware using HELP.md.

The packager writes `publish/tsc-esp32-source.zip` with an explicit allowlist. It excludes credentials, compiled firmware, generated SDK configuration, editor settings, dependency sources, and logs. Inspect the archive before upload. Update the allowlist when adding source files.

## License

Choose and add a LICENSE if you intend to grant reuse rights. No license was chosen on the owner's behalf. Review ownership and dependency licenses before advertising the project as open source.

## New Git repository

Create an empty GitHub repository in your account. From this source folder:

```sh
git init
git add .
git status --short
git diff --cached --stat
git diff --cached --check
git diff --cached
```

Confirm staging excludes `main/wifi_credentials.h`, `.pio`, `.vscode`, `managed_components`, generated `sdkconfig.esp32-s3-n16r8`, binaries, logs, and secrets. Confirm **sdkconfig.defaults is included**. Then substitute the real repository URL:

```sh
git commit -m "Prepare ESP32-S3 TSC print server source and documentation"
git branch -M main
git remote add origin YOUR_GITHUB_REPOSITORY_URL
git push -u origin main
```

Authenticate using your credential manager or SSH. Do not embed tokens in remote URLs.

## Existing repository

Do not overwrite a remote or force-push using the new-repository commands. Bring these changes into a clone and inspect tracked files/history first. Ignoring previously committed credentials does not remove them from tracking or history. Rotate any exposed secrets and review historical copies before publishing.

## Final review

- Follow README setup from an extracted source archive and build the pinned environment.
- Report compilation and hardware tests separately.
- Verify the archive excludes the private header and all compiled firmware.
- Review screenshots/logs for AP passwords and network identifiers.
- Add your chosen license if granting reuse rights.
- Review existing remote history separately; it is outside a source archive scan.
