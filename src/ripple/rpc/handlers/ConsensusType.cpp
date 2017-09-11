#include <BeastConfig.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/JsonFields.h>
#include <ripple/rpc/Context.h>
#include <ripple/app/ledger/Consensus.h>

namespace ripple {

Json::Value doConsensusType (RPC::Context& context)
{
    auto p = context.params[jss::type].asString ();

    if (p == "Ripple")
    {
        Consensus::setConsensusType(Consensus::Ripple);
    }
    else if (p == "ZooKeeper")
    {
        Consensus::setConsensusType(Consensus::ZooKeeper);
    }
    else
        return rpcError (rpcINVALID_PARAMS);

    return RPC::makeObjectValue ("Consensus type set to " + p);
}

} // ripple
