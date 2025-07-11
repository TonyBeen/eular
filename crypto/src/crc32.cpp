#include "crc32.h"
#include "crc32_table.hh"

/* If available, use the ARM processor CRC32 instruction. */
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && W == 8
#  define ARMCRC32
#endif

/* Local functions. */
static uint32_t multmodp OF((uint32_t a, uint32_t b));
static uint32_t x2nmodp OF((int64_t n, uint32_t k));

#if defined(W) && (!defined(ARMCRC32) || defined(DYNAMIC_CRC_TABLE))
    static z_word_t byte_swap OF((z_word_t word));
#endif

#if defined(W) && !defined(ARMCRC32)
    static uint32_t crc_word OF((z_word_t data));
    static z_word_t crc_word_big OF((z_word_t data));
#endif

#if defined(W) && (!defined(ARMCRC32) || defined(DYNAMIC_CRC_TABLE))
/*
  Swap the bytes in a z_word_t to convert between little and big endian. Any
  self-respecting compiler will optimize this to a single machine byte-swap
  instruction, if one is available. This assumes that word_t is either 32 bits
  or 64 bits.
 */
static z_word_t byte_swap(z_word_t word)
{
#  if W == 8
    return
        (word & 0xff00000000000000) >> 56 |
        (word & 0xff000000000000) >> 40 |
        (word & 0xff0000000000) >> 24 |
        (word & 0xff00000000) >> 8 |
        (word & 0xff000000) << 8 |
        (word & 0xff0000) << 24 |
        (word & 0xff00) << 40 |
        (word & 0xff) << 56;
#  else   /* W == 4 */
    return
        (word & 0xff000000) >> 24 |
        (word & 0xff0000) >> 8 |
        (word & 0xff00) << 8 |
        (word & 0xff) << 24;
#  endif
}
#endif

/* CRC polynomial. */
#define POLY 0xedb88320         /* p(x) reflected, with x^32 implied */

/* ========================================================================
 * Routines used for CRC calculation. Some are also required for the table
 * generation above.
 */

/*
  Return a(x) multiplied by b(x) modulo p(x), where p(x) is the CRC polynomial,
  reflected. For speed, this requires that a not be zero.
 */
static uint32_t multmodp(uint32_t a, uint32_t b)
{
    uint32_t m, p;

    m = (uint32_t)1 << 31;
    p = 0;
    for (;;) {
        if (a & m) {
            p ^= b;
            if ((a & (m - 1)) == 0)
                break;
        }
        m >>= 1;
        b = b & 1 ? (b >> 1) ^ POLY : b >> 1;
    }
    return p;
}

/*
  Return x^(n * 2^k) modulo p(x). Requires that x2n_table[] has been
  initialized.
 */
static uint32_t x2nmodp(int64_t n, uint32_t k)
{
    uint32_t p;

    p = (uint32_t)1 << 31;           /* x^0 == 1 */
    while (n) {
        if (n & 1)
            p = multmodp(x2n_table[k & 31], p);
        n >>= 1;
        k++;
    }
    return p;
}

const uint32_t * get_crc_table()
{
    return (const uint32_t *)crc_table;
}

/* =========================================================================
 * Use ARM machine instructions if available. This will compute the CRC about
 * ten times faster than the braided calculation. This code does not check for
 * the presence of the CRC instruction at run time. __ARM_FEATURE_CRC32 will
 * only be defined if the compilation specifies an ARM processor architecture
 * that has the instructions. For example, compiling with -march=armv8.1-a or
 * -march=armv8-a+crc, or -march=native if the compile machine has the crc32
 * instructions.
 */
#ifdef ARMCRC32

/*
   Constants empirically determined to maximize speed. These values are from
   measurements on a Cortex-A57. Your mileage may vary.
 */
#define Z_BATCH 3990                /* number of words in a batch */
#define Z_BATCH_ZEROS 0xa10d3d0c    /* computed from Z_BATCH = 3990 */
#define Z_BATCH_MIN 800             /* fewest words in a final batch */

