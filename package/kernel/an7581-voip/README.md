# AN7581 VoIP/ISI diagnostic driver

This is the first, deliberately limited bring-up stage for the TP-Link
XB432v telephony hardware. It configures the AN7581 ISI wrapper, resets the
Si32280, and reads direct registers 3 and 0 from both FXS channels.

The reset sequence follows the vendor `spi.ko` HIR `0xe` path: it clears
bits 10 and 11 of Chip-SCU register `0x1d0`, pulses `PCM_SPIWP_RST`, and then
pulses `PCM1_ZSI_ISI_RST`. `PCM1_RST` is intentionally left untouched until
the PCM DMA driver is implemented.

It does **not** load a ProSLIC patch or profile and does not enable the
DC/DC converter, linefeed, or ringing.

Expected identification output for both channels is:

```
ready=1 reg3=0x1f reg0=0x0b revision=3 part=1 (Si32280)
```

Build just this package with:

```
make package/kernel/an7581-voip/compile V=s
```

After booting, inspect the probe output and cached result:

```
dmesg | rg an7581-voip
cat /sys/bus/platform/drivers/an7581-voip/*/identity
```

If an ISI transaction times out or the ID is `0xff`, do not add SLIC
initialization writes. Capture the complete `an7581-voip` log first; the next
things to verify are the PCM1 pin mapping, clock-source mask, reset polarity,
and ISI wrapper timing.
