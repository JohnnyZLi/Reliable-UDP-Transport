# Validation

Validated on a Linux x86-64 environment with GCC 14.2.0.

- Warning-clean build with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`
- Integration suite passed for text, empty, 50 KB binary, and deterministic 15% lossy transfers
- SHA-256 equality checked for every completed transfer
- AddressSanitizer and UndefinedBehaviorSanitizer integration run passed

Run the same checks with:

```sh
make clean
make
make test
```