uint64_t crc32_z(crc, buf, len)
    uint64_t crc;
    const uint8_t *buf;
    size_t len;
{
    uint32_t val;
    z_word_t crc1, crc2;
    const z_word_t *word;
    z_word_t val0, val1, val2;
    size_t last, last2, i;
    size_t num;

    /* Return initial CRC, if requested. */
    if (buf == NULL) return 0;

    /* Pre-condition the CRC */
    crc = (~crc) & 0xffffffff;

    /* Compute the CRC up to a word boundary. */
    while (len && ((size_t)buf & 7) != 0) {
        len--;
        val = *buf++;
        __asm__ volatile("crc32b %w0, %w0, %w1" : "+r"(crc) : "r"(val));
    }

    /* Prepare to compute the CRC on full 64-bit words word[0..num-1]. */
    word = (z_word_t const *)buf;
    num = len >> 3;
    len &= 7;

    /* Do three interleaved CRCs to realize the throughput of one crc32x
       instruction per cycle. Each CRC is calculated on Z_BATCH words. The
       three CRCs are combined into a single CRC after each set of batches. */
    while (num >= 3 * Z_BATCH) {
        crc1 = 0;
        crc2 = 0;
        for (i = 0; i < Z_BATCH; i++) {
            val0 = word[i];
            val1 = word[i + Z_BATCH];
            val2 = word[i + 2 * Z_BATCH];
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc) : "r"(val0));
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc1) : "r"(val1));
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc2) : "r"(val2));
        }
        word += 3 * Z_BATCH;
        num -= 3 * Z_BATCH;
        crc = multmodp(Z_BATCH_ZEROS, crc) ^ crc1;
        crc = multmodp(Z_BATCH_ZEROS, crc) ^ crc2;
    }

    /* Do one last smaller batch with the remaining words, if there are enough
       to pay for the combination of CRCs. */
    last = num / 3;
    if (last >= Z_BATCH_MIN) {
        last2 = last << 1;
        crc1 = 0;
        crc2 = 0;
        for (i = 0; i < last; i++) {
            val0 = word[i];
            val1 = word[i + last];
            val2 = word[i + last2];
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc) : "r"(val0));
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc1) : "r"(val1));
            __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc2) : "r"(val2));
        }
        word += 3 * last;
        num -= 3 * last;
        val = x2nmodp(last, 6);
        crc = multmodp(val, crc) ^ crc1;
        crc = multmodp(val, crc) ^ crc2;
    }

    /* Compute the CRC on any remaining words. */
    for (i = 0; i < num; i++) {
        val0 = word[i];
        __asm__ volatile("crc32x %w0, %w0, %x1" : "+r"(crc) : "r"(val0));
    }
    word += num;

    /* Complete the CRC on any remaining bytes. */
    buf = (const uint8_t *)word;
    while (len) {
        len--;
        val = *buf++;
        __asm__ volatile("crc32b %w0, %w0, %w1" : "+r"(crc) : "r"(val));
    }

    /* Return the CRC, post-conditioned. */
    return crc ^ 0xffffffff;
}

#else

#ifdef W

/*
  Return the CRC of the W bytes in the word_t data, taking the
  least-significant byte of the word as the first byte of data, without any pre
  or post conditioning. This is used to combine the CRCs of each braid.
 */
static uint32_t crc_word(z_word_t data)
{
    int k;
    for (k = 0; k < W; k++)
        data = (data >> 8) ^ crc_table[data & 0xff];
    return (uint32_t)data;
}

static z_word_t crc_word_big(z_word_t data)
{
    int k;
    for (k = 0; k < W; k++)
        data = (data << 8) ^
            crc_big_table[(data >> ((W - 1) << 3)) & 0xff];
    return data;
}

#endif

