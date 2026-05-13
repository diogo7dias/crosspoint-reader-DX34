# firmware branch

This branch holds the latest released `firmware.bin` so it can be served via jsDelivr's CORS-friendly CDN:

```
https://cdn.jsdelivr.net/gh/diogo7dias/crosspoint-reader-DX34@firmware/firmware.bin
```

The device's `/update` web page fetches this URL from the user's browser, then uploads to the device for installation.

Every release should update this branch with the new `firmware.bin`.
