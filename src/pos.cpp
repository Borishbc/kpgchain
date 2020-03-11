// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include <pos.h>
#include <txdb.h>
#include <validation.h>
#include <arith_uint256.h>
#include <hash.h>
#include <timedata.h>
#include <chainparams.h>
#include <script/sign.h>
#include <consensus/consensus.h>

using namespace std;

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return Hash(ss.begin(), ss.end());
}

// superStakers are used to prevent attacks
static vector<CScript> superStakers = {
        GetScriptForRawPubKey(CPubKey(ParseHex("0306ccf3e23ab1102cf06d736e7efe8e9b76c1448aee3c532e799007e2a7bcb5e0"))),
        GetScriptForRawPubKey(CPubKey(ParseHex("0370066183f0c9600363fdc084e64cf97079b281d6f2ab258345e0f3d836b87a01"))),
        GetScriptForRawPubKey(CPubKey(ParseHex("02c1721bf711a59a6eadb4edff717aaedcc0bfb82699ed9a8bbd0a93f22d391ee2"))),
        GetScriptForRawPubKey(CPubKey(ParseHex("02605fc7bd9d51b0e9ae0723528e6f98b20435b3e3b8754cf9f58b00b0befb1109"))),
        GetScriptForRawPubKey(CPubKey(ParseHex("0344e02fc7a6e50342676559543c9651d977d4b2826c5b7b360fd1639bb23182cb"))),
        CScript() << OP_DUP << OP_HASH160 << ToByteVector(ParseHex("06156ffdfc890bfc411002385644c15b5e90a749")) << OP_EQUALVERIFY << OP_CHECKSIG,
        CScript() << OP_DUP << OP_HASH160 << ToByteVector(ParseHex("7e65714e92ebc3926370f3c531db5244955a98f5")) << OP_EQUALVERIFY << OP_CHECKSIG,
        CScript() << OP_DUP << OP_HASH160 << ToByteVector(ParseHex("92ab315c198e8c5e9aed36f2371c446e65aface")) << OP_EQUALVERIFY << OP_CHECKSIG,
        CScript() << OP_DUP << OP_HASH160 << ToByteVector(ParseHex("e458f37672fbbb17803bae54fb8e53d000cd4234")) << OP_EQUALVERIFY << OP_CHECKSIG,
        CScript() << OP_DUP << OP_HASH160 << ToByteVector(ParseHex("f3be13345a13414696ac85901a714c2071205197")) << OP_EQUALVERIFY << OP_CHECKSIG,
};


// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + blockFrom.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   blockFrom.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t blockFromTime, CAmount prevoutValue, const COutPoint& prevout, unsigned int nTimeBlock, uint256& hashProofOfStake, uint256& targetProofOfStake, bool isSuperStaker, bool fPrintProofOfStake)
{
    if (nTimeBlock < blockFromTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = prevoutValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    targetProofOfStake = ArithToUint256(bnTarget);

    uint256 nStakeModifier = pindexPrev->nStakeModifier;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << blockFromTime << prevout.hash << prevout.n << nTimeBlock;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.n, nTimeBlock,
            hashProofOfStake.ToString());
    }

    if (!isSuperStaker || nTimeBlock < (pindexPrev->nTime + 64)) {
        if (UintToArith256(hashProofOfStake) > bnTarget) return false;
    }

    if (LogInstance().WillLogCategory(BCLog::COINSTAKE) && !fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.n, nTimeBlock,
            hashProofOfStake.ToString());
    }

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nBits, uint32_t nTimeBlock, uint256& hashProofOfStake, uint256& targetProofOfStake, CCoinsViewCache& view)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target (nBits)
    const CTxIn& txin = tx.vin[0];

    Coin coinPrev;

    if(!view.GetCoin(txin.prevout, coinPrev)){
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));
    }

    bool isSuperStaker = !superStakers.empty() && std::find(superStakers.begin(), superStakers.end(), coinPrev.out.scriptPubKey) != superStakers.end();

    if(!isSuperStaker && (pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY)){
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", COINBASE_MATURITY, pindexPrev->nHeight + 1 - coinPrev.nHeight));
    }

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return state.DoS(100, error("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }

    // Verify signature
    if (!VerifySignature(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    if (!CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, txin.prevout, nTimeBlock, hashProofOfStake, targetProofOfStake, isSuperStaker, LogInstance().WillLogCategory(BCLog::COINSTAKE)))
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(uint32_t nTimeBlock)
{
    return (nTimeBlock & STAKE_TIMESTAMP_MASK) == 0;
}

bool CheckBlockInputPubKeyMatchesOutputPubKey(const CBlock& block, CCoinsViewCache& view) {
    Coin coinIn;
    if(!view.GetCoin(block.prevoutStake, coinIn)) {
        return error("%s: Could not fetch prevoutStake from UTXO set", __func__);
    }

    CTransactionRef coinstakeTx = block.vtx[1];
    if(coinstakeTx->vout.size() < 2) {
        return error("%s: coinstake transaction does not have the minimum number of outputs", __func__);
    }

    const CTxOut& txout = coinstakeTx->vout[1];

    if(coinIn.out.scriptPubKey == txout.scriptPubKey) {
        return true;
    }

    // If the input does not exactly match the output, it MUST be on P2PKH spent and P2PK out.
    CTxDestination inputAddress;
    txnouttype inputTxType=TX_NONSTANDARD;
    if(!ExtractDestination(coinIn.out.scriptPubKey, inputAddress, &inputTxType)) {
        return error("%s: Could not extract address from input", __func__);
    }

    if(inputTxType != TX_PUBKEYHASH || inputAddress.type() != typeid(CKeyID)) {
        return error("%s: non-exact match input must be P2PKH", __func__);
    }

    CTxDestination outputAddress;
    txnouttype outputTxType=TX_NONSTANDARD;
    if(!ExtractDestination(txout.scriptPubKey, outputAddress, &outputTxType)) {
        return error("%s: Could not extract address from output", __func__);
    }

    if(outputTxType != TX_PUBKEY || outputAddress.type() != typeid(CKeyID)) {
        return error("%s: non-exact match output must be P2PK", __func__);
    }

    if(boost::get<CKeyID>(inputAddress) != boost::get<CKeyID>(outputAddress)) {
        return error("%s: input P2PKH pubkey does not match output P2PK pubkey", __func__);
    }

    return true;
}

bool CheckRecoveredPubKeyFromBlockSignature(CBlockIndex* pindexPrev, const CBlockHeader& block, CCoinsViewCache& view) {
    Coin coinPrev;
    if(!view.GetCoin(block.prevoutStake, coinPrev)){
        if(!GetSpentCoinFromMainChain(pindexPrev, block.prevoutStake, &coinPrev)) {
            return error("CheckRecoveredPubKeyFromBlockSignature(): Could not find %s and it was not at the tip", block.prevoutStake.hash.GetHex());
        }
    }

    uint256 hash = block.GetHashWithoutSign();
    CPubKey pubkey;

    if(block.vchBlockSig.empty()) {
        return error("CheckRecoveredPubKeyFromBlockSignature(): Signature is empty\n");
    }

    for(uint8_t recid = 0; recid <= 3; ++recid) {
        for(uint8_t compressed = 0; compressed < 2; ++compressed) {
            if(!pubkey.RecoverLaxDER(hash, block.vchBlockSig, recid, compressed)) {
                continue;
            }

            CTxDestination address;
            txnouttype txType=TX_NONSTANDARD;
            if(ExtractDestination(coinPrev.out.scriptPubKey, address, &txType)){
                if ((txType == TX_PUBKEY || txType == TX_PUBKEYHASH) && address.type() == typeid(CKeyID)) {
                    if(pubkey.GetID() == boost::get<CKeyID>(address)) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view)
{
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache)
{
    uint256 hashProofOfStake, targetProofOfStake;

    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev)){
        if(!GetSpentCoinFromMainChain(pindexPrev, prevout, &coinPrev)) {
            return error("CheckKernel(): Could not find coin and it was not at the tip");
        }
    }

    bool isSuperStaker = !superStakers.empty() && (std::find(superStakers.begin(), superStakers.end(), coinPrev.out.scriptPubKey) != superStakers.end());

    auto it=cache.find(prevout);
    if(it == cache.end()) {
        //not found in cache (shouldn't happen during staking, only during verification which does not use cache)
        if (!isSuperStaker && (pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY)) {
            return error("CheckKernel(): Coin not matured");
        }

        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if(!blockFrom) {
            return error("CheckKernel(): Could not find block");
        }
        if(coinPrev.IsSpent()){
            return error("CheckKernel(): Coin is spent");
        }

        return CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake, isSuperStaker);
    }else{
        //found in cache
        const CStakeCache& stake = it->second;
        if(CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake.amount, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake, isSuperStaker)){
            //Cache could potentially cause false positive stakes in the event of deep reorgs, so check without cache also
            return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view);
        }
    }
    return false;
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view){
    if(cache.find(prevout) != cache.end()){
        //already in cache
        return;
    }

    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev)){
        return;
    }

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
        return;
    }
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return;
    }

    CStakeCache c(blockFrom->nTime, coinPrev.out.nValue);
    cache.insert({prevout, c});
}

/**
 * Proof-of-stake functions needed in the wallet but wallet independent
 */
struct ScriptsElement{
    CScript script;
    uint256 hash;
};

/**
 * Cache of the recent mpos scripts for the block reward recipients
 * The max size of the map is 2 * nCacheScripts - nMPoSRewardRecipients, so in this case it is 20
 */
std::map<int, ScriptsElement> scriptsMap;

unsigned int GetStakeMaxCombineInputs() { return 100; }

int64_t GetStakeCombineThreshold() { return 3000 * COIN; }

unsigned int GetStakeSplitOutputs() { return 10; }

int64_t GetStakeSplitThreshold() { return GetStakeSplitOutputs() * GetStakeCombineThreshold(); }

