CREATE TABLE accounts (
  account_id BIGINT PRIMARY KEY,
  display_name VARCHAR(64) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE inventories (
  account_id BIGINT NOT NULL,
  item_id BIGINT NOT NULL,
  quantity INT NOT NULL,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (account_id, item_id),
  CONSTRAINT inventories_quantity_non_negative CHECK (quantity >= 0)
);

CREATE TABLE wallets (
  account_id BIGINT PRIMARY KEY,
  gold BIGINT NOT NULL,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  CONSTRAINT wallets_gold_non_negative CHECK (gold >= 0)
);

CREATE TABLE settlement_records (
  settlement_id VARCHAR(64) PRIMARY KEY,
  account_id BIGINT NOT NULL,
  session_id BIGINT NOT NULL,
  room_id BIGINT NOT NULL,
  status VARCHAR(16) NOT NULL,
  gold_delta BIGINT NOT NULL,
  request_hash CHAR(64) NOT NULL,
  applied_at TIMESTAMP NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);
