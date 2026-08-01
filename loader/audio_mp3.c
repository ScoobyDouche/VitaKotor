/* audio_mp3.c -- KOTOR audio decode: MP3 -> PCM via the Vita's hardware decoder
 *
 * KOTOR's `.wav` files are MP3 in disguise. Verified offline against the OBB
 * (300-file sample, main.53.com.aspyr.swkotor.obb):
 *
 *   - 91% carry a 58-byte fake RIFF header (declares PCM/8-bit/22050 and a
 *     `data` chunk of size 0 -- all lies) with the real MP3 starting at byte 58.
 *   - 9% are raw MP3 from byte 0, no header at all.
 *   - Every file sampled: 48 kbps, MONO. Rates are 32000 Hz (with the header)
 *     or 22050 Hz (raw). Both MPEG Layer III.
 *
 * So we sniff rather than blindly skipping 58 bytes, and we never trust the RIFF
 * fields -- the MPEG frame header is the only authority for rate and channels.
 *
 * Decoding runs on sceAudiodec (hardware). We do NOT hand-compute frame lengths:
 * sceAudiodecDecode reports how much elementary stream it actually consumed in
 * ctrl.inputEsSize, so we just advance by that. It is both simpler and correct
 * across MPEG1/2/2.5, which differ in frame-size formula and samples-per-frame.
 */

#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>

#include "audio_mp3.h"
#include "log.h"

#define MP3_MAX_SAMPLES 1152                 /* MPEG1 Layer III granule pair */
#define MP3_MAX_PCM     (MP3_MAX_SAMPLES * 2 * 2)   /* stereo, 16-bit */

static int g_lib_ready = 0;

int audio_mp3_init_library(void) {
  if (g_lib_ready) return 1;
  SceAudiodecInitParam p;
  memset(&p, 0, sizeof p);
  p.mp3.size = sizeof(SceAudiodecInitStreamParam);
  p.mp3.totalStreams = 1;
  int r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &p);
  // 0x807F0002 = already initialised; treat as success.
  if (r < 0 && (unsigned)r != 0x807F0002u) {
    log_printf("[snd] sceAudiodecInitLibrary(MP3) failed 0x%08X", (unsigned)r);
    return 0;
  }
  g_lib_ready = 1;
  log_printf("[snd] MP3 decoder library ready");
  return 1;
}

/* Locate the first MPEG audio sync word, skipping a fake RIFF header and/or an
 * ID3v2 tag. Returns the byte offset, or -1 if this is not MPEG audio at all. */
static int find_sync(const unsigned char *d, unsigned len) {
  unsigned start = 0;
  if (len > 10 && !memcmp(d, "ID3", 3)) {
    /* ID3v2 size is 4 syncsafe bytes (7 bits each) after a 10-byte header. */
    start = 10 + (((unsigned)d[6] << 21) | ((unsigned)d[7] << 14) |
                  ((unsigned)d[8] << 7)  |  (unsigned)d[9]);
  } else if (len > 12 && !memcmp(d, "RIFF", 4)) {
    start = 58;                              /* KOTOR's fixed fake header */
  }
  if (start >= len) start = 0;
  /* Scan forward for a plausible frame header: sync + Layer III + valid rate. */
  for (unsigned i = start; i + 4 <= len && i < start + 8192; i++) {
    if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
    unsigned ver = (d[i + 1] >> 3) & 3, layer = (d[i + 1] >> 1) & 3;
    unsigned bri = (d[i + 2] >> 4) & 0xF, sri = (d[i + 2] >> 2) & 3;
    if (ver == 1 || layer != 1) continue;    /* reserved version / not Layer III */
    if (bri == 0 || bri == 0xF || sri == 3) continue;
    return (int)i;
  }
  return -1;
}

/* MPEG version field -> sample rate table index, and the value sceAudiodec wants
 * in SceAudiodecInfoMp3.version (MPEG1 = 3, MPEG2 = 2, MPEG2.5 = 0). */