/* ========================================================================= */
uint64_t crc32_z(uint64_t crc, const uint8_t * buf, size_t len)
{
    /* Return initial CRC, if requested. */
    if (buf == NULL) return 0;

    /* Pre-condition the CRC */
    crc = (~crc) & 0xffffffff;

#ifdef W

    /* If provided enough bytes, do a braided CRC calculation. */
    if (len >= N * W + W - 1) {
        size_t blks;
        z_word_t const *words;
        uint32_t endian;
        int k;

        /* Compute the CRC up to a z_word_t boundary. */
        while (len && ((size_t)buf & (W - 1)) != 0) {
            len--;
            crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        }

        /* Compute the CRC on as many N z_word_t blocks as are available. */
        blks = len / (N * W);
        len -= blks * N * W;
        words = (z_word_t const *)buf;

        /* Do endian check at execution time instead of compile time, since ARM
           processors can change the endianess at execution time. If the
           compiler knows what the endianess will be, it can optimize out the
           check and the unused branch. */
        endian = 1;
        if (*(uint8_t *)&endian) {
            /* Little endian. */

            uint32_t crc0;
            z_word_t word0;
#if N > 1
            uint32_t crc1;
            z_word_t word1;
#if N > 2
            uint32_t crc2;
            z_word_t word2;
#if N > 3
            uint32_t crc3;
            z_word_t word3;
#if N > 4
            uint32_t crc4;
            z_word_t word4;
#if N > 5
            uint32_t crc5;
            z_word_t word5;
#endif
#endif
#endif
#endif
#endif

            /* Initialize the CRC for each braid. */
            crc0 = crc;
#if N > 1
            crc1 = 0;
#if N > 2
            crc2 = 0;
#if N > 3
            crc3 = 0;
#if N > 4
            crc4 = 0;
#if N > 5
            crc5 = 0;
#endif
#endif
#endif
#endif
#endif

            /*
              Process the first blks-1 blocks, computing the CRCs on each braid
              independently.
             */
            while (--blks) {
                /* Load the word for each braid into registers. */
                word0 = crc0 ^ words[0];
#if N > 1
                word1 = crc1 ^ words[1];
#if N > 2
                word2 = crc2 ^ words[2];
#if N > 3
                word3 = crc3 ^ words[3];
#if N > 4
                word4 = crc4 ^ words[4];
#if N > 5
                word5 = crc5 ^ words[5];
#endif
#endif
#endif
#endif
#endif
                words += N;

                /* Compute and update the CRC for each word. The loop should
                   get unrolled. */
                crc0 = crc_braid_table[0][word0 & 0xff];
#if N > 1
                crc1 = crc_braid_table[0][word1 & 0xff];
#if N > 2
                crc2 = crc_braid_table[0][word2 & 0xff];
#if N > 3
                crc3 = crc_braid_table[0][word3 & 0xff];
#if N > 4
                crc4 = crc_braid_table[0][word4 & 0xff];
#if N > 5
                crc5 = crc_braid_table[0][word5 & 0xff];
#endif
#endif
#endif
#endif
#endif
                for (k = 1; k < W; k++) {
                    crc0 ^= crc_braid_table[k][(word0 >> (k << 3)) & 0xff];
#if N > 1
                    crc1 ^= crc_braid_table[k][(word1 >> (k << 3)) & 0xff];
#if N > 2
                    crc2 ^= crc_braid_table[k][(word2 >> (k << 3)) & 0xff];
#if N > 3
                    crc3 ^= crc_braid_table[k][(word3 >> (k << 3)) & 0xff];
#if N > 4
                    crc4 ^= crc_braid_table[k][(word4 >> (k << 3)) & 0xff];
#if N > 5
                    crc5 ^= crc_braid_table[k][(word5 >> (k << 3)) & 0xff];
#endif
#endif
#endif
#endif
#endif
                }
            }

            /*
              Process the last block, combining the CRCs of the N braids at the
              same time.
             */
            crc = crc_word(crc0 ^ words[0]);
#if N > 1
            crc = crc_word(crc1 ^ words[1] ^ crc);
#if N > 2
            crc = crc_word(crc2 ^ words[2] ^ crc);
#if N > 3
            crc = crc_word(crc3 ^ words[3] ^ crc);
#if N > 4
            crc = crc_word(crc4 ^ words[4] ^ crc);
#if N > 5
            crc = crc_word(crc5 ^ words[5] ^ crc);
#endif
#endif
#endif
#endif
#endif
            words += N;
        }
        else {
            /* Big endian. */

            z_word_t crc0, word0, comb;
#if N > 1
            z_word_t crc1, word1;
#if N > 2
            z_word_t crc2, word2;
#if N > 3
            z_word_t crc3, word3;
#if N > 4
            z_word_t crc4, word4;
#if N > 5
            z_word_t crc5, word5;
#endif
#endif
#endif
#endif
#endif

            /* Initialize the CRC for each braid. */
            crc0 = byte_swap(crc);
#if N > 1
            crc1 = 0;
#if N > 2
            crc2 = 0;
#if N > 3
            crc3 = 0;
#if N > 4
            crc4 = 0;
#if N > 5
            crc5 = 0;
#endif
#endif
#endif
#endif
#endif

            /*
              Process the first blks-1 blocks, computing the CRCs on each braid
              independently.
             */
            while (--blks) {
                /* Load the word for each braid into registers. */
                word0 = crc0 ^ words[0];
#if N > 1
                word1 = crc1 ^ words[1];
#if N > 2
                word2 = crc2 ^ words[2];
#if N > 3
                word3 = crc3 ^ words[3];
#if N > 4
                word4 = crc4 ^ words[4];
#if N > 5
                word5 = crc5 ^ words[5];
#endif
#endif
#endif
#endif
#endif
                words += N;

                /* Compute and update the CRC for each word. The loop should
                   get unrolled. */
                crc0 = crc_braid_big_table[0][word0 & 0xff];
#if N > 1
                crc1 = crc_braid_big_table[0][word1 & 0xff];
#if N > 2
                crc2 = crc_braid_big_table[0][word2 & 0xff];
#if N > 3
                crc3 = crc_braid_big_table[0][word3 & 0xff];
#if N > 4
                crc4 = crc_braid_big_table[0][word4 & 0xff];
#if N > 5
                crc5 = crc_braid_big_table[0][word5 & 0xff];
#endif
#endif
#endif
#endif
#endif
                for (k = 1; k < W; k++) {
                    crc0 ^= crc_braid_big_table[k][(word0 >> (k << 3)) & 0xff];
#if N > 1
                    crc1 ^= crc_braid_big_table[k][(word1 >> (k << 3)) & 0xff];
#if N > 2
                    crc2 ^= crc_braid_big_table[k][(word2 >> (k << 3)) & 0xff];
#if N > 3
                    crc3 ^= crc_braid_big_table[k][(word3 >> (k << 3)) & 0xff];
#if N > 4
                    crc4 ^= crc_braid_big_table[k][(word4 >> (k << 3)) & 0xff];
#if N > 5
                    crc5 ^= crc_braid_big_table[k][(word5 >> (k << 3)) & 0xff];
#endif
#endif
#endif
#endif
#endif
                }
            }

            /*
              Process the last block, combining the CRCs of the N braids at the
              same time.
             */
            comb = crc_word_big(crc0 ^ words[0]);
#if N > 1
            comb = crc_word_big(crc1 ^ words[1] ^ comb);
#if N > 2
            comb = crc_word_big(crc2 ^ words[2] ^ comb);
#if N > 3
            comb = crc_word_big(crc3 ^ words[3] ^ comb);
#if N > 4
            comb = crc_word_big(crc4 ^ words[4] ^ comb);
#if N > 5
            comb = crc_word_big(crc5 ^ words[5] ^ comb);
#endif
#endif
#endif
#endif
#endif
            words += N;
            crc = byte_swap(comb);
        }

        /*
          Update the pointer to the remaining bytes to process.
         */
        buf = (uint8_t const *)words;
    }

#endif /* W */

    /* Complete the computation of the CRC on any remaining bytes. */
    while (len >= 8) {
        len -= 8;
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
    }
    while (len) {
        len--;
        crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
    }

    /* Return the CRC, post-conditioned. */
    return crc ^ 0xffffffff;
}

#endif

/* ========================================================================= */
uint64_t crc32(uint64_t crc, const void * buf, uint32_t len)
{
    return crc32_z(crc, static_cast<const uint8_t *>(buf), len);
}

/* ========================================================================= */
uint64_t crc32_combine64(uint64_t crc1, uint64_t crc2, int64_t len2)
{
    return multmodp(x2nmodp(len2, 3), crc1) ^ (crc2 & 0xffffffff);
}

/* ========================================================================= */
uint64_t crc32_combine(uint64_t crc1, uint64_t crc2, int64_t len2)
{
    return crc32_combine64(crc1, crc2, (int64_t)len2);
}

/* ========================================================================= */
uint64_t crc32_combine_gen64(int64_t len2)
{
    return x2nmodp(len2, 3);
}

/* ========================================================================= */
uint64_t crc32_combine_gen(int64_t len2)
{
    return crc32_combine_gen64((int64_t)len2);
}

/* ========================================================================= */
uint64_t crc32_combine_op(uint64_t crc1, uint64_t crc2, uint64_t op)
{
    return multmodp(op, crc1) ^ (crc2 & 0xffffffff);
}
