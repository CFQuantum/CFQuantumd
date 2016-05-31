#include <BeastConfig.h>
#include <ripple/app/tx/impl/Dividend.h>
#include <ripple/app/misc/DividendMaster.h>
#include <ripple/basics/Log.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

TER
Dividend::preflight (PreflightContext const& ctx)
{
    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != zero)
    {
        JLOG(ctx.j.warning) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount (sfFee);
    if (!fee.native () || fee != beast::zero)
    {
        JLOG(ctx.j.warning) << "Non-zero fee";
        return temBAD_FEE;
    }

    // check if signing public key is trusted.
    auto const& dividendAccount = ctx.app.config ()[SECTION_QUANTUM];
    std::string public_key = get<std::string> (dividendAccount, "public_key");
    if (public_key.empty())
    {
        JLOG(ctx.j.warning) << "public_key is not configured in dividend_account to check dividend transaction";
        return tefBAD_AUTH;
    }
    auto const accountPublic = parseBase58<AccountID> (public_key);
    if (!accountPublic ||
        calcAccountID (RippleAddress::createAccountPublic (ctx.tx.getSigningPubKey ())) != accountPublic)
    {
        JLOG(ctx.j.warning) << "apply: Invalid transaction (bad signature)";
        return temBAD_SIGNATURE;
    }

    if ((ctx.tx.getSequence () != 0) || ctx.tx.isFieldPresent (sfPreviousTxnID))
    {
        JLOG(ctx.j.warning) << "Bad sequence";
        return temBAD_SEQUENCE;
    }

    return tesSUCCESS;
}

TER Dividend::preclaim (PreclaimContext const &ctx)
{
    if (ctx.view.txExists(ctx.tx.getTransactionID ()))
        return tefALREADY;

    return tesSUCCESS;
}

void Dividend::preCompute ()
{
    account_ = ctx_.tx.getAccountID(sfAccount);
    assert(account_ == zero);
}

TER Dividend::startCalc ()
{
    auto const k = keylet::dividend();
    
    SLE::pointer dividendObject = view().peek (k);

    if (!dividendObject)
    {
        dividendObject = std::make_shared<SLE>(k);
        view().insert(dividendObject);
    }

    auto& tx=ctx_.tx;
    
    JLOG(j_.info) << "Previous dividend object: " << dividendObject->getText ();

    uint32_t dividendLedger = tx.getFieldU32 (sfDividendLedger);

    dividendObject->setFieldU8 (sfDividendState, DividendMaster::DivState_Start);
    dividendObject->setFieldU32 (sfDividendLedger, dividendLedger);
    dividendObject->setFieldU64 (sfQuantumCoins, tx.getFieldU64 (sfQuantumCoins));
    dividendObject->setFieldU64 (sfQuantumAccounts, tx.getFieldU64 (sfQuantumAccounts));
    dividendObject->setFieldU64 (sfQuantumEnergy, tx.getFieldU64 (sfQuantumEnergy));
    dividendObject->setAccountID (sfDividendMarker, AccountID ());
    view ().update (dividendObject);
    
    auto& dm = ctx_.app.getDividendMaster ();
    dm.setDividendState (DividendMaster::DivType_Start);

    JLOG(j_.info) << "Current dividend object: " << dividendObject->getText ();

    return tesSUCCESS;
}

//apply dividend result here
TER Dividend::applyTx ()
{
    auto& tx=ctx_.tx;

    const auto& account = tx.getAccountID (sfDestination);
    uint64_t divCoins = tx.getFieldU64 (sfQuantumCoins);
    uint32_t dividendLedger = tx.getFieldU32 (sfDividendLedger);

    auto sleAccountModified = view ().peek (keylet::account (account));

    if (sleAccountModified)
    {
        if (divCoins > 0)
        {
            sleAccountModified->setFieldAmount (sfBalance,
                sleAccountModified->getFieldAmount (sfBalance) + divCoins);
            ctx_.createXRP (divCoins);
        }
        if (dividendLedger > 0)
            sleAccountModified->setFieldU32(sfDividendLedger, dividendLedger);
        if (tx.isFieldPresent (sfDividendMarker))
            sleAccountModified->setAccountID (sfDividendMarker, tx.getAccountID (sfDividendMarker));
        if (tx.isFieldPresent (sfQuantumEnergy))
            sleAccountModified->setFieldU64 (sfQuantumEnergy, tx.getFieldU64 (sfQuantumEnergy));
        if (tx.isFieldPresent (sfQuantumActivity))
            sleAccountModified->setFieldU64 (sfQuantumActivity, tx.getFieldU64 (sfQuantumActivity));
        view ().update(sleAccountModified);
        JLOG(j_.debug) << "Dividend Applied:" << sleAccountModified->getText ();
    }
    else
    {
        JLOG(j_.warning) << "Dividend account not found :" << account;
        return tefBAD_LEDGER;
    }
    return tesSUCCESS;
}

TER Dividend::doApply ()
{
    if (ctx_.tx.getTxnType () == ttISSUE)
    {
        uint8_t divOpType = ctx_.tx.isFieldPresent (sfDividendType) ? ctx_.tx.getFieldU8 (sfDividendType) : DividendMaster::DivType_Start;
        switch (divOpType)
        {
        case DividendMaster::DivType_Start:
        {
            return startCalc ();
        }
        case DividendMaster::DivType_Apply:
        {
            return applyTx ();
        }
        }
    }
    return temUNKNOWN;
}
}
