# Protocol design

## Wire format

All integer fields use network byte order. Every datagram is limited to the client-selected MSS.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic (`RUDP`) |
| 4 | 1 | Version |
| 5 | 1 | Packet type: START, DATA, FIN, ACK, ERROR |
| 6 | 2 | Flags |
| 8 | 4 | Session identifier |
| 12 | 4 | Sequence number |
| 16 | 4 | Acknowledgement number |
| 20 | 4 | Payload length |
| 24 | 4 | CRC-32 of header and payload with this field zeroed |
| 28 | 4 | Reserved |
| 32 | variable | Payload |

`START` uses sequence 0 and carries the relative output path. `DATA` begins at sequence 1. `FIN` follows the final DATA sequence. ACK packets identify the acknowledged sequence in the acknowledgement field.

## Client state

The client maintains at most `winsz` unacknowledged slots. Each slot stores only one datagram, its last transmission time, and its retransmission count. Timed-out slots are retransmitted independently, which gives the protocol selective-repeat behavior. The base advances only across a contiguous run of acknowledged packets.

The client reads DATA payloads from disk on demand using sequence-derived offsets, so memory consumption is bounded by the transmission window rather than the file size.

## Server state

Sessions are keyed by client address and random session identifier. Each session tracks:

- final and temporary output paths
- the next sequence that can be written
- a map of out-of-order payloads
- the FIN sequence, when received
- completion and inactivity timestamps

The server writes contiguous DATA packets immediately, buffers later packets, and atomically renames the temporary file after every sequence preceding FIN has been written.

## Loss model

The configured drop percentage is evaluated independently for each received client packet and each outgoing ACK. A deterministic seed can be supplied through `RUDP_DROP_SEED` for repeatable tests.

## Failure behavior

- Exit 1: MSS cannot contain the protocol header plus data
- Exit 3: no server packet was ever detected before the maximum interval
- Exit 4: a packet reached five retransmissions
- Exit 5: a previously responsive server became unreachable or silent

The default retransmission timeout is 750 ms and the default server interval is 30 seconds. Tests may override these with `RUDP_RETRANSMIT_TIMEOUT_MS` and `RUDP_SERVER_TIMEOUT_MS`.
