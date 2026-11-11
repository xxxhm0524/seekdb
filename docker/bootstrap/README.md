# Pinned HTTPS bootstrap certificate data

`ca-certificates.deb` is the unmodified Ubuntu package
`ca-certificates_20260816ubuntu0.24.04.2_all.deb`.

- Snapshot source: <https://snapshot.ubuntu.com/ubuntu/20261110T160000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.2_all.deb>
- Identical bytes retrieved from the official package pool:
  <https://archive.ubuntu.com/ubuntu/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.2_all.deb>
- SHA-256: `f9fbacd417b515450b831032cc39d18059e9874e67525679a43c16071dabf9a0`
- Size: 140064 bytes.

The Docker certificate-data stage checks this hash before extracting the
public Mozilla trust anchors. It does not run the package's installation
scripts. Package copyright/license notices are included inside the archive.
Keeping these fixed bytes in the build context removes the extra remote ADD
request that failed during CI DNS resolution. APT still uses the dated Ubuntu
snapshot, its normal signature/TLS validation, and bounded network retries.
