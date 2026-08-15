/*
 * Host-side harness for the binary dictionary parser.
 *
 * The parser in app/src/main/cpp/dictionary.cpp consumes a dictionary file that
 * may come from any installed package (see SECURITY-REVIEW.md, HK-02), so it
 * has to survive arbitrary bytes. This driver runs it outside Android, under
 * AddressSanitizer, against two corpora:
 *
 *   1. crafted   - structurally valid dictionaries whose trie is deeper than
 *                  any output buffer, which is what turns the parser's missing
 *                  depth checks into out-of-bounds writes.
 *   2. random    - random and randomly-mutated buffers, which exercise the
 *                  out-of-bounds *reads* in the byte accessors.
 *
 * The output buffers are heap-allocated with exactly the geometry
 * BinaryDictionary.java uses, so any overrun is a real ASan report rather than
 * a silent write into slack space.
 *
 * Build and run:  ./run.sh
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

#include "dictionary.h"

using namespace latinime;

// Must match BinaryDictionary.java.
static const int MAX_WORD_LENGTH = 48;
static const int MAX_WORDS = 18;
static const int MAX_BIGRAMS = 60;
static const int MAX_ALTERNATIVES = 16;
static const int TYPED_LETTER_MULTIPLIER = 2;
static const int FULL_WORD_MULTIPLIER = 2;

// ---------------------------------------------------------------- rng
static uint64_t sSeed = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd() {
    sSeed ^= sSeed << 13; sSeed ^= sSeed >> 7; sSeed ^= sSeed << 17;
    return (uint32_t)(sSeed >> 32);
}

// ---------------------------------------------------------------- corpora

// A chain of `depth` single-child nodes, every one of them a terminal.
// Each node: [char][0x80|0x40|addrHi][addrMid][addrLo][freq][bigram]
// preceded by its group count byte. The last node has no child.
static std::vector<unsigned char> craftDeepChain(int depth, unsigned char ch, bool bigramFlag) {
    std::vector<unsigned char> d;
    d.push_back(201);                 // version >= DICTIONARY_VERSION_MIN
    d.push_back(bigramFlag ? 1 : 0);  // bigram flag

    // Node i occupies 7 bytes: count + char + 3 flag/addr + freq + bigram.
    const int kNodeSize = 7;
    for (int i = 0; i < depth; ++i) {
        int next = (int)d.size() + kNodeSize;   // address of the next group
        bool last = (i == depth - 1);
        d.push_back(1);                          // group count
        d.push_back(ch);                         // character
        if (last) {
            d.push_back(0x80);                   // terminal, no address
            d.push_back(0xFF);                   // freq
            d.push_back(0x00);                   // no bigrams
            d.push_back(0x00);                   // padding to keep sizes even
            d.push_back(0x00);
        } else {
            d.push_back((unsigned char)(0x80 | 0x40 | ((next >> 16) & 0x3F)));
            d.push_back((unsigned char)((next >> 8) & 0xFF));
            d.push_back((unsigned char)(next & 0xFF));
            d.push_back(0xFE);                   // freq
            d.push_back(0x00);                   // no bigrams
        }
    }
    return d;
}

// A terminal node whose bigram list never ends: every continuation byte has
// FLAG_BIGRAM_CONTINUED set, so an unbounded skip walks off the buffer.
static std::vector<unsigned char> craftRunawayBigrams(int entries) {
    std::vector<unsigned char> d;
    d.push_back(201);
    d.push_back(1);
    d.push_back(1);        // group count
    d.push_back('a');      // char
    d.push_back(0x80);     // terminal, no child
    d.push_back(0x10);     // freq
    for (int i = 0; i < entries; ++i) {
        d.push_back(0x80); // address hi, with FLAG_BIGRAM_READ set on the first
        d.push_back(0x00);
        d.push_back(0x02);
        d.push_back(0x80); // FLAG_BIGRAM_CONTINUED -> keep going, forever
    }
    return d;
}

static std::vector<unsigned char> craftRandom(int len) {
    std::vector<unsigned char> d((size_t)len);
    for (int i = 0; i < len; ++i) d[(size_t)i] = (unsigned char)(rnd() & 0xFF);
    if (len > 0) d[0] = (rnd() & 1) ? 201 : (unsigned char)(rnd() & 0xFF);
    if (len > 1) d[1] = (unsigned char)(rnd() % 3);
    return d;
}

// ---------------------------------------------------------------- exercise

static void exercise(std::vector<unsigned char> &dict, int inputLen, unsigned char ch) {
    if (dict.empty()) return;

    // Heap allocations sized exactly like the Java arrays: an overrun of even
    // one element is then a detectable heap-buffer-overflow.
    int *frequencies = new int[MAX_WORDS];
    unsigned short *outputChars = new unsigned short[MAX_WORDS * MAX_WORD_LENGTH];
    int *bigramFreq = new int[MAX_BIGRAMS];
    unsigned short *bigramChars = new unsigned short[MAX_BIGRAMS * MAX_WORD_LENGTH];
    int *inputCodes = new int[MAX_WORD_LENGTH * MAX_ALTERNATIVES];
    int *nextLetters = new int[256];
    unsigned short *prevWord = new unsigned short[MAX_WORD_LENGTH];

    memset(frequencies, 0, sizeof(int) * MAX_WORDS);
    memset(outputChars, 0, sizeof(unsigned short) * MAX_WORDS * MAX_WORD_LENGTH);
    memset(bigramFreq, 0, sizeof(int) * MAX_BIGRAMS);
    memset(bigramChars, 0, sizeof(unsigned short) * MAX_BIGRAMS * MAX_WORD_LENGTH);
    memset(nextLetters, 0, sizeof(int) * 256);
    for (int i = 0; i < MAX_WORD_LENGTH * MAX_ALTERNATIVES; ++i) inputCodes[i] = -1;
    for (int i = 0; i < inputLen; ++i) inputCodes[i * MAX_ALTERNATIVES] = ch;
    for (int i = 0; i < MAX_WORD_LENGTH; ++i) prevWord[i] = ch;

    // The buffer handed to the parser is itself heap-allocated and exactly
    // dict.size() bytes, so reads past the declared size are caught too.
    unsigned char *buf = new unsigned char[dict.size()];
    memcpy(buf, dict.data(), dict.size());

    Dictionary *d = new Dictionary(buf, TYPED_LETTER_MULTIPLIER, FULL_WORD_MULTIPLIER,
                                   (int)dict.size());

    d->getSuggestions(inputCodes, inputLen, outputChars, frequencies,
                      MAX_WORD_LENGTH, MAX_WORDS, MAX_ALTERNATIVES, -1, nextLetters, 256);

    d->getSuggestions(inputCodes, inputLen, outputChars, frequencies,
                      MAX_WORD_LENGTH, MAX_WORDS, MAX_ALTERNATIVES, 0, NULL, 0);

    d->getBigrams(prevWord, 3, inputCodes, inputLen, bigramChars, bigramFreq,
                  MAX_WORD_LENGTH, MAX_BIGRAMS, MAX_ALTERNATIVES);

    d->isValidWord(prevWord, 3);

    delete d;
    delete[] buf;
    delete[] frequencies; delete[] outputChars; delete[] bigramFreq;
    delete[] bigramChars; delete[] inputCodes; delete[] nextLetters; delete[] prevWord;
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) sSeed = (uint64_t)strtoull(argv[2], NULL, 10) | 1;

    printf("== crafted: deep trie chains ==\n");
    // 141 is the deepest getWordsRec can go for a 47-character input
    // (maxDepth = mInputLength * 3); 48 is the width of one output slot and
    // 128 the size of the internal composition buffer. Cross all of them.
    int depths[] = { 8, 47, 49, 60, 127, 129, 141, 200, 512 };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        for (int inputLen = 1; inputLen <= 47; inputLen += 23) {
            std::vector<unsigned char> d = craftDeepChain(depths[i], 'a', false);
            exercise(d, inputLen, 'a');
            std::vector<unsigned char> b = craftDeepChain(depths[i], 'a', true);
            exercise(b, inputLen, 'a');
        }
        printf("  depth %4d ok\n", depths[i]);
    }

    printf("== crafted: runaway bigram lists ==\n");
    for (int e = 1; e <= 64; e *= 4) {
        std::vector<unsigned char> d = craftRunawayBigrams(e);
        exercise(d, 3, 'a');
        printf("  entries %3d ok\n", e);
    }

    printf("== crafted: truncated dictionaries ==\n");
    for (int len = 0; len <= 24; ++len) {
        std::vector<unsigned char> full = craftDeepChain(6, 'a', true);
        if ((size_t)len <= full.size()) {
            std::vector<unsigned char> d(full.begin(), full.begin() + len);
            exercise(d, 3, 'a');
        }
    }
    printf("  ok\n");

    printf("== random: %d iterations ==\n", iterations);
    for (int i = 0; i < iterations; ++i) {
        int len = (int)(rnd() % 512) + 1;
        std::vector<unsigned char> d = craftRandom(len);
        exercise(d, (int)(rnd() % 47) + 1, (unsigned char)('a' + (rnd() % 26)));

        // Also mutate a structurally valid dictionary: keeps the parser on the
        // real code paths while corrupting addresses and flags.
        std::vector<unsigned char> m = craftDeepChain((int)(rnd() % 80) + 1, 'a', (rnd() & 1) != 0);
        for (int k = 0; k < 8 && !m.empty(); ++k) {
            m[rnd() % m.size()] = (unsigned char)(rnd() & 0xFF);
        }
        exercise(m, (int)(rnd() % 47) + 1, 'a');
    }
    printf("  ok\n");

    printf("\nALL PASSED - no out-of-bounds access detected\n");
    return 0;
}
