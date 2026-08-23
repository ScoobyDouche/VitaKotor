/* test_audio_codec.c -- host tests for KOTOR's audio containers and the IMA
 * ADPCM decoder. Not built into the VPK.
 *
 * Build and run (needs the extracted OBB for the real assets):
 *   gcc -std=c99 -Wall -Wextra -I tools/hoststubs -I loader \
 *       -o /tmp/test_audio_codec tools/test_audio_codec.c loader/audio_mp3.c \
 *   && /tmp/test_audio_codec com.aspyr.swkotor/main.obb
 *
 * Why this exists. KOTOR ships audio in three containers and two of them are
 * disguised as the third:
 *
 *   58-byte fake RIFF  + real MP3     -- music and VO (data chunk size 0)
 *   470-byte fake MP3  + real RIFF    -- environmental beds, IMA ADPCM stereo
 *   plain RIFF                        -- the in-memory SFX out of sounds.bzf
 *
 * Two bugs came out of that pair, and both are cheap to catch here and
 * expensive to catch on hardware:
 *
 *   1. The 470-byte prefix is itself three valid MPEG-2 frames, so the MP3
 *      sniffer claimed the beds, "decoded" 1728 samples of the junk header and
 *      reported end-of-stream 78 ms in. The engine restarted the loop ten times
 *      a second for the rest of the session (log167: 1256 phantom finishes).
 *
 *   2. Stereo IMA ADPCM stores 4-byte groups that ALTERNATE channels, eight
 *      samples each -- not per-sample interleave. Emitting the nibbles in file
 *      order put eight left samples where L R L R belongs, halving the frame
 *      count, so every bed played an octave high with the stereo image
 *      shredded (log168). It hid for months because the only ADPCM assets in
 *      use were four MONO effects, and the mono path is right.
 *
 * So the decoder is checked against an independent reference implementation
 * written from the format spec, not just against itself.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_mp3.h"

/* audio_mp3.c's quote-includes resolve next to itself, so the real log.h and
 * bigalloc.h win over anything on the include path; supply their symbols. */
void  log_printf(const char *fmt, ...) { (void)fmt; }
void *big_malloc(size_t n) { return malloc(n); }
void  big_free(void *p)    { free(p); }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("    FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

static unsigned char *slurp(const char *path, unsigned *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f);
  *len = (unsigned)n;
  return b;
}

/* ---- reference IMA ADPCM ---------------------------------------------------
 * Written from the format description, deliberately structured differently from
 * the decoder under test: per-channel sample arrays assembled first, interleaved
 * afterwards. If both agree, the interleave is right. */
