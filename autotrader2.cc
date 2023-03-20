// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader2.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 20;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int HEDGE_THRESHOLD = 200;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";

    if (mFutureAsks.count(clientOrderId) == 1){
        mFuturePosition -= (long)volume;
    }
        else if (mFutureBids.count(clientOrderId) == 1){
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

    if (instrument == Instrument::ETF){
        
        mETFBestAsk = askPrices[0];
        mETFBestBid = bidPrices[0];
    }
    
    if (instrument == Instrument::FUTURE)
    { 
        // HEDGE COUNTER
        mHedgeCounter += 1;
        if (mHedgeCounter == HEDGE_THRESHOLD){
            mHedge = true;
        }
        
        // ASK
        unsigned long newAskPrice = 0;
        if (mPosition < 80 or mETFBestAsk - mETFBestBid < 2 * TICK_SIZE_IN_CENTS){
            // SET ASK ORDER MARGIN AS MULTIPLE OF TICK SIZE DEPENDING ON CURRENT POSITION
            unsigned int ask_multiplier {3};
            switch(mPosition){
                case -99 ... -80:
                    ask_multiplier = 5;
                    break;
                case -79 ... -50:
                    ask_multiplier = 4;
                    break;
                case 50 ... 79:
                    ask_multiplier = 2;
                    break;
            }
                // CALCULATE ASK PRICE
                newAskPrice = (askPrices[0] != 0) ? askPrices[0] + ask_multiplier * TICK_SIZE_IN_CENTS : 0;
         }
        else{ // UNDERCUT THE BEST ASK
             newAskPrice = mETFBestAsk - TICK_SIZE_IN_CENTS;
        }
        
        // BID
        unsigned long newBidPrice = 0;
        if (mPosition > -80 or mETFBestAsk - mETFBestBid < 2 * TICK_SIZE_IN_CENTS){
            // SET BID ORDER MARGIN AS MULTIPLE OF TICK SIZE DEPENDING ON CURRENT POSITION
            unsigned int bid_multiplier {3};
            switch (mPosition){
                case -79 ... -50:
                    bid_multiplier = 2;
                    break;
                case 50 ... 79:
                    bid_multiplier = 4;
                    break;
                case 80 ... 99:
                    bid_multiplier = 5;
                    break;
            }
            
            // CALCULATE BID LIMIT ORDER PRICE
            newBidPrice = (askPrices[0] != 0) ? askPrices[0] - bid_multiplier * TICK_SIZE_IN_CENTS : 0;
            
        }
        else{ // UNDERCUT THE BEST BID
            newBidPrice = mETFBestBid + TICK_SIZE_IN_CENTS;
        }

        // CANCEL ORDERS IF THE PRICE BUDGES BY AT LEAST TWO PRICE TICKS
        if (mBidId != 0 && bidPrices[0] != mBidPrice){
            SendCancelOrder(mBidId);
            mBidId = 0;
        }
        if (mAskId != 0 && askPrices[0] != mAskPrice){
            SendCancelOrder(mAskId);
            mAskId = 0;
        }

        // POSITION ADJUSTED VOLUME
        unsigned int askVolume = std::round(LOT_SIZE * (1 + (double)mPosition / POSITION_LIMIT));
        unsigned int bidVolume = std::round(LOT_SIZE * (1 - (double)mPosition / POSITION_LIMIT));

        // SEND ASK LIMIT ORDER PRICE
        if (mAskId == 0 && newAskPrice != 0 && mPosition - askVolume >= -100){
            mAskId = mNextMessageId++;
            mAskPrice = newAskPrice;
            SendInsertOrder(mAskId, Side::SELL, newAskPrice, askVolume, Lifespan::GOOD_FOR_DAY);
            mAsks.emplace(mAskId);
        }
        // SEND BID LIMIT ORDER PRICE
        if (mBidId == 0 && newBidPrice != 0 && mPosition + bidVolume <= 100){
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
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";
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
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];
}
