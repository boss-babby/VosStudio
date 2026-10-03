# Tectonic — Writer's LaTeX compiler

VOSStudio bundles the official x64 Windows MSVC executable from Tectonic **0.17.0**:

- Release: <https://github.com/tectonic-typesetting/tectonic/releases/tag/tectonic@0.17.0>
- Asset: `tectonic-0.17.0-x86_64-pc-windows-msvc.zip`
- SHA-256: `f61ce51f0b0ade1015b7de7ef368541c5424e9756ecbd0d7af97d6d48030845f`
- License: `LICENSE` (the same notice is embedded in VOSStudio's third-party notices)

`tools/fetch-tectonic.sh` verifies this digest before unpacking the executable into the ignored `build/` directory.
The Windows build compresses `tectonic.exe` into VOSStudio.exe as a resource. On the first Writer PDF preview,
VOSStudio extracts the verified resource to `%LOCALAPPDATA%\VOSStudio\tectonic\<size>-<crc32>\tectonic.exe`.
The user does not need to install Tectonic or add it to `PATH`.

Tectonic's executable is included; its TeX support bundle is separate. Tectonic fetches missing support files from
its configured default bundle and caches them per user. The first compile may require an internet connection;
subsequent compiles generally reuse cached files, although Tectonic can periodically check the remote bundle. The
support bundle is not copied into the application.