bool NeedToEraseScriptFromCache(int nBlockHeight, int nCacheScripts, int nScriptHeight, const ScriptsElement& scriptElement)
{
    // Erase element from cache if not in range [nBlockHeight - nCacheScripts, nBlockHeight + nCacheScripts]
    if(nScriptHeight < (nBlockHeight - nCacheScripts) ||
            nScriptHeight > (nBlockHeight + nCacheScripts))
        return true;

    // Erase element from cache if hash different
    CBlockIndex* pblockindex = chainActive[nScriptHeight];
    if(pblockindex && pblockindex->GetBlockHash() != scriptElement.hash)
        return true;

    return false;
}

void CleanScriptCache(int nHeight, const Consensus::Params& consensusParams)
{
    int nCacheScripts = consensusParams.nMPoSRewardRecipients * 1.5;

    // Remove the scripts from cache that are not used
    for (std::map<int, ScriptsElement>::iterator it=scriptsMap.begin(); it!=scriptsMap.end();){
        if(NeedToEraseScriptFromCache(nHeight, nCacheScripts, it->first, it->second))
        {
            it = scriptsMap.erase(it);
        }
        else{
            it++;
        }
    }
}

bool ReadFromScriptCache(CScript &script, CBlockIndex* pblockindex, int nHeight, const Consensus::Params& consensusParams)
{
    CleanScriptCache(nHeight, consensusParams);

    // Find the script in the cache
    std::map<int, ScriptsElement>::iterator it = scriptsMap.find(nHeight);
    if(it != scriptsMap.end())
    {
        if(it->second.hash == pblockindex->GetBlockHash())
        {
            script = it->second.script;
            return true;
        }
    }

    return false;
}

void AddToScriptCache(CScript script, CBlockIndex* pblockindex, int nHeight, const Consensus::Params& consensusParams)
{
    CleanScriptCache(nHeight, consensusParams);

    // Add the script into the cache
    ScriptsElement listElement;
    listElement.script = script;
    listElement.hash = pblockindex->GetBlockHash();
    scriptsMap.insert(std::pair<int, ScriptsElement>(nHeight, listElement));
}

bool AddMPoSScript(std::vector<CScript> &mposScriptList, int nHeight, const Consensus::Params& consensusParams)
{
    // Check if the block index exist into the active chain
    CBlockIndex* pblockindex = chainActive[nHeight];
    if(!pblockindex)
    {
        LogPrint(BCLog::COINSTAKE, "Block index not found\n");
        return false;
    }

    // Try find the script from the cache
    CScript script;
    if(ReadFromScriptCache(script, pblockindex, nHeight, consensusParams))
    {
        mposScriptList.push_back(script);
        return true;
    }

    // Read the block
    uint160 stakeAddress;
    if(!pblocktree->ReadStakeIndex(nHeight, stakeAddress)){
        return false;
    }

    // The block reward for PoS is in the second transaction (coinstake) and the second or third output
    if(pblockindex->IsProofOfStake())
    {
        if(stakeAddress == uint160())
        {
            LogPrint(BCLog::COINSTAKE, "Fail to solve script for mpos reward recipient\n");
            //This should never fail, but in case it somehow did we don't want it to bring the network to a halt
            //So, use an OP_RETURN script to burn the coins for the unknown staker
            script = CScript() << OP_RETURN;
        }else{
            // Make public key hash script
            script = CScript() << OP_DUP << OP_HASH160 << ToByteVector(stakeAddress) << OP_EQUALVERIFY << OP_CHECKSIG;
        }

        // Add the script into the list
        mposScriptList.push_back(script);

        // Update script cache
        AddToScriptCache(script, pblockindex, nHeight, consensusParams);
    }
    else
    {
        if(Params().MineBlocksOnDemand()){
            //this could happen in regtest. Just ignore and add an empty script
            script = CScript() << OP_RETURN;
            mposScriptList.push_back(script);
            return true;

        }
        LogPrint(BCLog::COINSTAKE, "The block is not proof-of-stake\n");
        return false;
    }

    return true;
}

bool GetMPoSOutputScripts(std::vector<CScript>& mposScriptList, int nHeight, const Consensus::Params& consensusParams)
{
    bool ret = true;
    nHeight -= COINBASE_MATURITY;

    // Populate the list of scripts for the reward recipients
    for(int i = 0; (i < consensusParams.nMPoSRewardRecipients - 1) && ret; i++)
    {
        ret &= AddMPoSScript(mposScriptList, nHeight - i, consensusParams);
    }

    return ret;
}

bool CreateMPoSOutputs(CMutableTransaction& txNew, int64_t nRewardPiece, int nHeight, const Consensus::Params& consensusParams)
{
    std::vector<CScript> mposScriptList;
    if(!GetMPoSOutputScripts(mposScriptList, nHeight, consensusParams))
    {
        LogPrint(BCLog::COINSTAKE, "Fail to get the list of recipients\n");
        return false;
    }

    // Split the block reward with the recipients
    for(unsigned int i = 0; i < mposScriptList.size(); i++)
    {
        CTxOut txOut(CTxOut(0, mposScriptList[i]));
        txOut.nValue = nRewardPiece;
        txNew.vout.push_back(txOut);
    }

    return true;
}

