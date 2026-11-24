# Real Bybit messages

Captured on mainnet (2026-11-08) with `PEEWEE_DEBUG=1 ./chase_live`, used by
`tests/test_bybit_codec.cpp`. The account ID in the `"id"` field of order
messages was replaced with `123456789`; everything else is unchanged.

| File | Stream | What happened |
|---|---|---|
| `private_auth_ok.json` | private | Login accepted |
| `trade_auth_ok.json` | trade | Login accepted |
| `private_subscribe_ok.json` | private | Subscribed to `order` and `execution` |
| `private_pong.json`, `trade_pong.json` | private, trade | Answers to `{"op":"ping"}` |
| `trade_create_rejected_balance.json` | trade | `order.create` refused: not enough balance (110007) |
| `order_new.json` | private | Post-only buy 0.001 BTCPERP @ 60000 entered the book |
| `trade_create_ok.json` | trade | `order.create` accepted (arrived AFTER `order_new.json`) |
| `trade_cancel_ok.json` | trade | `order.cancel` accepted |
| `order_cancelled.json` | private | The order left the book (`CancelByUser`) |
| `trade_amend_ok.json` | trade | `order.amend` accepted (chase run with limit 90000) |
| `order_amended.json` | private | The amended order, still `New`, now at 75263 |
| `execution_fill.json` | private | Filled 0.001 @ 75263 as maker (arrived BEFORE `order_filled.json`) |
| `order_filled.json` | private | The order closed as `Filled` |

Still to capture: order book snapshot/delta, partial fills.

2026-11-22, examples/bybit_order_types_test on BTCPERP: conditional (stop)
orders: "Untriggered" (waiting), "Triggered" (entering the book, same orderId),
"Filled", "Deactivated" (cancelled before the trigger); a Limit IOC that found
nothing: "Cancelled" + rejectReason "EC_NoImmediateQtyToFill". Bybit kept OUR
limit price (61210.4) for the triggered stop, unlike Hyperliquid. (Messages
re-serialized from the log with the same fields and order; account ID replaced.)
