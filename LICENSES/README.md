# Redistributed license texts

Release packaging copies the copyright and license files belonging to the
actual resolved vcpkg and NuGet packages into this directory in the package.
`THIRD_PARTY_NOTICES.md` is the human-readable inventory.

Do not treat this placeholder as the complete notices bundle for a binary
release. `scripts/build-packages.ps1` fails when it cannot collect the required
license material.