static unsigned rate_of(unsigned ver, unsigned sri) {
  static const unsigned r[4][3] = {
    { 11025, 12000,  8000 },   /* 0: MPEG 2.5 */
    {     0,     0,     0 },   /* 1: reserved */
    { 22050, 24000, 16000 },   /* 2: MPEG 2   */
    { 44100, 48000, 32000 },   /* 3: MPEG 1   */
  };
  return (ver < 4 && sri < 3) ? r[ver][sri] : 0;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

/* IMA/DVI ADPCM (WAVE format tag 17). Only 4 of the 1928 sounds in sounds.bzf
 * use it, but they are common action effects, so rejecting the tag left audible
 * gaps. Layout confirmed offline: mono, 44100 Hz, blockAlign 1024, 4 bits,
 * 2041 samples per block -- i.e. a 4-byte per-channel block header (initial
 * predictor int16 + step index) followed by 2 samples per byte. */
static const int ima_step[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_idx[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

static inline int ima_next(int nib, int *pred, int *idx) {
  int step = ima_step[*idx], diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  if (nib & 8) diff = -diff;
  int p = *pred + diff;
  if (p >  32767) p =  32767;
  if (p < -32768) p = -32768;
  *pred = p;
  *idx += ima_idx[nib & 15];
  if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
  return p;
}

static int adpcm_ima_decode(const unsigned char *d, unsigned len, unsigned ch,
                            unsigned rate, unsigned balign, AudioPcm *out) {
  if ((ch != 1 && ch != 2) || !rate) return 0;
  if (balign < 4 * ch) balign = 1024;

  unsigned nblocks = len / balign;
  unsigned tail    = len % balign;
  if (!nblocks && tail < 4 * ch) return 0;

  /* samples per full block: one seeded by the header, then 2 per byte */
  unsigned per_block = 1 + ((balign - 4 * ch) * 2) / ch;
  unsigned frames    = nblocks * per_block;
  if (tail > 4 * ch) frames += 1 + ((tail - 4 * ch) * 2) / ch;
  if (!frames) return 0;

  int16_t *pcm = (int16_t *)malloc((size_t)frames * ch * sizeof(int16_t));
  if (!pcm) { log_printf("[snd] ADPCM out of memory (%u frames)", frames); return 0; }

  int pred[2] = {0, 0}, idx[2] = {0, 0};
  unsigned w = 0;
  for (unsigned b = 0; b * balign < len && w < frames * ch; b++) {
    const unsigned char *blk = d + b * balign;
    unsigned blen = len - b * balign;
    if (blen > balign) blen = balign;
    if (blen < 4 * ch) break;

    for (unsigned c = 0; c < ch; c++) {
      pred[c] = (int16_t)rd16(blk + c * 4);
      idx[c]  = blk[c * 4 + 2];
      if (idx[c] > 88) idx[c] = 88;
    }
    for (unsigned c = 0; c < ch && w < frames * ch; c++) pcm[w++] = (int16_t)pred[c];

    /* mono: nibbles run low-then-high. stereo: 4-byte groups alternate channels. */
    for (unsigned i = 4 * ch; i < blen && w < frames * ch; i++) {
      unsigned c = (ch == 1) ? 0 : ((i - 4 * ch) / 4) & 1;
      pcm[w++] = (int16_t)ima_next(blk[i] & 15, &pred[c], &idx[c]);
      if (w < frames * ch)
        pcm[w++] = (int16_t)ima_next(blk[i] >> 4, &pred[c], &idx[c]);
    }
  }

  out->pcm      = pcm;
  out->nsamples = w / ch;
  out->channels = ch;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)out->nsamples * 1000u / rate);
  return out->nsamples ? 1 : 0;
}

/* Real RIFF/WAVE PCM -- a SECOND, different asset format.
 *
 * Verified offline 2026-07-31 by LZMA-decompressing data/sounds.bzf out of the
 * OBB: the in-memory SFX resources (gui_click, gui_scroll, ...) are genuine
 * 16-bit MONO PCM at 22050 Hz. They are NOT MP3, and feeding them to the MP3
 * decoder produced "no sync" / "decoded 0 samples" for every sound effect.
 *
 * Only streamwaves/streammusic use the MP3-in-a-fake-RIFF form above. The two
 * are told apart by the `data` chunk: the fake header declares size 0, a real
 * one declares the true byte count. So: real data chunk -> PCM, else fall
 * through to the MP3 sniffer.
 */
static int wav_try_pcm(const unsigned char *d, unsigned len, AudioPcm *out) {
  if (len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;

  unsigned pos = 12, afmt = 0, ch = 0, rate = 0, bits = 0, balign = 0, have_fmt = 0;
  while (pos + 8 <= len) {
    unsigned csz = rd32(d + pos + 4);
    const unsigned char *body = d + pos + 8;
    unsigned avail = len - (pos + 8);
    if (csz > avail) csz = avail;                  /* tolerate a short tail */

    if (!memcmp(d + pos, "fmt ", 4) && csz >= 16) {
      afmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4);
      balign = rd16(body + 12); bits = rd16(body + 14);
      have_fmt = 1;
    } else if (!memcmp(d + pos, "data", 4)) {
      /* size 0 == the fake header the MP3 assets carry; let MP3 handle it */
      if (!have_fmt || csz == 0) return 0;
      if (afmt == 17) return adpcm_ima_decode(body, csz, ch, rate, balign, out);
      if (afmt != 1) return 0;
      if (ch != 1 && ch != 2) return 0;
      if (bits != 8 && bits != 16) return 0;
      if (!rate) return 0;

      unsigned frames = csz / (ch * (bits / 8));
      if (!frames) return 0;
      int16_t *pcm = (int16_t *)malloc((size_t)frames * ch * sizeof(int16_t));
      if (!pcm) return 0;

      if (bits == 16) {
        memcpy(pcm, body, (size_t)frames * ch * sizeof(int16_t));
      } else {
        /* 8-bit WAV is unsigned, centred on 128 */
        for (unsigned i = 0; i < frames * ch; i++)
          pcm[i] = (int16_t)(((int)body[i] - 128) << 8);
      }

      out->pcm      = pcm;
      out->nsamples = frames;
      out->channels = ch;
      out->rate     = rate;
      out->ms       = (unsigned)((uint64_t)frames * 1000u / rate);
      /* Throttled: this fired 2569 times in log110 and the log I/O alone hurt. */
      static unsigned nlog = 0;
      if (nlog++ < 24)
        log_printf("[snd] RIFF PCM %u Hz %u ch %u-bit -> %u frames (%u ms)",
                   rate, ch, bits, frames, out->ms);
      return 1;
    }
    pos += 8 + csz + (csz & 1);                    /* chunks are word-aligned */
  }
  return 0;
}

/* kbps by bitrate index, Layer III. Index 0 (free) and 15 (bad) are rejected by
 * find_sync, so 0 entries there are never selected. */
static const unsigned br_mpeg1[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const unsigned br_mpeg2[16] = { 0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0 };

int audio_mp3_probe(const void *data, unsigned len, AudioPcm *out) {
  memset(out, 0, sizeof *out);
  if (!data || len < 16) return 0;

  const unsigned char *d = (const unsigned char *)data;

  /* Real PCM: the exact answer is in the header, so no estimate needed. */
  AudioPcm tmp;
  if (wav_try_pcm(d, len, &tmp)) {
    free(tmp.pcm);
    tmp.pcm = NULL;
    *out = tmp;
    return 1;
  }

  int off = find_sync(d, len);
  if (off < 0) return 0;

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned bri  = (d[off + 2] >> 4) & 0xF;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned rate = rate_of(ver, sri);
  unsigned kbps = (ver == 3) ? br_mpeg1[bri] : br_mpeg2[bri];
  if (!rate || !kbps) return 0;

  unsigned es = len - (unsigned)off;
  out->pcm      = NULL;                       /* silence: no allocation at all */
  out->channels = (cmod == 3) ? 1 : 2;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)es * 8u / kbps);   /* bytes*8/kbps == ms */
  out->nsamples = (unsigned)((uint64_t)out->ms * rate / 1000u);
  return out->nsamples ? 1 : 0;
}

