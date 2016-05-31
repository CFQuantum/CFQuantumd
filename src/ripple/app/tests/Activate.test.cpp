#include <BeastConfig.h>
#include <ripple/protocol/JsonFields.h>
#include <ripple/test/jtx.h>

namespace ripple
{
namespace test
{
struct Activate_test : public beast::unit_test::suite
{
    static Json::Value
    activate (jtx::Account const& account,
              jtx::Account const& dest,
              STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::Account] = account.human ();
        jv[jss::Destination] = dest.human ();
        jv[jss::Amount] = amount.getJson (0);
        jv[jss::TransactionType] = "Activate";
        return jv;
    }

    void testActivate ()
    {
        using namespace jtx;
        auto const gw = Account ("gw");
        auto const USD = gw["USD"];

        Env env (*this);
        env.fund (XRP (100000), "alice", gw);
        env (trust ("alice", USD (1000)));
        env (pay (gw, "alice", USD (100)));

        env (activate ("alice", "bob", XRP (100)));
        env (activate (gw, "bob", XRP (100)), ter (tefCREATED));
        
        auto sle = env.le ("bob");
        expect (sle &&
                sle->getFieldAmount (sfBalance) == XRP (100));

        Json::Value amounts;
        amounts[0u]["Entry"][jss::Amount] = STAmount (USD (100)).getJson (0);
        env (activate ("alice", "carol", XRP (100)),
             json (Json::StaticString ("Amounts"), amounts),
             ter (temBAD_CURRENCY));

        Json::Value limits;
        limits[0u]["Entry"][jss::LimitAmount] = STAmount (USD (100)).getJson (0);
        amounts[0u]["Entry"][jss::Amount] = STAmount (XRP (100)).getJson (0);
        env (activate ("alice", "carol", XRP (100)),
             json (Json::StaticString ("Amounts"), amounts),
             json (Json::StaticString ("Limits"), limits));
        sle = env.le ("carol");
        expect (sle &&
                sle->getFieldAmount (sfBalance) == XRP (200));
        expect (env.le (
            keylet::line (Account ("carol").id (),
                          gw.id (), USD.currency)));
    }

    void run () override
    {
        testActivate ();
    }
};

BEAST_DEFINE_TESTSUITE (Activate, test, ripple);

} // test
} // ripple