static const int ref_step[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ref_idx[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

static int ref_next(int nib, int *pred, int *idx) {
  int step = ref_step[*idx], diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  if (nib & 8) diff = -diff;
  int p = *pred + diff;
  if (p >  32767) p =  32767;
  if (p < -32768) p = -32768;
  *pred = p;
  *idx += ref_idx[nib & 15];
  if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
  return p;
}

/* One block -> interleaved frames. Returns frames written. */
static unsigned ref_block(const unsigned char *blk, unsigned blen, unsigned ch,
                          int16_t *dst, unsigned cap) {
  int pred[2], idx[2];
  for (unsigned c = 0; c < ch; c++) {
    pred[c] = (int16_t)(blk[c * 4] | (blk[c * 4 + 1] << 8));
    idx[c]  = blk[c * 4 + 2] > 88 ? 88 : blk[c * 4 + 2];
    if (cap) dst[c] = (int16_t)pred[c];
  }
  unsigned f = 1, pos = 4 * ch;

  if (ch == 1) {
    while (pos < blen && f < cap) {
      dst[f++] = (int16_t)ref_next(blk[pos] & 15, &pred[0], &idx[0]);
      if (f < cap) dst[f++] = (int16_t)ref_next(blk[pos] >> 4, &pred[0], &idx[0]);
      pos++;
    }
    return f;
  }

  /* Build each channel's eight samples separately, then interleave. */
  while (pos + 8 <= blen && f < cap) {
    int16_t s[2][8];
    for (unsigned c = 0; c < 2; c++)
      for (unsigned j = 0; j < 4; j++) {
        unsigned byte = blk[pos + c * 4 + j];
        s[c][j * 2]     = (int16_t)ref_next(byte & 15, &pred[c], &idx[c]);
        s[c][j * 2 + 1] = (int16_t)ref_next(byte >> 4, &pred[c], &idx[c]);
      }
    pos += 8;
    for (unsigned j = 0; j < 8 && f < cap; j++, f++) {
      dst[f * 2]     = s[0][j];
      dst[f * 2 + 1] = s[1][j];
    }
  }
  return f;
}

/* Locate the data chunk the same way the loader must, so the reference reads
 * the same bytes. Returns 0 if this is not a decodable RIFF. */
static int ref_locate(const unsigned char *d, unsigned len, unsigned *data_off,
                      unsigned *data_len, unsigned *ch, unsigned *balign) {
  unsigned base;
  if (len >= 44 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) base = 0;
  else if (len >= 514 && !memcmp(d + 470, "RIFF", 4) &&
           !memcmp(d + 478, "WAVE", 4)) base = 470;
  else return 0;

  unsigned pos = base + 12, have = 0;
  while (pos + 8 <= len) {
    unsigned csz = d[pos+4] | (d[pos+5]<<8) | (d[pos+6]<<16) | ((unsigned)d[pos+7]<<24);
    if (csz > len - (pos + 8)) csz = len - (pos + 8);
    if (!memcmp(d + pos, "fmt ", 4) && csz >= 16) {
      *ch     = d[pos + 10] | (d[pos + 11] << 8);
      *balign = d[pos + 20] | (d[pos + 21] << 8);
      have = 1;
    } else if (!memcmp(d + pos, "data", 4)) {
      if (!have || csz == 0) return 0;
      *data_off = pos + 8;
      *data_len = csz;
      return 1;
    }
    pos += 8 + csz + (csz & 1);
  }
  return 0;
}

/* ---- cases ---------------------------------------------------------------- */

static void bed(const char *path, unsigned rate, unsigned ch, unsigned frames) {
  unsigned len = 0;
  unsigned char *d = slurp(path, &len);
  const char *base = strrchr(path, '/');
  printf("  %s\n", base ? base + 1 : path);
  if (!d) { printf("    SKIP (cannot read)\n"); return; }

  AudioPcm pr;
  CHECK(audio_mp3_probe(d, len, &pr), "probe rejected the asset");
  CHECK(pr.pcm == NULL, "probe allocated %p -- it must stay header-only",
        (void *)pr.pcm);
  CHECK(pr.rate == rate, "probe rate %u != %u", pr.rate, rate);
  CHECK(pr.channels == ch, "probe channels %u != %u", pr.channels, ch);
  CHECK(pr.nsamples == frames, "probe frames %u != %u", pr.nsamples, frames);
  CHECK(!audio_mp3_stream_needs_hw(d, len),
        "claimed a hardware decoder -- ADPCM must decode in software");

  AudioPcm whole;
  CHECK(audio_mp3_decode(d, len, &whole), "whole-asset decode failed");
  if (!whole.pcm) { free(d); return; }
  CHECK(whole.nsamples == frames, "whole decode gave %u frames, expected %u",
        whole.nsamples, frames);

  /* 1. against the independent reference, over the first blocks */
  unsigned doff, dlen, rch, balign;
  if (ref_locate(d, len, &doff, &dlen, &rch, &balign)) {
    unsigned per = 1 + ((balign - 4 * rch) * 2) / rch, bad = 0, done = 0;
    int16_t *ref = malloc((size_t)per * rch * sizeof(int16_t));
    for (unsigned b = 0; b < 8 && (b + 1) * balign <= dlen; b++) {
      unsigned n = ref_block(d + doff + b * balign, balign, rch, ref, per);
      if (done + n > whole.nsamples) break;
      if (memcmp(ref, whole.pcm + (size_t)done * rch,
                 (size_t)n * rch * sizeof(int16_t))) bad++;
      done += n;
    }
    CHECK(bad == 0, "%u of the first 8 blocks differ from the reference decoder "
                    "-- check the stereo group interleave", bad);
    if (!bad) printf("    %u frames match an independent reference decode\n", done);
    free(ref);
  } else {
    CHECK(0, "reference could not locate the data chunk");
  }

  /* 2. streaming must reproduce the whole-asset decode exactly, at any chunk
   *    size -- including sizes that straddle block boundaries */
  AudioPcm fmt;
  AudioMp3Stream *s = audio_mp3_stream_open(d, len, &fmt);
  CHECK(s != NULL, "stream_open failed");
  if (!s) { free(whole.pcm); free(d); return; }
  CHECK(fmt.nsamples == frames, "stream reports %u frames, whole decode %u",
        fmt.nsamples, frames);

  static const unsigned chunks[] = { 1, 7, 1023, 2041, 2042, 4096, 333 };
  int16_t *buf = malloc(8192 * (size_t)ch * sizeof(int16_t));
  unsigned total = 0, ci = 0, mismatch = 0;
  for (;;) {
    unsigned want = chunks[ci++ % (sizeof chunks / sizeof *chunks)];
    unsigned got  = audio_mp3_stream_read(s, buf, want);
    CHECK(got <= want, "read returned %u frames for a %u request", got, want);
    if (got) {
      if (total + got <= whole.nsamples &&
          memcmp(buf, whole.pcm + (size_t)total * ch,
                 (size_t)got * ch * sizeof(int16_t))) mismatch++;
      total += got;
    }
    if (!got && audio_mp3_stream_eos(s)) break;
    if (!got) { CHECK(0, "read returned 0 with no eos at frame %u", total); break; }
  }
  CHECK(mismatch == 0, "%u streamed chunks differ from the whole-asset decode",
        mismatch);
  CHECK(total == whole.nsamples, "streamed %u frames, whole decode had %u",
        total, whole.nsamples);
  printf("    streamed %u frames in mixed chunk sizes, identical to whole decode\n",
         total);

  /* 3. looping ambience rewinds constantly, so this must land on frame 0 */
  audio_mp3_stream_rewind(s);
  CHECK(!audio_mp3_stream_eos(s), "still at eos after rewind");
  unsigned got = audio_mp3_stream_read(s, buf, 2041);
  CHECK(got == 2041, "post-rewind read gave %u frames", got);
  CHECK(!memcmp(buf, whole.pcm, (size_t)got * ch * sizeof(int16_t)),
        "post-rewind samples differ from the start of the asset");

  audio_mp3_stream_close(s);
  free(buf); free(whole.pcm); free(d);
}

/* The other two containers must keep behaving exactly as they did. */
static void container(const char *path, int want_hw, const char *what) {
  unsigned len = 0;
  unsigned char *d = slurp(path, &len);
  const char *base = strrchr(path, '/');
  printf("  %s (%s)\n", base ? base + 1 : path, what);
  if (!d) { printf("    SKIP (cannot read)\n"); return; }

  int hw = audio_mp3_stream_needs_hw(d, len);
  CHECK(hw == want_hw, "needs_hw=%d, expected %d", hw, want_hw);

  if (!want_hw) {
    /* The companion opens these at 470 itself, so both views must agree. */
    AudioPcm a, b;
    if (audio_mp3_decode(d, len, &a)) {
      CHECK(audio_mp3_decode(d + 470, len - 470, &b), "offset-470 view failed");
      if (b.pcm) {
        CHECK(a.nsamples == b.nsamples && a.rate == b.rate &&
              !memcmp(a.pcm, b.pcm, (size_t)a.nsamples * a.channels * 2),
              "offset-0 and offset-470 views disagree");
        free(b.pcm);
      }
      printf("    %u Hz %u ch %u ms, offset-0 and offset-470 views identical\n",
             a.rate, a.channels, a.ms);
      free(a.pcm);
    } else {
      CHECK(0, "decode failed");
    }
  }
  free(d);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : "com.aspyr.swkotor/main.obb";
  char p[512];

  printf("470-byte junk-MP3 prefix + IMA ADPCM stereo (the environmental beds)\n");
  struct { const char *n; unsigned rate, ch, frames; } beds[] = {
    { "al_en_sthbasebed.wav", 44100, 2, 2024672 },   /* 45910 ms */
    { "al_en_undergrnd.wav",  44100, 2, 1583816 },   /* 35914 ms */
    { "al_en_cityext.wav",    44100, 2, 6235255 },   /* 141389 ms, Lower City */
  };
  for (unsigned i = 0; i < sizeof beds / sizeof *beds; i++) {
    snprintf(p, sizeof p, "%s/streammusic/%s", root, beds[i].n);
    bed(p, beds[i].rate, beds[i].ch, beds[i].frames);
  }

  printf("the other two containers must be unaffected\n");
  snprintf(p, sizeof p, "%s/streammusic/mus_area_gang.wav", root);
  container(p, 1, "58-byte fake RIFF wrapping real MP3");
  snprintf(p, sizeof p, "%s/streamsounds/al_vx_forcfield2.wav", root);
  container(p, 0, "470-byte prefix + real PCM");

  printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", fails);
  return fails ? 1 : 0;
}
