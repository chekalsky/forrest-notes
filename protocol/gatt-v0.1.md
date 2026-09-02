# Forrest Voice GATT Protocol v0.1

Normative spec for ESP32 peripheral ↔ iPhone central.

## Service

| Name | UUID |
|------|------|
| ForrestVoice | `6E4000F0-B5A3-F393-E0A9-E50E24DCCA9E` |

## Characteristics

| Name | UUID | Properties | Direction |
|------|------|------------|-----------|
| DeviceInfo | `6E4000F1-B5A3-F393-E0A9-E50E24DCCA9E` | Read | ESP → Phone |
| DeviceStatus | `6E4000F2-B5A3-F393-E0A9-E50E24DCCA9E` | Read, Notify | ESP → Phone |
| Control | `6E4000F3-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write Without Response | Phone → ESP |
| RecordingMeta | `6E4000F4-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | ESP → Phone |
| AudioData | `6E4000F5-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | ESP → Phone |
| ResultText | `6E4000F6-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write Without Response | Phone → ESP |
| ProtocolVer | `6E4000F7-B5A3-F393-E0A9-E50E24DCCA9E` | Read | ESP → Phone |

Protocol version read value: `0x0001` (little-endian uint16).

## DeviceStatus (notify payload, 4 bytes)

```
[0] state enum (see below)
[1] battery percent (0–100, 255 = unknown)
[2] pending recording count
[3] pingSeq (increments each status notify; use for keepalive detection)
```

State enum: `0=idle 1=recording 2=finalizing 3=transferring 4=waiting 5=success 6=error`

## RecordingMeta (notify, 16 bytes)

```
[0]    msgType = 0x10
[1]    codec: 0 = PCM WAV
[2-3]  recordingId (uint16 LE)
[4-7]  totalBytes (uint32 LE)
[8-9]  sampleRate (uint16 LE, default 16000)
[10]   channels (default 1)
[11]   bitsPerSample (default 16)
[12-15] reserved
```

## AudioData chunk (notify, variable)

```
[0]    msgType = 0x01
[1]    flags: bit0 = last chunk
[2-3]  recordingId (uint16 LE)
[4-5]  seq (uint16 LE)
[6-7]  payloadLen (uint16 LE)
[8..]  audio bytes (payloadLen)
```

Phone sends Control ACK every 16 chunks (optional for PoC):

```
[0] cmd = 0x01 (ACK_CHUNK)
[1-2] recordingId LE
[3-4] seq LE
```

After the file is written and byte count verified, the last-chunk ACK confirms receipt.
ESP32 deletes from its local queue after receiving the final ACK.

## Control commands

| cmd | Name |
|-----|------|
| 0x01 | ACK_CHUNK |
| 0x02 | XFER_COMPLETE |
| 0x03 | CANCEL_XFER |
| 0x04 | RETRY_PENDING |
