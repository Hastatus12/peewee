#pragma once
// Exchange capabilities. Algorithms adapt to these instead
// of asking "am I on Bybit or Hyperliquid?".

namespace peewee {

struct VenueCapabilities {
    bool native_amend = true;          // Modify an existing order without cancel+new.
    bool amend_keeps_priority = false; // Does a price amendment preserve queue priority? (Usually not.)
    bool ws_order_entry = true;        // Order entry over WebSocket (faster than REST).
    bool post_only = true;             // Supports post-only orders.
    bool book_checksum = false;        // The order book feed includes a checksum.
    int max_book_depth = 50;
};

}  // namespace peewee
