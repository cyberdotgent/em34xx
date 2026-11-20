# ROM images

ROM images are **not** part of this repository. Place them in this directory
under the names below; they are listed in `.gitignore`. The emulator verifies
them by SHA-256 and contains no code or data taken from them.

| Model | File | Size | SHA-256 | Status |
| --- | --- | ---: | --- | --- |
| 3486 | `54g0011.bin` | 524288 | `f5d968694fdb3b6991b219460eb0ce5eec131797a9c0ea928ec61a6a5b9291ef` | **Suspect.** File-address bit 17 is exactly invariant (128 KiB blocks pattern `A B A B C D C D` at 64 KiB granularity); consistent with an undriven/stuck address line during the read. Needs re-dump. |
| 3477 | `95f4171.bin` | 131072 | `3f7fb58aec7505315ff94206c8dc05102e319944bd0bde55092274777fb39fde` | Plausible. Contains the reset vector (top 16 bytes). No invariant address bit. |
| 3477 | `95f4172.bin` | 131072 | `290a58cd5db6543957bdcb2c9e92efefd62a54e37bcd239d41d9bd9cdd6be1d0` | Plausible. No reset vector; no invariant address bit. |

Verify with:

```sh
sha256sum roms/*.bin
```
