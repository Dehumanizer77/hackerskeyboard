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
#include <utility>

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

// ---------------------------------------------------------------- real trie
// A well-formed dictionary built from a word list, used to check that the
// bounds added for safety did not break ordinary suggestion lookups.
struct TrieNode {
    std::vector<std::pair<unsigned char, TrieNode*> > kids;
    bool terminal;
    int freq;
    TrieNode() : terminal(false), freq(1) {}
    ~TrieNode() {
        for (size_t i = 0; i < kids.size(); ++i) delete kids[i].second;
    }
    TrieNode *child(unsigned char c) {
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i].first == c) return kids[i].second;
        }
        TrieNode *n = new TrieNode();
        kids.push_back(std::make_pair(c, n));
        return n;
    }
};

static void serializeGroup(std::vector<unsigned char> &out, TrieNode *node) {
    out.push_back((unsigned char)node->kids.size());
    std::vector<std::pair<size_t, TrieNode*> > patches;
    for (size_t i = 0; i < node->kids.size(); ++i) {
        unsigned char ch = node->kids[i].first;
        TrieNode *kid = node->kids[i].second;
        bool hasKids = !kid->kids.empty();
        out.push_back(ch);
        if (hasKids) {
            patches.push_back(std::make_pair(out.size(), kid));
            out.push_back((unsigned char)(kid->terminal ? (0x80 | 0x40) : 0x40));
            out.push_back(0);   // address, backpatched below
            out.push_back(0);
        } else {
            out.push_back((unsigned char)(kid->terminal ? 0x80 : 0x00));
        }
        if (kid->terminal) {
            out.push_back((unsigned char)(kid->freq & 0xFF));
            out.push_back(0x00);   // no bigrams
        }
    }
    for (size_t i = 0; i < patches.size(); ++i) {
        size_t at = patches[i].first;
        int addr = (int)out.size();
        out[at] = (unsigned char)((out[at] & 0xC0) | ((addr >> 16) & 0x3F));
        out[at + 1] = (unsigned char)((addr >> 8) & 0xFF);
        out[at + 2] = (unsigned char)(addr & 0xFF);
        serializeGroup(out, patches[i].second);
    }
}

static std::vector<unsigned char> buildDictionary(const std::vector<std::string> &words) {
    TrieNode root;
    for (size_t i = 0; i < words.size(); ++i) {
        TrieNode *n = &root;
        for (size_t j = 0; j < words[i].size(); ++j) {
            n = n->child((unsigned char)words[i][j]);
        }
        n->terminal = true;
        n->freq = 100 + (int)(words.size() - i);
    }
    std::vector<unsigned char> out;
    out.push_back(201);   // version
    out.push_back(0);     // no bigrams
    serializeGroup(out, &root);
    return out;
}

// Returns the suggestions produced for `typed`, in ranked order.
static std::vector<std::string> suggestionsFor(std::vector<unsigned char> &dict,
                                               const std::string &typed) {
    int *frequencies = new int[MAX_WORDS];
    unsigned short *outputChars = new unsigned short[MAX_WORDS * MAX_WORD_LENGTH];
    int *inputCodes = new int[MAX_WORD_LENGTH * MAX_ALTERNATIVES];
    memset(frequencies, 0, sizeof(int) * MAX_WORDS);
    memset(outputChars, 0, sizeof(unsigned short) * MAX_WORDS * MAX_WORD_LENGTH);
    for (int i = 0; i < MAX_WORD_LENGTH * MAX_ALTERNATIVES; ++i) inputCodes[i] = -1;
    for (size_t i = 0; i < typed.size(); ++i) {
        inputCodes[i * MAX_ALTERNATIVES] = (unsigned char)typed[i];
    }

    unsigned char *buf = new unsigned char[dict.size()];
    memcpy(buf, dict.data(), dict.size());
    Dictionary *d = new Dictionary(buf, TYPED_LETTER_MULTIPLIER, FULL_WORD_MULTIPLIER,
                                   (int)dict.size());
    int count = d->getSuggestions(inputCodes, (int)typed.size(), outputChars, frequencies,
                                  MAX_WORD_LENGTH, MAX_WORDS, MAX_ALTERNATIVES, -1, NULL, 0);
    std::vector<std::string> result;
    for (int j = 0; j < count; ++j) {
        if (frequencies[j] < 1) break;
        std::string w;
        for (int k = 0; k < MAX_WORD_LENGTH; ++k) {
            unsigned short c = outputChars[j * MAX_WORD_LENGTH + k];
            if (c == 0) break;
            w += (char)c;
        }
        if (!w.empty()) result.push_back(w);
    }
    delete d;
    delete[] buf;
    delete[] frequencies; delete[] outputChars; delete[] inputCodes;
    return result;
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

    int failures = 0;

    // Functional check first: the bounds added for safety must not have broken
    // ordinary lookups. A parser that returns nothing is "safe" and useless.
    printf("== functional: suggestions from a well-formed dictionary ==\n");
    {
        std::vector<std::string> words;
        words.push_back("keyboard"); words.push_back("key"); words.push_back("keys");
        words.push_back("keyed");    words.push_back("the"); words.push_back("there");
        words.push_back("their");    words.push_back("this");
        std::vector<unsigned char> dict = buildDictionary(words);
        printf("  dictionary: %d words, %d bytes\n", (int)words.size(), (int)dict.size());

        struct { const char *typed; const char *expect; } cases[] = {
            { "key",  "keyboard" },
            { "the",  "there"    },
            { "thi",  "this"     },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            std::vector<std::string> got = suggestionsFor(dict, cases[c].typed);
            bool found = false;
            std::string joined;
            for (size_t i = 0; i < got.size(); ++i) {
                joined += got[i]; joined += " ";
                if (got[i] == cases[c].expect) found = true;
            }
            printf("  typed \"%s\" -> [%s]%s\n", cases[c].typed, joined.c_str(),
                   found ? "" : "   <-- MISSING EXPECTED WORD");
            if (!found) failures++;
        }

        // Long words must still come back intact, right up to the width of one
        // output slot (MAX_WORD_LENGTH - 1 characters plus a terminator).
        // getWordsRec prunes at mInputLength * 3, so reaching depth 46 needs at
        // least 16 typed characters - that is the parser's own rule, not a
        // consequence of the bounds added here.
        std::vector<std::string> longWords;
        longWords.push_back(std::string(46, 'a'));
        longWords.push_back(std::string(47, 'a'));
        std::vector<unsigned char> longDict = buildDictionary(longWords);
        std::vector<std::string> got = suggestionsFor(longDict, std::string(16, 'a'));
        bool ok46 = false, ok47 = false;
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i].size() == 46) ok46 = true;
            if (got[i].size() == 47) ok47 = true;
        }
        printf("  46-character word returned intact: %s\n", ok46 ? "yes" : "NO");
        printf("  47-character word (slot limit) intact: %s\n", ok47 ? "yes" : "NO");
        if (!ok46) failures++;
        if (!ok47) failures++;
    }

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

    if (failures > 0) {
        printf("\n%d FUNCTIONAL CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nALL PASSED - suggestions correct, no out-of-bounds access detected\n");
    return 0;
}
