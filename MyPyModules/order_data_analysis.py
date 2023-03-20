import pandas as pd
import matplotlib.pyplot as plt


def split_future_etf(df_md1):
    """Returns the orders for the futures contract and the ETF separately."""
    df_md1_split = df_md1.groupby('Instrument')
    df_md1_future = df_md1_split.get_group(0)
    df_md1_etf = df_md1_split.get_group(1)
    return df_md1_future, df_md1_etf


def split_bids_asks(df_md1):
    """Returns the bids and asks in separate DataFrames."""
    df_md1_split = df_md1.groupby('Side')
    df_md1_asks = df_md1_split.get_group('A')
    df_md1_bids = df_md1_split.get_group('B')
    return df_md1_bids, df_md1_asks


def split_canceled_non_canceled(df_md1):
    """Returns the canceled and non-canceled orders in separate DataFrames."""
    # Find all OrderIds that were canceled
    canceled_orders = df_md1[df_md1['Operation'] == 'Cancel']['OrderId'].unique()

    # Create dataframe for non-canceled orders
    df_non_canceled = df_md1[~df_md1['OrderId'].isin(canceled_orders)]

    # Create dataframe for canceled orders
    df_canceled = df_md1[df_md1['OrderId'].isin(canceled_orders)]
    canceled_percentage = len(df_non_canceled)/len(df_md1)
    return df_canceled, df_non_canceled, canceled_percentage


def get_best_bids_asks(df_bids, df_asks):

    # only keep the times where we have both asks and bids
    all_times = sorted(set.intersection(set(df_bids.Time), set(df_asks.Time)))
    best_bids = pd.DataFrame(columns=df_bids.columns)
    best_asks = pd.DataFrame(columns=df_asks.columns)
    for t in all_times:
        bids_t = df_bids.loc[df_bids.Time == t]
        asks_t = df_asks.loc[df_asks.Time == t]
        best_bid = bids_t.loc[bids_t.Price == bids_t.Price.max()]
        best_bids = pd.concat([best_bids, best_bid])
        best_ask = asks_t.loc[asks_t.Price == asks_t.Price.min()]
        best_asks = pd.concat([best_asks, best_ask])
    best_bids = best_bids.groupby('Time').first().reset_index()
    best_asks = best_asks.groupby('Time').first().reset_index()

    return best_bids, best_asks


class OrderData:

    def __init__(self, name):
        self.name = name
        self.orders = pd.read_csv(self.name)
        self.future_orders, self.etf_orders = split_future_etf(self.orders)

        # CANCELED AND NON-CANCELED ORDERS
        self.future_canceled_orders, self.future_non_canceled_order, self.future_canceled_percentage = split_canceled_non_canceled(
            self.future_orders)
        self.etf_canceled_orders, self.etf_non_canceled_order, self.etf_canceled_percentage = split_canceled_non_canceled(
            self.etf_orders)

        # BIDS & ASKS
        self.future_bids, self.future_asks = split_bids_asks(self.future_non_canceled_order)
        self.etf_bids, self.etf_asks = split_bids_asks(self.etf_non_canceled_order)

        # BEST BIDS AND ASKS
        self.future_best_bids, self.future_best_asks = get_best_bids_asks(self.future_bids, self.future_asks)
        self.etf_best_bids, self.etf_best_asks = get_best_bids_asks(self.etf_bids, self.etf_asks)

        # SPREADS
        self.future_spreads = self.future_best_asks.Price - self.future_best_bids.Price
        self.etf_spreads = self.etf_best_asks.Price - self.etf_best_bids.Price
        self.future_spreads_mean = self.future_spreads.mean()
        self.future_spreads_std = self.future_spreads.std()
        self.etf_spreads_mean = self.etf_spreads.mean()
        self.etf_spreads_std = self.etf_spreads.std()

    def plot_spread_values(self):
        fig, axes = plt.subplots(nrows=2, ncols=1, figsize=(12, 6))

        self.etf_spreads.plot(x='Time', y='Spread', ax=axes[0], label='ETF spreads')
        axes[0].set_xlabel('Time')
        axes[0].set_ylabel('Price')
        axes[0].set_title('ETF Bid-Ask Spread')
        axes[0].legend()

        self.future_spreads.plot(x='Time', y='Spread', ax=axes[1], label='Futures spreads')
        axes[1].set_xlabel('Time')
        axes[1].set_ylabel('Price')
        axes[1].set_title('Futures Bid-Ask Spread')
        axes[1].legend()

        plt.show()

    def plot_bid_ask_spreads(self):
        fig, axes = plt.subplots(nrows=2, ncols=1, figsize=(12, 6))

        self.future_best_asks.plot(x='Time', y='Price', ax=axes[0], label='Future Asks')
        self.future_best_bids.plot(x='Time', y='Price', ax=axes[0], label='Future Bids')
        axes[0].set_xlabel('Time')
        axes[0].set_ylabel('Price')
        axes[0].set_title('ETP Bid-Ask Prices')
        axes[0].legend()

        self.etf_best_asks.plot(x='Time', y='Price', ax=axes[1], label='ETP Asks')
        self.etf_best_bids.plot(x='Time', y='Price', ax=axes[1], label='ETP Bids')
        axes[1].set_xlabel('Time')
        axes[1].set_ylabel('Price')
        axes[1].set_title('ETP Bid-Ask Prices')
        axes[1].legend()

        plt.show()
