#include <array>
#include <iostream>
#include <boost/asio/io_context.hpp>
#include <boost/circular_buffer.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader4.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 40;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int WINDOW = 5;
constexpr int HEDGE_THRESHOLD  = 40;

boost::circular_buffer<unsigned long> mETFSpreads(WINDOW);
boost::circular_buffer<unsigned long> mLastBids(WINDOW);
boost::circular_buffer<unsigned long> mLastAsks(WINDOW);


AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{

}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    // RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    if (mFutureAsks.count(clientOrderId) == 1)
    {
        mFuturePosition -= (long)volume;
    }
        else if (mFutureBids.count(clientOrderId) == 1)
    {
        mFuturePosition += (long)volume;
    }

}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    // RLOG(LG_AT, LogLevel::LL_INFO) << "New Order";

    if (instrument == Instrument::ETF){

        // CALCULATE THE ROLLING AVERAGE AND STD OF THE ETF'S SPREAD
        if (askPrices[0] != 0 and bidPrices[0] != 0){
            mLastBids.push_back(bidPrices[0]);
            mLastAsks.push_back(askPrices[0]);
        }

        if (mLastBids.size() > WINDOW){
            mLastBids.pop_front();
            mLastAsks.pop_front();
        }

        double sumB {0}, sumA {0};
        for (const auto& value : mLastAsks) {
            sumA += value;
        }
        for (const auto& value : mLastBids) {
            sumB += value;
         }
        double AsksMean = sumA / WINDOW;
        double BidsMean = sumB / WINDOW;
        sumA = 0;
        sumB = 0;
        for (const auto& value: mLastAsks){
                sumA += (AsksMean - value) * (AsksMean - value);
        }
        double AsksSTD = std::sqrt(sumA / WINDOW);

        for (const auto& value: mLastBids){
            sumB += (BidsMean - value) * (BidsMean - value);
        }
        double BidsSTD = std::sqrt(sumB / WINDOW);

        // HEDGE COUNTER
        mHedgeCounter += 1;

        if (mHedgeCounter == HEDGE_THRESHOLD){
            mHedge = true;
        }

        // SET THE BID AND ASK PRICES
        unsigned long newBidPrice {0};
        unsigned long newAskPrice {0};

        //
        unsigned int askVolume = std::round(LOT_SIZE * (1 + (double)mPosition / POSITION_LIMIT));
        unsigned int bidVolume = std::round(LOT_SIZE * (1 - (double)mPosition / POSITION_LIMIT));

        if (mPosition < 80 && mPosition > -80){
            /////////////////////////////////
            if (mETFSpreads.size() == WINDOW){
                ////////////////////////
                newBidPrice = (bidPrices[0] != 0) ? BidsMean - std::floor(BidsSTD / TICK_SIZE_IN_CENTS) * TICK_SIZE_IN_CENTS : 0;
                newAskPrice = (askPrices[0] != 0) ? AsksMean + std::floor(AsksSTD / TICK_SIZE_IN_CENTS) * TICK_SIZE_IN_CENTS : 0;
            }
            /////////////////////////////////
            else{
                newBidPrice = bidPrices[0] - 3 * TICK_SIZE_IN_CENTS;
                newAskPrice = askPrices[0] + 3 * TICK_SIZE_IN_CENTS;
            }
        }
        //////////////////////////////////
        else if (mPosition >= 80){
            // UNDERCUT BEST ASK
            newAskPrice = (askPrices[0] - bidPrices[0] > TICK_SIZE_IN_CENTS) ? askPrices[0] - TICK_SIZE_IN_CENTS : askPrices[0];
        }
        //////////////////////////////////
        else if (mPosition <= -80){
            // UNDERCUT
            newBidPrice = (askPrices[0] - bidPrices[0] > TICK_SIZE_IN_CENTS) ? bidPrices[0] + TICK_SIZE_IN_CENTS : bidPrices[0];
        }



        // CANCEL ORDERS IF THE PRICE BUDGES BY AT LEAST TWO PRICE TICKS
        if (mBidId != 0 && newBidPrice != mBidPrice){
            SendCancelOrder(mBidId);
            mBidId = 0;
        }
        if (mAskId != 0 and newAskPrice != mAskPrice){
            SendCancelOrder(mAskId);
            mAskId = 0;
        }

        // SEND ASK LIMIT ORDER PRICE
        if (mAskId == 0 && newAskPrice != 0 && mPosition - askVolume > - POSITION_LIMIT){
            mAskId = mNextMessageId++;
            mAskPrice = newAskPrice;
            SendInsertOrder(mAskId, Side::SELL, newAskPrice, askVolume, Lifespan::GOOD_FOR_DAY);
            mAsks.emplace(mAskId);
        }   
        // SEND BID LIMIT ORDER PRICE
        if (mBidId == 0 && newBidPrice != 0 && mPosition + bidVolume < POSITION_LIMIT){
            mBidId = mNextMessageId++;
            mBidPrice = newBidPrice;
            SendInsertOrder(mBidId, Side::BUY, newBidPrice, bidVolume, Lifespan::GOOD_FOR_DAY);
            mBids.emplace(mBidId);
        }
    }
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
    //                                << " lots at $" << price << " cents";
    if (mAsks.count(clientOrderId) == 1)
    {
        mPosition -= (long)volume;
    }
        else if (mBids.count(clientOrderId) == 1)
    {
        mPosition += (long)volume;
    }

    // RESET THE HEDGE COUNTER IF WE ARE WITHIN HEDGE LIMIT
    if (mPosition + mFuturePosition > -10 and mPosition + mFuturePosition < 10 ){
        mHedgeCounter = 0;
    }

    // HEDGE IF WE GET THE HEDGE SIGNAL
    if (mHedge && mPosition + mFuturePosition < 0){
        mFutureBidId = mNextMessageId++;
        SendHedgeOrder(mFutureBidId, Side::BUY, MAX_ASK_NEAREST_TICK, std::abs(mFuturePosition + mPosition));
        mFutureBids.emplace(mFutureBidId);
        mHedgeCounter = 0;
        mHedge = false;
    }
    else if (mHedge && mPosition + mFuturePosition > 0){
        mFutureAskId = mNextMessageId++;
        SendHedgeOrder(mFutureAskId, Side::SELL, MIN_BID_NEARST_TICK, std::abs(mFuturePosition + mPosition));
        mFutureAsks.emplace(mFutureAskId);
        mHedgeCounter = 0;
        mHedge = false;
    }
    
    
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    if (remainingVolume == 0)
    {
        if (clientOrderId == mAskId)
        {
            mAskId = 0;
        }
        else if (clientOrderId == mBidId)
        {
            mBidId = 0;
        }

        mAsks.erase(clientOrderId);
        mBids.erase(clientOrderId);
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    // RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
    //                                << ": ask prices: " << askPrices[0]
    //                                << "; ask volumes: " << askVolumes[0]
    //                                << "; bid prices: " << bidPrices[0]
    //                                << "; bid volumes: " << bidVolumes[0];
}
