#ifndef RIPPLE_APP_DIVIDEND_MASTER_H_INCLUDED
#define RIPPLE_APP_DIVIDEND_MASTER_H_INCLUDED

#include <ripple/app/ledger/Ledger.h>
#include <ripple/shamap/SHAMap.h>

namespace ripple {

class DividendMaster
{
public:
    typedef enum
    {
        DivType_Done = 0,   /// Deprecated, do not use.
        DivType_Start = 1,
        DivType_Apply = 2
    } DivdendType;
    typedef enum { DivState_Done = 0, DivState_Start = 1 } DivdendState;
    
    // <AccountID, DivCoins, DivCoinsXRS, DivCoinsXRSRank, DivCoinsXRSSpd, VRank, VSpd, TSpd>
    typedef std::map<AccountID, std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, uint64_t, uint64_t>> AccountsDividend;
    
    // quantum dividend data set
    // <Balance, DivCoins, Activity, Energy, Links>
    typedef std::map<AccountID, std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>> QuantumDividend;
    
    virtual ~DividendMaster(){}
    virtual int getDividendState() = 0;
    virtual void setDividendState(int) = 0;
    
    //virtual bool calcDividend (const uint32_t ledgerIndex) = 0;
    virtual bool calcQuantumDividend (const uint32_t ledgerIndex) = 0;
    virtual bool dumpQuantumDividend (const uint32_t ledgerIndex) = 0;

    //virtual std::pair<bool, Json::Value> checkDividend (const uint32_t ledgerIndex, const std::string hash) = 0;
    virtual bool launchDividend (const uint32_t ledgerIndex) = 0;
    virtual void dividendProgress () = 0;
};

std::unique_ptr<DividendMaster>
make_DividendMaster(Application& app, beast::Journal journal);

}

#endif //RIPPLE_APP_DIVIDEND_MASTER_H_INCLUDED
