#ifndef RIPPLE_TX_ACTIVATE_H_INCLUDED
#define RIPPLE_TX_ACTIVATE_H_INCLUDED

#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/tx/impl/Payment.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Indexes.h>

namespace ripple {

class Activate
    : public Payment
{
public:
    Activate (ApplyContext& ctx)
        : Payment(ctx)
    {
    }

    static
    TER
    preflight (PreflightContext const& ctx);

    static
    TER
    preclaim(PreclaimContext const& ctx);

    TER doApply () override;
};

} // ripple

#endif
