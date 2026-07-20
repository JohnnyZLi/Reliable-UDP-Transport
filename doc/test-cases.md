# Test cases

1. **Small text transfer, no loss** - verifies basic START/DATA/FIN sequencing and byte-for-byte output.
2. **Empty file** - verifies that START and FIN complete without DATA packets.
3. **Large binary transfer** - verifies binary safety, multiple windows, and bounded memory behavior.
4. **Lossy transfer** - runs deterministic 15% inbound/outbound loss and verifies retransmission and final SHA-256 equality.
5. **Nested output path** - verifies creation of parent directories communicated by the client.
6. **Invalid MSS** - verifies `Required minimum MSS is 33` and exit status 1.
7. **Missing server** - verifies `Cannot detect server` and exit status 3 after the configured maximum interval.
