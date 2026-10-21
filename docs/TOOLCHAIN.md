# Toolchain setup

Two builds matter: the **host** build (native, for unit tests and fast
iteration) and the **DOS** build (DJGPP + Watt-32). The host build needs
nothing but a C compiler and works today.

```sh
make test     # host unit tests
make host     # native build of the handshake probe
make dos      # DOSYPP.EXE  (needs the steps below)
```

---

## Host build

Any C99 compiler. On macOS, Xcode command line tools are enough. No other
dependencies — deliberately: everything in `src/adc/`, `src/hash/` and
`src/share/index.c` must stay free of DOS and third-party dependencies so it
can be tested natively.

---

## DOS build: the container (recommended)

```sh
docker build -f tools/docker/Dockerfile.djgpp -t dosypp-djgpp .   # ~20 min, once
make dos-docker                                                   # -> build/dos/DOSYPP.EXE
make dosfiles                                                     # CWSDPMI into dosbox-x/c/
```

On an arm64 host this builds a **native arm64 cross-compiler**, so DOS builds
are not emulated. Verified output:

```
build/dos/DOSYPP.EXE: MS-DOS executable, COFF for MS-DOS, DJGPP go32 DOS extender
```

### Why a container and not a host install

A host build of GCC 12.2 on current macOS **does not work**, and it is not
worth the fight. GCC bundles an old zlib whose `zutil.h` does
`#define fdopen(fd,mode) NULL`, which collides with the macOS SDK's `fdopen`
prototype under clang 21:

```
_stdio.h:322:7: error: expected ')'
  322 | FILE *fdopen(int, const char *) __DARWIN_ALIAS_STARTING(...)
./zutil.h:140:33: note: expanded from macro 'fdopen'
```

`--with-system-zlib` is the right fix in principle, but the bundled copy gets
built anyway from another configure in the tree. Debian's GCC builds it
cleanly, so that is where it is built.

### Why Watt-32 comes prebuilt

The image takes DJGPP's own Watt-32 package (`wat3211b.zip`) rather than
cross-building from git. That is not laziness — **the cross-build cannot be
completed**, and `configur.sh` says so in its own header comment:

> What does not work: generation of `inc/sys/<target>.err`, e.g. `djgpp.err`
> ... `util/errnos.c` relies on being compiled as a target-exe

The build dies on a missing `sys/djgpp.err`. Generating it needs
`util/dj_err.exe`, a **DOS** binary, so a from-source cross-build would need a
DOS emulator inside the toolchain build. The prebuilt package ships both
`libwatt.a` and that generated `djgpp.err`. Watt-32 is C with a stable ABI, so
a library built by an older DJGPP links fine against GCC 12.2 objects.

If you do want the source build some day, these are the landmines, in order:

1. `src/build/djgpp` must exist; `configur.sh` writes into it but never
   creates it.
2. `util/linux/*` ship as prebuilt **x86-64** ELF binaries. On arm64 they fail
   with a confusing `qemu-x86_64: Could not open '/lib64/ld-linux-x86-64.so.2'`.
   `make -C util linux` is not enough — the binaries are *newer than their
   sources*, so make leaves them alone. Delete them first.
3. `mkmake` needs S-Lang (`libslang2-dev`) **and** `pkg-config`; without
   pkg-config, `SLANG_LIBS` silently expands to nothing and you get undefined
   `SLprep_*` references.
4. `nasm` is required (`asmpkt.nas`).
5. Watt-32's `DJGPP_PREFIX` is a compiler **triple**, while build-djgpp's is an
   install **path**. Exporting one breaks the other. This project's Makefile
   therefore calls its own variable `DJGPP_TRIPLE`.
6. Then you still hit `djgpp.err`.

### Host install (not recommended)

