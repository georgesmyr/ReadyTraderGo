# Copyright 2021 Optiver Asia Pacific Pty. Ltd.
#
# This file is part of Ready Trader Go.
#
#     Ready Trader Go is free software: you can redistribute it and/or
#     modify it under the terms of the GNU Affero General Public License
#     as published by the Free Software Foundation, either version 3 of
#     the License, or (at your option) any later version.
#
#     Ready Trader Go is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU Affero General Public License for more details.
#
#     You should have received a copy of the GNU Affero General Public
#     License along with Ready Trader Go.  If not, see
#     <https://www.gnu.org/licenses/>.
import asyncio
import itertools
import numpy as np

from typing import List

from ready_trader_go import BaseAutoTrader, Instrument, Lifespan, MAXIMUM_ASK, MINIMUM_BID, Side


LOT_SIZE = 20
POSITION_LIMIT = 100
TICK_SIZE_IN_CENTS = 100
MIN_BID_NEAREST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
MAX_ASK_NEAREST_TICK = MAXIMUM_ASK // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
TAKER_FEES = 0.0002
WINDOW = 20


class AutoTrader(BaseAutoTrader):
    """Example Auto-trader.

    When it starts this auto-trader places ten-lot bid and ask orders at the
    current best-bid and best-ask prices respectively. Thereafter, if it has
    a long position (it has bought more lots than it has sold) it reduces its
    bid and ask prices. Conversely, if it has a short position (it has sold
    more lots than it has bought) then it increases its bid and ask prices.
    """

    def __init__(self, loop: asyncio.AbstractEventLoop, team_name: str, secret: str):
        """Initialise a new instance of the AutoTrader class."""
        super().__init__(loop, team_name, secret)
        self.order_ids = itertools.count(1)
        self.bids = set()
        self.asks = set()
        self.ask_id = self.ask_price = self.bid_id = self.bid_price = 0
        self.etf_position = self.future_position = 0

        self.future_bids = set()
        self.future_asks = set()

        self.etf_best_bid = self.etf_best_ask = self.future_best_bid = self.future_best_ask = 0
        self.etf_best_bid_window = self.etf_best_ask_window = self.future_best_ask_window = self.future_best_bid_window = []
        self.etf_spreads_window = []
        self.etf_spreads_std = self.arbitrage_margin = 0

    def on_error_message(self, client_order_id: int, error_message: bytes) -> None:
        """Called when the exchange detects an error.

        If the error pertains to a particular order, then the client_order_id
        will identify that order, otherwise the client_order_id will be zero.
        """
        self.logger.warning("error with order %d: %s", client_order_id, error_message.decode())
        if client_order_id != 0 and (client_order_id in self.bids or client_order_id in self.asks):
            self.on_order_status_message(client_order_id, 0, 0, 0)

    def on_hedge_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your hedge orders is filled.

        The price is the average price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        if client_order_id in self.future_bids:
            self.future_position += volume
        elif client_order_id in self.future_asks:
            self.future_position -= volume

    def on_order_book_update_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                                     ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically to report the status of an order book.

        The sequence number can be used to detect missed or out-of-order
        messages. The five best available ask (i.e. sell) and bid (i.e. buy)
        prices are reported along with the volume available at each of those
        price levels.
        """

        outstanding_bid = True if self.bid_id != 0 else False
        outstanding_ask = True if self.ask_id != 0 else False
        self.logger.info(f"ORDERBOOK:{instrument}:{sequence_number}:{bid_prices}:{bid_volumes}:{ask_prices}:{ask_volumes}")

        if instrument == Instrument.ETF:

            # COLLECTING ETF DATA
            if ask_prices[0] != 0 and bid_prices[0] != 0:
                self.etf_best_ask_window.append(ask_prices[0])
                self.etf_best_bid_window.append(bid_prices[0])
                self.etf_best_ask_window = self.etf_best_ask_window[-WINDOW:]
                self.etf_best_bid_window = self.etf_best_bid_window[-WINDOW:]
                self.etf_spreads_window = np.array(self.etf_best_ask_window)-np.array(self.etf_best_bid_window)

            if len(self.etf_spreads_window) == WINDOW:
                self.etf_spreads_std = np.std(self.etf_spreads_window) / TICK_SIZE_IN_CENTS
                self.arbitrage_margin = (round(2 * self.etf_spreads_std) + 2) * TICK_SIZE_IN_CENTS
            else:
                self.arbitrage_margin = 3 * TICK_SIZE_IN_CENTS

            self.etf_best_bid, self.etf_best_ask = bid_prices[0], ask_prices[0]

            # SELL ETF BUY FUTURE
            if self.etf_best_bid > self.future_best_ask + self.arbitrage_margin and not outstanding_ask:
                ask_volume = round(LOT_SIZE*(1+self.etf_position/100))
                if self.etf_position - ask_volume >= - POSITION_LIMIT and self.future_position + ask_volume < POSITION_LIMIT:
                    if self.etf_best_bid not in (self.ask_price, 0):
                        self.ask_id = next(self.order_ids)
                        self.logger.info(
                            f"ARBITRAGE:ETF_BB:{self.etf_best_bid}:FUTURE_BA:{self.future_best_ask}:ETF_POS:{self.etf_position}:FUTURE_POS:{self.future_position}")
                        self.send_insert_order(self.ask_id, Side.SELL, self.etf_best_bid, ask_volume, Lifespan.FILL_AND_KILL)
                        self.asks.add(self.ask_id)
                        self.ask_id = 0

            # BUY ETF SELL FUTURE
            elif self.future_best_bid > self.etf_best_ask + self.arbitrage_margin and not outstanding_bid:
                bid_volume = round(LOT_SIZE*(1-self.etf_position/100))
                if self.etf_position + bid_volume <= POSITION_LIMIT and self.future_position - bid_volume < POSITION_LIMIT:
                    if self.etf_best_ask not in (self.bid_price, 0):
                        self.bid_id = next(self.order_ids)
                        self.logger.info(
                            f"ARBITRAGE:FUTURE_BB:{self.future_best_bid}:ETF_BA:{self.etf_best_ask}:ETF_POS:{self.etf_position}:FUTURE_POS:{self.future_position}")
                        self.send_insert_order(self.bid_id, Side.BUY, self.etf_best_ask, bid_volume, Lifespan.FILL_AND_KILL)
                        self.bids.add(self.bid_id)
                        self.bid_id = 0

        if instrument == Instrument.FUTURE:

            # COLLECTING FUTURE DATA
            if ask_prices[0] != 0 and bid_prices[0] != 0:
                self.future_best_ask_window.append(ask_prices[0])
                self.future_best_bid_window.append(bid_prices[0])
                self.future_best_ask_window = self.future_best_ask_window[-WINDOW:]
                self.future_best_bid_window = self.future_best_bid_window[-WINDOW:]

            self.future_best_bid, self.future_best_ask = bid_prices[0], ask_prices[0]

            # DETERMIN PRICE FOR MARKET MAKING
            if len(self.etf_best_ask_window) == len(self.etf_best_bid_window) == WINDOW:
                new_bid_price = bid_prices[0] - round(self.etf_spreads_std) * TICK_SIZE_IN_CENTS if bid_prices[
                                                                                                        0] != 0 else 0
                new_ask_price = ask_prices[0] + round(self.etf_spreads_std) * TICK_SIZE_IN_CENTS if ask_prices[
                                                                                                        0] != 0 else 0
            else:
                new_bid_price = bid_prices[0] - 3 * TICK_SIZE_IN_CENTS if bid_prices[0] != 0 else 0
                new_ask_price = ask_prices[0] + 1 * TICK_SIZE_IN_CENTS if ask_prices[0] != 0 else 0

            bid_volume = round(LOT_SIZE * (1 - self.etf_position / 100))
            ask_volume = round(LOT_SIZE * (1 + self.etf_position / 100))

            if self.etf_position > 90 and self.etf_best_ask - TICK_SIZE_IN_CENTS >= self.future_best_ask:
                new_ask_price = self.etf_best_ask - TICK_SIZE_IN_CENTS
                ask_volume = 100
            if self.etf_position < -90 and self.etf_best_bid + TICK_SIZE_IN_CENTS <= self.future_best_bid:
                new_bid_price = self.etf_best_bid + TICK_SIZE_IN_CENTS
                bid_volume = 100

            # CANCEL OUTSTANDING ORDERS IF THE NEW PRICE IS NOT THE SAME
            if outstanding_bid and new_bid_price not in (self.bid_price, 0):
                self.send_cancel_order(self.bid_id)
                self.bid_id = 0
            if outstanding_ask and new_ask_price not in (self.ask_price, 0):
                self.send_cancel_order(self.ask_id)
                self.ask_id = 0

            if self.bid_id == 0 and new_bid_price != 0 and self.etf_position + bid_volume <= POSITION_LIMIT and new_bid_price not in (self.bid_price, 0):
                self.bid_id = next(self.order_ids)
                self.bid_price = new_bid_price
                self.logger.info(
                    f'MARKET_MAKING:ID:{self.bid_id}:ETF_POS:{self.etf_position}:FUTURE_POS:{self.future_position}')
                self.send_insert_order(self.bid_id, Side.BUY, new_bid_price, bid_volume, Lifespan.GOOD_FOR_DAY)
                self.bids.add(self.bid_id)

            if self.ask_id == 0 and new_ask_price != 0 and self.etf_position - ask_volume >= -POSITION_LIMIT and new_ask_price not in (
            self.ask_price, 0):
                self.ask_id = next(self.order_ids)
                self.ask_price = new_ask_price
                self.logger.info(
                    f'MARKET_MAKING:ID:{self.ask_id}:ETF_POS:{self.etf_position}:FUTURE_POS:{self.future_position}')
                self.send_insert_order(self.ask_id, Side.SELL, new_ask_price, ask_volume, Lifespan.GOOD_FOR_DAY)
                self.asks.add(self.ask_id)

    def on_order_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your orders is filled, partially or fully.

        The price is the price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        if client_order_id in self.bids:
            self.etf_position += volume
            self.logger.info(f"ORDERED_FILLED:ID:{client_order_id}NEW_ETF_POS:{self.etf_position}")
            new_hedge_order_id = next(self.order_ids)
            self.send_hedge_order(new_hedge_order_id, Side.ASK, MIN_BID_NEAREST_TICK, volume)
            self.future_asks.add(new_hedge_order_id)
        elif client_order_id in self.asks:
            self.etf_position -= volume
            self.logger.info(f"ORDERED_FILLED:ID:{client_order_id}NEW_ETF_POS:{self.etf_position}")
            new_hedge_order_id = next(self.order_ids)
            self.send_hedge_order(new_hedge_order_id, Side.BID, MAX_ASK_NEAREST_TICK, volume)
            self.future_bids.add(new_hedge_order_id)

    def on_order_status_message(self, client_order_id: int, fill_volume: int, remaining_volume: int,
                                fees: int) -> None:
        """Called when the status of one of your orders changes.

        The fill_volume is the number of lots already traded, remaining_volume
        is the number of lots yet to be traded and fees is the total fees for
        this order. Remember that you pay fees for being a market taker, but
        you receive fees for being a market maker, so fees can be negative.

        If an order is cancelled its remaining volume will be zero.
        """
        self.logger.info("received order status for order %d with fill volume %d remaining %d and fees %d",
                         client_order_id, fill_volume, remaining_volume, fees)
        if remaining_volume == 0:
            if client_order_id == self.bid_id:
                self.bid_id = 0
            elif client_order_id == self.ask_id:
                self.ask_id = 0

            # It could be either a bid or an ask
            self.bids.discard(client_order_id)
            self.asks.discard(client_order_id)

    def on_trade_ticks_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                               ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically when there is trading activity on the market.

        The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
        has been trading activity are reported along with the aggregated volume
        traded at each of those price levels.

        If there are less than five prices on a side, then zeros will appear at
        the end of both the prices and volumes arrays.
        """
        # self.logger.info("received trade ticks for instrument %d with sequence number %d", instrument,sequence_number)
