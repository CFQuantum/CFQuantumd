//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/app/tx/impl/Transfer.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/JsonFields.h>
#include <ripple/ledger/View.h>

namespace ripple {

TER
Transfer::preflight (PreflightContext const& ctx)
{
    auto const ret = preflight1 (ctx);
    if (!isTesSuccess (ret))
        return ret;

    auto& tx = ctx.tx;
    auto& j = ctx.j;


    STAmount const saDstAmount (tx.getFieldAmount (sfAmount));
    
    STAmount maxSourceAmount;
    auto const account = tx.getAccountID(sfAccount);
    
    if (saDstAmount.native ())
        return temBAD_CURRENCY;
    else
        maxSourceAmount = STAmount (
            { saDstAmount.getCurrency (), account },
            saDstAmount.mantissa(), saDstAmount.exponent (),
            saDstAmount < zero);

    auto const& uSrcCurrency = maxSourceAmount.getCurrency ();
    auto const& uDstCurrency = saDstAmount.getCurrency ();

    if (!isLegalNet (saDstAmount))
        return temBAD_AMOUNT;

    auto const uDstAccountID = tx.getAccountID (sfDestination);

    if (!uDstAccountID)
    {
        JLOG(j.trace) << "Malformed transaction: " <<
            "Payment destination account not specified.";
        return temDST_NEEDED;
    }
    if (saDstAmount <= zero)
    {
        JLOG(j.trace) << "Malformed transaction: "<<
            "bad dst amount: " << saDstAmount.getFullText ();
        return temBAD_AMOUNT;
    }
    if (badCurrency() == uSrcCurrency || badCurrency() == uDstCurrency)
    {
        JLOG(j.trace) <<"Malformed transaction: " <<
            "Bad currency.";
        return temBAD_CURRENCY;
    }
    if (account == uDstAccountID && uSrcCurrency == uDstCurrency)
    {
        // You're signing yourself a payment.
        // If bPaths is true, you might be trying some arbitrage.
        JLOG(j.trace) << "Malformed transaction: " <<
            "Redundant payment from " << to_string (account) <<
            " to self without path for " << to_string (uDstCurrency);
        return temREDUNDANT;
    }

    // additional checking for currency ASSET.
    if (assetCurrency () == uDstCurrency)
    {
        if (saDstAmount.getIssuer () == uDstAccountID)
        {
            // Return Asset to issuer is not allowed.
            JLOG(j.trace) << "Return Asset to issuer is not allowed"
                     << " src=" << to_string (tx.getAccountID(sfAccount)) << " dst=" << to_string (uDstAccountID) << " src_cur=" << to_string (uSrcCurrency) << " dst_cur=" << to_string (uDstCurrency);

            return temBAD_ISSUER;
        }

        if (saDstAmount < STAmount (saDstAmount.issue (), ctx.app.config ().ASSET_TX_MIN) || !saDstAmount.isMathematicalInteger ())
            return temBAD_CURRENCY;
    }

    if (assetCurrency () == uSrcCurrency)
    {
        if (saDstAmount.getIssuer () == tx.getAccountID(sfAccount))
        {
            JLOG(j.trace) << "Asset payment from issuer is not allowed";
            return temBAD_ISSUER;
        }
    }

    return preflight2 (ctx);
}

TER
Transfer::preclaim(PreclaimContext const& ctx)
{
    AccountID const uDstAccountID(ctx.tx[sfDestination]);
    STAmount const saDstAmount(ctx.tx[sfAmount]);

    auto const k = keylet::account(uDstAccountID);
    auto const sleDst = ctx.view.read(k);

    if (!sleDst)
    {
        // Destination account does not exist.
        if (!saDstAmount.native())
        {
            JLOG(ctx.j.trace) <<
                "Delay transaction: Destination account does not exist.";

            // Another transaction could create the account and then this
            // transaction would succeed.
            return tecNO_DST;
        }
        else if (saDstAmount < STAmount(ctx.view.fees().accountReserve(0)))
        {
            // accountReserve is the minimum amount that an account can have.
            // Reserve is not scaled by load.
            JLOG(ctx.j.trace) <<
                "Delay transaction: Destination account does not exist. " <<
                "Insufficent payment to create account.";

            // TODO: dedupe
            // Another transaction could create the account and then this
            // transaction would succeed.
            return tecNO_DST_INSUF_XRP;
        }
    }
    else if ((sleDst->getFlags() & lsfRequireDestTag) &&
        !ctx.tx.isFieldPresent(sfDestinationTag))
    {
        // The tag is basically account-specific information we don't
        // understand, but we can require someone to fill it in.

        // We didn't make this test for a newly-formed account because there's
        // no way for this field to be set.
        JLOG(ctx.j.trace) << "Malformed transaction: DestinationTag required.";

        return tecDST_TAG_NEEDED;
    }

    return tesSUCCESS;
}

TER
Transfer::doApply ()
{
    AccountID const uDstAccountID (ctx_.tx.getAccountID (sfDestination));
    STAmount const saDstAmount (ctx_.tx.getFieldAmount (sfAmount));

    // only allowed transfer !native
    if (saDstAmount.native ())
        return temBAD_CURRENCY;
    
    // check balance
    auto const sleRippleState = view().peek (keylet::line (account_, saDstAmount.getIssuer(), saDstAmount.getCurrency ()));
    if (!sleRippleState)
        return temBAD_ISSUER;
    STAmount balance = sleRippleState->getFieldAmount (sfBalance);
    if (balance < saDstAmount)
    {
        return tecUNFUNDED_TRANSFER;
    }

    // Open a ledger for editing.
    auto const k = keylet::account(uDstAccountID);
    SLE::pointer sleDst = view().peek (k);

    if (!sleDst)
    {
        // Create the account.
        sleDst = std::make_shared<SLE>(k);
        sleDst->setAccountID(sfAccount, uDstAccountID);
        sleDst->setFieldU32(sfSequence, 1);
        view().insert(sleDst);
    }
    else
    {
        // Tell the engine that we are intending to change the destination
        // account.  The source account gets always charged a fee so it's always
        // marked as modified.
        view().update (sleDst);
    }

    auto const issuer   = saDstAmount.getIssuer ();
    TER terResult = tesSUCCESS;
    if (account_ == issuer || uDstAccountID == issuer || issuer == noAccount())
    {
        // Account <-> issuer
        terResult   = rippleCredit (view (), account_, uDstAccountID, saDstAmount, false, j_);
    }
    else
    {
        STAmount saActual = saDstAmount;
        STAmount saTransitFee = saActual = saDstAmount;
        JLOG (j_.debug) << "rippleSend> " <<
            to_string (account_) <<
            " - > " << to_string (uDstAccountID) <<
            " : deliver=" << saDstAmount.getFullText () <<
            " fee=" << saTransitFee.getFullText () <<
            " cost=" << saActual.getFullText ();

        if (tesSUCCESS == terResult)
            terResult   = rippleCredit (view (), issuer, uDstAccountID, saDstAmount, true, j_);

        if (tesSUCCESS == terResult)
            terResult   = rippleCredit (view (), account_, issuer, saActual, true, j_);
    }

    return terResult;
}

}  // ripple
