Real Lighter messages, captured on 2026-11-26 (public endpoints, no account):

- order_book_snapshot.json, order_book_update.json, order_book_update_next.json:
  the first three messages of order_book/4096 (BTC) on testnet. The snapshot is
  the whole book; each update's begin_nonce is the previous message's nonce.
- order_book_details.json: GET /api/v1/orderBookDetails on testnet (BTC is
  market 4096 there, market 1 on mainnet).
- apikeys.json, next_nonce.json: GET /api/v1/apikeys and /api/v1/nextNonce for
  testnet account 1, API key 0 (a public account).
- sendtx_error.json: the answer to a "jsonapi/sendtx" with a bad signature,
  sent on purpose on testnet: it shows how errors carry our request id.
- subscribe_auth_error.json: a private subscription with a bad auth token.

Not captured yet (they need an account): the answer to an ACCEPTED sendtx and
the account_all_orders / account_all_trades messages. The tests build those
from the documented schema; examples/lighter_order_test prints the real ones
(PEEWEE_DEBUG=1) to save here.
