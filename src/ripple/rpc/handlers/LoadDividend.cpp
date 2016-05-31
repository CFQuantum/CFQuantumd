#include <fstream>
#include <iostream>
#include <stdio.h>
#include <boost/algorithm/string.hpp>
#include <ripple/app/misc/DividendMaster.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

Json::Value doLoadDividend (RPC::Context &context)
{
    Json::Value jvResult;
    uint32_t ledgerIndex = context.params[jss::ledger_index].asUInt ();
    bool bSave = context.params["save"].asBool ();
    bool bLaunch = context.params["launch"].asBool ();
    
    // 0.check dividend state
    //ret = dm.checkDividend (ledgerIndex, hash);
    
    // 1. calc quantum dividend
    auto& dm = context.app.getDividendMaster ();
    if (!dm.calcQuantumDividend(ledgerIndex))
    {
        return RPC::make_error (rpcINTERNAL, "Failed to calculate quantum issue.");;
    }
    
    // 2.save dividend result into hbase
    if (bSave && !dm.dumpQuantumDividend (ledgerIndex))
        return RPC::make_error (rpcINTERNAL, "Failed to store dividend to hbase.");
    
    // 3.launch dividend
    if (bLaunch && !dm.launchDividend(ledgerIndex))
    {
        return RPC::make_error (rpcINTERNAL, "Failed to launch dividend");
    }
    
    return jvResult;
}

}// ripple