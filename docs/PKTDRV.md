# Packet drivers

DOSy++ reaches the network through a DOS **packet driver**, the same way
period software did. That indirection is why one binary covers real hardware,
DOSBox-X, ao486 and the browser: Watt-32 only ever talks to the driver's
software interrupt, never to a card.

The drivers are not in this repo — they are separately licensed (the Crynwr
ones are GPL) and which one you need depends on the NIC. `make pktdrv` fetches
what it can into `dosbox-x/c/`.

## Which driver for which target

| Target | NIC | Driver | Load as |
|---|---|---|---|
| DOSBox-X | ISA NE2000 | `NE2000.COM` (Crynwr) | `NE2000 0x60 10 0x300` |
| Real 386/486 | ISA NE2000/NE1000 | `NE2000.COM` | match your card's IRQ/IO |
| ao486 / MiSTer | ISA NE2000 (core fork) | `NE2000.COM` | as the core documents |
| **Browser (v86)** | **PCI RTL8029** | **`PCIPKT.COM`** (Realtek) | `PCIPKT 0x60` |

**The browser case is the one that catches people.** v86 emulates an
RTL8029AS — a *PCI* NE2000 derivative — and the ISA `NE2000.COM` probes fixed
I/O ports, so it simply will not find it. You need Realtek's own
`PCIPKT.COM`, which walks the PCI bus itself and therefore takes only a
software interrupt. Observed working: `I/O port is 0xC000`, MAC
`00:22:15:6B:93:8B`.

## Where they come from

**`NE2000.COM` (Crynwr, ISA)** — `crynwr.com` **no longer resolves**; the
domain is gone. `make pktdrv` uses the
[`yeokm1/ne2000plus-collection`](https://github.com/yeokm1/ne2000plus-collection)
mirror, which also carries the assembler source. The binary is UPX-compressed,
8693 bytes expanded. Other mirrors: `packetdriversdos.net`,
`dos.retro.software`.

**`PCIPKT.COM` (Realtek, PCI RTL8029)** — from the Internet Archive item
[`pcind.dos`](https://archive.org/details/pcind.dos), inside
`RTL8029_PKT_v1.10.zip` (`EXE/PCIPKT.COM`, version 1.1, dated 2000).
`make pktdrv` fetches and unpacks it.

## Verifying a driver works

Both rigs have an automated end-to-end check. Each loads the driver, brings up
Watt-32 and runs a real ADC handshake against a hub on the host:

```sh
make dos-net-run      # DOSBox-X, ISA NE2000, slirp
make test-v86         # v86 in Node, PCI RTL8029, Wisp relay
```

A healthy driver prints its own banner, then an interrupt and I/O port, then a
**real MAC address**. If the MAC comes back `FF:FF:FF:FF:FF:FF`, the IRQ or
I/O port is wrong — that is the classic symptom, not a dead card.

## Addressing differs per rig, and it matters

| Rig | Guest address | Hub reachable at |
|---|---|---|
| DOSBox-X + slirp | `10.0.2.15` (DHCP) | `10.0.2.2` = the host |
| v86 + Wisp relay | `192.168.86.100` (**static**) | `10.77.0.1` = relay's mapping |

**v86 uses a static address on purpose.** Its Wisp adapter does answer DHCP in
JavaScript, but Watt-32 does not accept the offer — with the driver loaded and
working you get `Configuring through DHCP..failed`, then RARP and BOOTP fail
too. Since v86's virtual router always hands out the same addresses, static
configuration is both deterministic and faster to boot. `tools/mkv86img.sh`
writes a v86-specific `WATTCP.CFG` for this reason; the DOSBox-X one keeps
`my_ip = dhcp`, which is correct there. Don't "fix" either to match the other.
