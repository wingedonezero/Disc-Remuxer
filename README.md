# disc-remuxer

A CLI tool for extracting elementary streams from optical disc images
(DVD, Blu-ray, UHD). Currently in early development — DVD support is
being scaffolded first.

## Status

Phase 0: workspace scaffolding. No working features yet.

## Building

The build vendors and compiles three C libraries from source
(libdvdread, libdvdcss, libdvdnav) via their autotools build systems,
so the host needs the autotools toolchain installed in addition to a
Rust compiler.

### Prerequisites

**Debian / Ubuntu:**
```
sudo apt install build-essential autoconf automake libtool pkg-config
```

**Fedora:**
```
sudo dnf install gcc make autoconf automake libtool pkgconf-pkg-config
```

**macOS (Homebrew):**
```
brew install autoconf automake libtool pkg-config
```

You'll also need a Rust toolchain (1.75 or later). Install via
[rustup](https://rustup.rs/).

### Build

```
git clone --recursive <repo-url> disc-remuxer
cd disc-remuxer
cargo build --release
```

If you cloned without `--recursive`, fetch the submodules first:
```
git submodule update --init
```

The build produces:
```
target/release/
├── disc-remuxer            the CLI binary
├── libdvdread.so.4         DVD IFO / sector reader
├── libdvdcss.so.2          CSS decryption
└── libdvdnav.so.4          DVD navigation / VM
```

The binary has `rpath=$ORIGIN` baked in, so it finds the bundled
`.so` files next to itself at runtime. You can run it from any
directory as long as the libraries are next to the binary; to
distribute, just copy the four files together (`cp target/release/*
dist/`).

## Layout (planned)

```
disc-remuxer/
├── Cargo.toml              workspace root
├── .cargo/config.toml      rustflags for rpath bundling
├── crates/
│   ├── libdvdread-sys/     FFI bindings to libdvdread
│   ├── libdvdcss-sys/      FFI bindings to libdvdcss
│   ├── libdvdnav-sys/      FFI bindings to libdvdnav
│   ├── disc-core/          DiscSource / Demuxer traits + types + errors
│   ├── disc-dvd/           DVD source and demuxer
│   └── disc-cli/           the binary
└── vendor/                 git submodules of the C libraries
    ├── libdvdread/
    ├── libdvdcss/
    └── libdvdnav/
```

Crates and submodules are added in subsequent commits.

## License

The distributed binary statically/dynamically links libdvdnav
(GPL-2.0-or-later), so the combined work is **GPL-2.0-or-later**.
Source files in this repository carry their own SPDX identifiers; the
overall distribution is GPL-2+.

Vendored libraries retain their upstream licenses:

| library     | license            |
|-------------|--------------------|
| libdvdread  | LGPL-2.1-or-later  |
| libdvdcss   | LGPL-2.1-or-later  |
| libdvdnav   | GPL-2.0-or-later   |
