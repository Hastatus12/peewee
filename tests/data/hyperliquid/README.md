Real Hyperliquid mainnet messages, captured on 2026-11-15 by examples/hl_order_test
(one post-only BTC buy, resting, then cancelled by cloid). The account address in
user_fills_snapshot_empty.json is replaced by a fake one.

Second run (2026-11-17): order, then MODIFY, then cancel. A modify does not move
the order: Hyperliquid creates a new oid and cancels the old one, both with our
cloid, in the same message (order_update_modified.json).

Third run (2026-11-22, examples/hl_trigger_test): a STOP-MARKET buy far above
the market (trigger_*.json: waiting trigger orders are "open", and a modify works
like for limit orders), then a MARKET buy whose worst price was below the market:
the post answers "could not immediately match" (post_market_no_match.json) AND
orderUpdates says "iocCancelRejected" (order_update_ioc_cancel_rejected.json).

Fourth run (2026-11-22, examples/hl_stop_fill_test): a STOP-MARKET buy that
really triggered (order_update_trigger_filled.json: "triggered" then "filled",
SAME oid; note limitPx becomes 67159 = trigger + 10%: Hyperliquid replaces our
worst price with its own 10% slippage), then a reduce-only MARKET sell that
filled (post_market_filled.json, order_update_market_filled.json). The two
userFills messages (user_fills_*.json) have the account address replaced by 0x1111...
