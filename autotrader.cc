#include <array>
#include <iostream>
#include <boost/asio/io_context.hpp>
#include <boost/circular_buffer.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 40;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int WINDOW = 5;
constexpr int HEDGE_THRESHOLD  = 200;

boost::circular_buffer<unsigned long> mETFSpreads(WINDOW);
double sum {0};
unsigned long newBidPrice {0};
unsigned long newAskPrice {0};
unsigned int askVolume {0};
unsigned int bidVolume {0};

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{

}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
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
    if (instrument == Instrument::ETF){

        // KEEP BEST BID AND BEST ASK
        mETFBestBid = bidPrices[0];
        mETFBestAsk = askPrices[0];
        // CALCULATE THE ROLLING AVERAGE AND STD OF THE ETF'S SPREAD
        if (askPrices[0] != 0 and bidPrices[0] != 0){
            mETFSpreads.push_back(askPrices[0] - bidPrices[0]);
        }
        if (mETFSpreads.size() > WINDOW){
            mETFSpreads.pop_front();
        }

        sum = 0;
        for (const auto& value : mETFSpreads) {
                sum += value;
            }
        mETFSpreadmean = sum / WINDOW;
        sum = 0;
        for (const auto& value: mETFSpreads){
                sum += (mETFSpreadmean - value) * (mETFSpreadmean - value);
        }
        mETFSpreadstd = std::sqrt(sum / WINDOW);
    }
    
    if (instrument == Instrument::FUTURE)
    { 
        // HEDGE COUNTER
        mHedgeCounter += 1;

        if (mHedgeCounter == HEDGE_THRESHOLD){
            mHedge = true;
        }

        // SET THE BID AND ASK PRICES
        newBidPrice = 0;
        newAskPrice = 0;

        //
        askVolume = std::round(LOT_SIZE * (1 + (double)mPosition / POSITION_LIMIT));
        bidVolume = std::round(LOT_SIZE * (1 - (double)mPosition / POSITION_LIMIT));

        if (mPosition < 80 && mPosition > -80){
            /////////////////////////////////
            if (mETFSpreads.size() == WINDOW){
                ////////////////////////
                int tickSpread = std::round(mETFSpreadstd / TICK_SIZE_IN_CENTS);
                if (tickSpread != 0){
                    newBidPrice = (bidPrices[0] != 0) ? bidPrices[0] - std::round(mETFSpreadstd / TICK_SIZE_IN_CENTS) * TICK_SIZE_IN_CENTS : 0;
                    newAskPrice = (askPrices[0] != 0) ? askPrices[0] + std::round(mETFSpreadstd / TICK_SIZE_IN_CENTS) * TICK_SIZE_IN_CENTS : 0;
                }
                ////////////////////////
                else{
                    // CALCULATE ASK AND BID PRICE
                    newAskPrice = (askPrices[0] != 0) ? askPrices[0] + std::round(mETFSpreadmean  / (2 * TICK_SIZE_IN_CENTS)) * TICK_SIZE_IN_CENTS : 0;
                    newBidPrice = (bidPrices[0] != 0) ? bidPrices[0] - std::round(mETFSpreadmean  / (2 * TICK_SIZE_IN_CENTS)) * TICK_SIZE_IN_CENTS : 0;
                }
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
            newAskPrice = (mETFBestAsk > askPrices[0] + TICK_SIZE_IN_CENTS) ? mETFBestAsk - TICK_SIZE_IN_CENTS : askPrices[0];
        }
        //////////////////////////////////
        else if (mPosition <= -80){
            // UNDERCUT
            newBidPrice = (mETFBestBid < bidPrices[0] - TICK_SIZE_IN_CENTS) ? mETFBestBid + TICK_SIZE_IN_CENTS : bidPrices[0];
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

}
