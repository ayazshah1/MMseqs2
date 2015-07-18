//
// Created by mad on 5/26/15.
//
#include "QueryTemplateMatcherExactMatch.h"
#include "QueryScoreLocal.h"

QueryTemplateMatcherExactMatch::QueryTemplateMatcherExactMatch(BaseMatrix *m, IndexTable *indexTable,
                                                               unsigned int *seqLens, short kmerThr,
                                                               double kmerMatchProb, int kmerSize, size_t dbSize,
                                                               unsigned int maxSeqLen, size_t maxHitsPerQuery)
        : QueryTemplateMatcher(m, indexTable, seqLens, kmerThr, kmerMatchProb, kmerSize, dbSize, false, maxSeqLen) {
    this->resList = (hit_t *) mem_align(ALIGN_INT, QueryScore::MAX_RES_LIST_LEN * sizeof(hit_t) );
    this->databaseHits = new CounterResult[MAX_DB_MATCHES];
    // data for histogram of score distribution
    this->scoreSizes = new unsigned int[QueryScoreLocal::SCORE_RANGE];
    memset(scoreSizes, 0, QueryScoreLocal::SCORE_RANGE * sizeof(unsigned int));

    this->maxHitsPerQuery = maxHitsPerQuery;
    this->counter = new CountInt32Array(dbSize << 4, MAX_DB_MATCHES/32 );
    // needed for p-value calc.
    this->mu = kmerMatchProb;
    this->logMatchProb = log(this->mu);
    this->seqLens = seqLens;
    this->logScoreFactorial = new double[QueryScoreLocal::SCORE_RANGE];
    QueryScoreLocal::computeFactorial(logScoreFactorial, QueryScoreLocal::SCORE_RANGE);

}

QueryTemplateMatcherExactMatch::~QueryTemplateMatcherExactMatch(){
    free(resList);
    delete [] scoreSizes;
    delete [] databaseHits;
    delete counter;
    delete [] logScoreFactorial;
}

size_t QueryTemplateMatcherExactMatch::evaluateBins(CounterResult *inputOutput, size_t N) {
    size_t localResultSize = 0;
    localResultSize += counter->countElements(inputOutput, N);
    return localResultSize;
}


std::pair<hit_t *, size_t> QueryTemplateMatcherExactMatch::matchQuery (Sequence * seq, unsigned int identityId){
    seq->resetCurrPos();
    memset(scoreSizes, 0, QueryScoreLocal::SCORE_RANGE * sizeof(unsigned int));
    match(seq);
    unsigned int thr = QueryScoreLocal::computeScoreThreshold(scoreSizes, this->maxHitsPerQuery);
    return getResult(seq->L, identityId, thr);
}

void QueryTemplateMatcherExactMatch::match(Sequence* seq){
    // go through the query sequence
    size_t kmerListLen = 0;
    size_t numMatches = 0;
    //size_t pos = 0;
    stats->diagonalOverflow = false;
    CounterResult* sequenceHits = databaseHits;
    CounterResult * lastSequenceHit = databaseHits + MAX_DB_MATCHES;
    size_t seqListSize;
    while(seq->hasNextKmer()){
        const int* kmer = seq->nextKmer();
        const ScoreMatrix kmerList = kmerGenerator->generateKmerList(kmer);
        kmerListLen += kmerList.elementSize;
        const unsigned short i =  seq->getCurrentPosition();
        // match the index table
        for (unsigned int kmerPos = 0; kmerPos < kmerList.elementSize; kmerPos++) {
            // generate k-mer list
            const IndexEntryLocal *entries = indexTable->getDBSeqList<IndexEntryLocal>(kmerList.index[kmerPos],
                                                                                       &seqListSize);
            // detected overflow while matching TODO smarter way
            seqListSize = (sequenceHits + seqListSize >= lastSequenceHit) ? 0 : seqListSize;

            for (unsigned int seqIdx = 0; LIKELY(seqIdx < seqListSize); seqIdx++) {
                IndexEntryLocal entry = entries[seqIdx];
                const unsigned char j = entry.position_j;
                const unsigned int seqId = entry.seqId;
                const unsigned char diagonal = (i - j);
                sequenceHits->id    = seqId;
                sequenceHits->count = diagonal;
                sequenceHits++;
            }
            numMatches += seqListSize;
        }
    }
    //fill the output
    CounterResult* inputOutputHits = databaseHits;
    size_t doubleMatches = evaluateBins(inputOutputHits, numMatches);
    updateScoreBins(inputOutputHits, doubleMatches);
    stats->doubleMatches = doubleMatches;
    stats->kmersPerPos   = ((double)kmerListLen/(double)seq->L);
    stats->querySeqLen   = seq->L;
    stats->dbMatches     = numMatches;
}

void QueryTemplateMatcherExactMatch::updateScoreBins(CounterResult *result, size_t elementCount) {
    for(size_t i = 0; i < elementCount; i++){
        scoreSizes[result[i].count]++;
    }
}

std::pair<hit_t *, size_t>  QueryTemplateMatcherExactMatch::getResult(const int l,
                                                                      const unsigned int id,
                                                                      const unsigned short thr) {
    size_t elementCounter = 0;
    size_t resultSize = stats->doubleMatches;
    if (id != UINT_MAX){
        hit_t * result = (resList + 0);
        const unsigned short rawScore  = l;
        result->seqId = id;
        result->prefScore = rawScore;
        //result->zScore = (((float)rawScore) - mu)/ sqrtMu;
        result->zScore = -QueryScoreLocal::computeLogProbability(rawScore, seqLens[id],
                                                                 mu, logMatchProb, logScoreFactorial[rawScore]);
        elementCounter++;
    }

    for (size_t i = 0; i < resultSize; i++) {
        const unsigned int seqIdCurr = databaseHits[i].id;
        const unsigned int scoreCurr = databaseHits[i].count;
        // write result to list
        if(scoreCurr > thr && id != seqIdCurr){
            hit_t *result = (resList + elementCounter);
            result->seqId = seqIdCurr;
            result->prefScore = scoreCurr;

            result->zScore = -QueryScoreLocal::computeLogProbability(scoreCurr, seqLens[seqIdCurr],
                                                                     mu, logMatchProb, logScoreFactorial[scoreCurr]);

            elementCounter++;
            if (elementCounter >= QueryScore::MAX_RES_LIST_LEN)
                break;
        }
    }

    if(elementCounter > 1){
        if (id != UINT_MAX){
            std::sort(resList + 1, resList + elementCounter, QueryScore::compareHits);
        }
        else{
            std::sort(resList, resList + elementCounter, QueryScore::compareHits);
        }
    }
    std::pair<hit_t *, size_t>  pair = std::make_pair(this->resList, elementCounter);
    return pair;
}