# FTDI D3XX Linux x86-64 SDK 1.1.8

FTDI lists Linux D3XX 1.1.8, released 2026-07-21, at:
https://ftdichip.com/drivers/d3xx-drivers/

The exact official archive URL resolves to:
https://ftdichip.com/wp-content/uploads/2026/07/libftd3xx-linux-x86_64-1.1.8.tgz

FTDI returned HTTP 403 to the automated download client. The identically named
archive was therefore obtained from the TousLesDrivers indexed mirror of that
official URL. Archive SHA-256:

`0e3c13c24c7b4009e7b07e7808b0f729a9008f8da3df3fad759d460d350b50fa`

Only the files required to compile/run this project and preserve the vendor
README/license context are vendored here. They are unmodified:

- `ftd3xx.h`: `06ba4fc2d11680f974c1cfa76d504c61ef7325a089133e5247271d25d7111cb4`
- `Types.h`: `1f1669cea660bddffc9e42197201b2f17dec6c8a81fb2e12b268748d4ce9d66b`
- `libftd3xx.so.1.1.8`: `949445eb019a6fe3e314e54cc45af1b8ddb1fcdf0c377f1144a59143d0a08752`
- `README.pdf`: `ff7f2f2bf948239ce18393748aa58b19411f9f6ac8cde9cd220ef3cb57195b36`

The license notice in `ftd3xx.h` says FTDI drivers may be used only with
FTDI-based products and may be distributed in any form if the license
information is not modified. The FT601 on Ft+ V2 is an FTDI part. Do not alter
the vendor license notice.
