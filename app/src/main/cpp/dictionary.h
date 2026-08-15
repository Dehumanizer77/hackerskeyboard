/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LATINIME_DICTIONARY_H
#define LATINIME_DICTIONARY_H

namespace latinime {

// 22-bit address = ~4MB dictionary size limit, which on average would be about 200k-300k words
#define ADDRESS_MASK 0x3FFFFF

// The bit that decides if an address follows in the next 22 bits
#define FLAG_ADDRESS_MASK 0x40
// The bit that decides if this is a terminal node for a word. The node could still have children,
// if the word has other endings.
#define FLAG_TERMINAL_MASK 0x80

#define FLAG_BIGRAM_READ 0x80
#define FLAG_BIGRAM_CHILDEXIST 0x40
#define FLAG_BIGRAM_CONTINUED 0x80
#define FLAG_BIGRAM_FREQ 0x7F

// Size of the internal word-composition buffer.
#define MAX_WORD_BUFFER 128

// The trie structure and the bigram continuation bits come from the dictionary
// file, which is not trustworthy: a dictionary can be supplied by any installed
// package (see SECURITY-REVIEW.md, HK-02). Every walk over that structure is
// therefore bounded by an iteration count as well as by the buffer length.
#define MAX_BIGRAM_ENTRIES 1024
#define MAX_TRAVERSE_STEPS 4096

// Total number of trie nodes a single lookup may visit. Child addresses are
// read from the file and may point backwards, so the "tree" can contain cycles
// and a node group can declare up to 255 children: without a budget the search
// is exponential in the depth and a crafted dictionary freezes the keyboard on
// every keystroke. A real lookup visits a few thousand nodes at most.
#define MAX_NODE_VISITS 200000

class Dictionary {
public:
    Dictionary(void *dict, int typedLetterMultipler, int fullWordMultiplier, int dictSize);
    int getSuggestions(int *codes, int codesSize, unsigned short *outWords, int *frequencies,
            int maxWordLength, int maxWords, int maxAlternatives, int skipPos,
            int *nextLetters, int nextLettersSize);
    int getBigrams(unsigned short *word, int length, int *codes, int codesSize,
            unsigned short *outWords, int *frequencies, int maxWordLength, int maxBigrams,
            int maxAlternatives);
    bool isValidWord(unsigned short *word, int length);
    void setAsset(void *asset) { mAsset = asset; }
    void *getAsset() { return mAsset; }
    ~Dictionary();

private:

    void getVersionNumber();
    bool checkIfDictVersionIsLatest();
    int getAddress(int *pos);
    int getBigramAddress(int *pos, bool advance);
    int getFreq(int *pos);
    int getBigramFreq(int *pos);
    void searchForTerminalNode(int address, int frequency);
    void skipBigrams(int *pos);

    // Every read of the dictionary buffer goes through these. inRange() checks
    // the whole span that is about to be touched, not just the start offset:
    // the original code validated *pos and then read *pos+1 and *pos+2.
    bool inRange(int pos, int len) const {
        return pos >= 0 && len >= 0 && len <= mDictSize && pos <= mDictSize - len;
    }
    unsigned char byteAt(int pos) const { return inRange(pos, 1) ? mDict[pos] : 0; }

    bool getFirstBitOfByte(int *pos) { return (byteAt(*pos) & 0x80) > 0; }
    bool getSecondBitOfByte(int *pos) { return (byteAt(*pos) & 0x40) > 0; }
    bool getTerminal(int *pos) { return (byteAt(*pos) & FLAG_TERMINAL_MASK) > 0; }
    int getCount(int *pos) { int p = (*pos)++; return byteAt(p) & 0xFF; }
    unsigned short getChar(int *pos);
    int wideStrLen(unsigned short *str);

    bool sameAsTyped(unsigned short *word, int length);
    bool checkFirstCharacter(unsigned short *word);
    bool addWord(unsigned short *word, int length, int frequency);
    bool addWordBigram(unsigned short *word, int length, int frequency);
    unsigned short toLowerCase(unsigned short c);
    void getWordsRec(int pos, int depth, int maxDepth, bool completion, int frequency,
            int inputIndex, int diffs);
    int isValidWordRec(int pos, unsigned short *word, int offset, int length);
    void registerNextLetter(unsigned short c);

    unsigned char *mDict;
    void *mAsset;

    int *mFrequencies;
    int *mBigramFreq;
    int mMaxWords;
    int mMaxBigrams;
    int mMaxWordLength;
    unsigned short *mOutputChars;
    unsigned short *mBigramChars;
    int *mInputCodes;
    int mInputLength;
    int mMaxAlternatives;
    unsigned short mWord[MAX_WORD_BUFFER];
    int mSkipPos;
    int mMaxEditDistance;

    int mFullWordMultiplier;
    int mTypedLetterMultiplier;
    int mDictSize;
    int *mNextLettersFrequencies;
    int mNextLettersSize;
    int mVersion;
    int mBigram;
    // Work budget for one lookup; see MAX_NODE_VISITS.
    int mNodeVisits;
};

// ----------------------------------------------------------------------------

}; // namespace latinime

#endif // LATINIME_DICTIONARY_H