int audio_mp3_decode(const void *data, unsigned len, AudioPcm *out) {
  memset(out, 0, sizeof *out);
  if (!data || len < 16) return 0;

  const unsigned char *d = (const unsigned char *)data;

  /* Uncompressed PCM needs no hardware decoder, so try it before init. */
  if (wav_try_pcm(d, len, out)) return 1;

  if (!audio_mp3_init_library()) return 0;
  int off = find_sync(d, len);
  if (off < 0) {
    log_printf("[snd] not MPEG audio (no sync in first 8KB)");
    return 0;
  }

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned ch   = (cmod == 3) ? 1 : 2;       /* 3 = single channel */
  unsigned rate = rate_of(ver, sri);
  if (!rate) return 0;

  SceAudiodecCtrl ctrl;
  SceAudiodecInfo info;
  memset(&ctrl, 0, sizeof ctrl);
  memset(&info, 0, sizeof info);
  info.mp3.size    = sizeof(SceAudiodecInfoMp3);
  info.mp3.ch      = ch;
  info.mp3.version = ver;
  ctrl.size        = sizeof ctrl;
  ctrl.pInfo       = &info;
  ctrl.maxEsSize   = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
  ctrl.maxPcmSize  = MP3_MAX_PCM;
  ctrl.wordLength  = SCE_AUDIODEC_WORD_LENGTH_16BITS;

  int r = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3);
  if (r < 0) {
    log_printf("[snd] CreateDecoder failed 0x%08X (ch=%u ver=%u rate=%u)",
               (unsigned)r, ch, ver, rate);
    return 0;
  }

  /* Upper bound on output: every frame yields at most MP3_MAX_SAMPLES frames of
   * `ch` samples. Estimating from the stream length and bitrate would be tighter
   * but this is a decode-once buffer and the slack is small. */
  unsigned est_frames = (len - (unsigned)off) / 96 + 8;   /* 96B = min L3 frame */
  unsigned cap = est_frames * MP3_MAX_SAMPLES * ch;
  int16_t *pcm = (int16_t *)malloc((size_t)cap * sizeof(int16_t));
  int16_t  frame[MP3_MAX_SAMPLES * 2];
  if (!pcm) {
    /* Was silent, and that hurt: two 15 MB music decodes filled the heap, every
     * later stream failed here with no log, and the game retried forever. */
    log_printf("[snd] OUT OF MEMORY: %u KB for %u ch @ %u Hz (es %u KB)",
               (unsigned)(cap * sizeof(int16_t) / 1024), ch, rate, len / 1024);
    sceAudiodecDeleteDecoder(&ctrl);
    return 0;
  }

  unsigned total = 0, pos = (unsigned)off, frames = 0, errs = 0;
  while (pos + 4 <= len && total + MP3_MAX_SAMPLES * ch <= cap) {
    ctrl.pEs         = (SceUInt8 *)(d + pos);
    ctrl.inputEsSize = 0;
    ctrl.pPcm        = frame;
    ctrl.outputPcmSize = 0;
    unsigned avail = len - pos;
    ctrl.maxEsSize = avail < SCE_AUDIODEC_MP3_MAX_ES_SIZE
                       ? avail : SCE_AUDIODEC_MP3_MAX_ES_SIZE;

    r = sceAudiodecDecode(&ctrl);
    if (r < 0 || ctrl.inputEsSize == 0) {
      /* Resync: step one byte and look for the next frame. A handful of bad
       * frames at a stream edge is normal; a wall of them means real trouble. */
      if (++errs > 64) break;
      pos++;
      continue;
    }
    unsigned got = ctrl.outputPcmSize / sizeof(int16_t);
    if (got) {
      memcpy(pcm + total, frame, ctrl.outputPcmSize);
      total += got;
    }
    pos += ctrl.inputEsSize;
    frames++;
  }

  sceAudiodecDeleteDecoder(&ctrl);

  if (!total) {
    free(pcm);
    log_printf("[snd] decoded 0 samples (frames=%u errs=%u)", frames, errs);
    return 0;
  }

  out->pcm      = pcm;
  out->nsamples = total / ch;
  out->channels = ch;
  out->rate     = rate;
  out->ms       = (unsigned)((uint64_t)out->nsamples * 1000u / rate);
  return 1;
}

void audio_pcm_free(AudioPcm *p) {
  if (p && p->pcm) { free(p->pcm); p->pcm = NULL; }
}