If you want to try anyway, [`andrewwutw/build-djgpp`](https://github.com/andrewwutw/build-djgpp)
is the right script; expect to patch GCC's zlib handling. Point the Makefile at
the result:

```sh
make dos DJGPP_TRIPLE=i586-pc-msdosdjgpp WATT32_ROOT=/path/to/watt32
```

`make dos` runs `toolchain-check` first and names whichever piece is missing
rather than failing deep inside a compile.

## Runtime files

`make dosfiles` fetches **CWSDPMI.EXE** (DJGPP's DPMI host — every DJGPP binary
needs it at runtime, and build-djgpp does not install it) into `dosbox-x/c/`.

**Packet drivers** come from `make pktdrv`. Which one you need depends on the
target, and getting it wrong is the most common first-run failure — full
details in [`PKTDRV.md`](PKTDRV.md):

| File | Target |
|---|---|
| `NE2000.COM` | DOSBox-X, real ISA NE2000 cards |
| `PCIPKT.COM` | v86 in the browser (its NIC is a **PCI RTL8029**) |

## Build flags worth knowing

Set in the Makefile, for reasons that will not be obvious later:

| Flag | Why |
|---|---|
| `-march=i386` | Keep the binary runnable on a 386. Raise to `-march=i486` only if we decide 386 support is not worth it. |
| `-msoft-float` | **No floating point anywhere.** Some 486 cores are SX-class or have incomplete x87. A stray `double` becomes a link error here instead of a trap on real hardware. |
| `-std=gnu89` | DJGPP's GCC is old; keeping the DOS build to gnu89 stops C99-isms compiling natively but failing there. |
| `-DDOSYPP_WATT32` | Selects the Watt-32 branches in `src/net/`. |

---

## Why the host build exists

Tiger, TTH, the tokenizer and the share index are where the bugs will be, and
they are all pure computation. Debugging them inside an emulator — no
debugger, slow turnaround, 80×25 of output — is the single easiest way to lose
weeks on this project. Everything testable natively gets tested natively; the
DOS build is for things that genuinely need DOS.

## libbzip2

An ADC file list is `files.xml.bz2`, so the client needs bzip2 in both
directions. `tools/fetchbzip2.sh` downloads bzip2 1.0.8, checks its sha256
and unpacks only the library sources into `third_party/bzip2/`; the build
runs it automatically when they are missing, and `make bzip2` does it on
demand. Nothing is committed, for the same reason Watt-32 is not.

Two things about how it is built:

- **`-DBZ_NO_STDIO`.** This removes libbzip2's own `FILE*` API, which we do
  not use, and with it the only floating point in the library -- the
  `"ratio %5.2f"` lines in its verbose progress output. DJGPP has no
  soft-float runtime, so with `-msoft-float` those printf arguments fail the
  link with `__divdf3` undefined. The low-level stream API is what
  `src/share/bz.c` talks to, and it only transforms buffers.
- **Warnings off (`-w`).** Upstream is clean for its own flags, and patching
  somebody else's warnings is how you end up maintaining a fork.

Memory, which is what decides the settings: compression needs
400KB + 8 x blocksize, so the block size is 1 (100KB) -- about 1.2MB, where
the maximum would be 7.6MB and would buy a few per cent on XML. Decompression
of a stream somebody else compressed may face any block size, so on DOS the
decompressor runs in `small` mode: about 2.5MB instead of up to 7.6MB, at
roughly half the speed. A client that dies because a peer chose `-9` would
be indefensible.

## A Docker quirk worth knowing

Deleting `build/dos` on the host while the DJGPP container has the project
bind-mounted leaves Docker Desktop's file sharing confused about that
directory: the container and the host then disagree about what is in it, in
both directions, and `make dos-docker` can report "built
build/dos/DOSYPP.EXE" with no such file visible on the host.

It is not the build. The fix is to stop deleting the directory from
underneath the mount; if it happens, build into a fresh one:

```sh
./tools/dosmake.sh make dos BUILD=build.dos
```

and copy the binary from there. `make clean` is fine -- the problem is
specifically `rm -rf` of a directory the container is holding open.
