#include "wav_util.h"
#include "../../config.h"

bool writePcmWavHeader(File& f, uint32_t dataBytes) {
  if (!f) return false;
  uint32_t dB = dataBytes, fS = dB + 36, bR = SAMPLE_RATE * 2;
  uint16_t bA = 2, aF = 1, ch = 1, bps = 16;
  uint32_t fL = 16, sr = SAMPLE_RATE;
  f.seek(0);
  f.write((uint8_t*)"RIFF", 4); f.write((uint8_t*)&fS, 4);
  f.write((uint8_t*)"WAVE", 4); f.write((uint8_t*)"fmt ", 4);
  f.write((uint8_t*)&fL, 4);   f.write((uint8_t*)&aF, 2);
  f.write((uint8_t*)&ch, 2);   f.write((uint8_t*)&sr, 4);
  f.write((uint8_t*)&bR, 4);   f.write((uint8_t*)&bA, 2);
  f.write((uint8_t*)&bps, 2);
  f.write((uint8_t*)"data", 4); f.write((uint8_t*)&dB, 4);
  return true;
}

uint32_t wavPcmBytes(const char* path) {
  File f = SD_MMC.open(path);
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  if (sz <= 44) return 0;
  return (uint32_t)(sz - 44);
}

bool extractWavPcmChunk(const char* srcPath, const char* dstPath,
                        uint32_t pcmOffset, uint32_t pcmBytes) {
  if (!srcPath || !dstPath || pcmBytes == 0) return false;
  File src = SD_MMC.open(srcPath);
  if (!src) return false;
  size_t srcSz = src.size();
  if (srcSz <= 44 || 44UL + pcmOffset + pcmBytes > srcSz) {
    src.close();
    return false;
  }

  if (SD_MMC.exists(dstPath)) SD_MMC.remove(dstPath);
  File dst = SD_MMC.open(dstPath, FILE_WRITE);
  if (!dst) { src.close(); return false; }

  writePcmWavHeader(dst, pcmBytes);
  dst.seek(44);
  src.seek(44UL + pcmOffset);

  uint8_t buf[2048];
  uint32_t left = pcmBytes;
  while (left > 0) {
    size_t n = left > sizeof(buf) ? sizeof(buf) : left;
    int got = src.read(buf, n);
    if (got <= 0) { src.close(); dst.close(); SD_MMC.remove(dstPath); return false; }
    dst.write(buf, (size_t)got);
    left -= (uint32_t)got;
  }
  dst.flush();
  src.close();
  dst.close();
  return true;
}

bool truncateWavPcm(const char* path, uint32_t maxPcmBytes) {
  uint32_t have = wavPcmBytes(path);
  if (have == 0) return false;
  if (have <= maxPcmBytes) return true;

  char tmp[80];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if (!extractWavPcmChunk(path, tmp, 0, maxPcmBytes)) return false;
  SD_MMC.remove(path);
  return SD_MMC.rename(tmp, path);
}
