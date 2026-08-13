ALTER TABLE settlement_inbox
    ADD COLUMN account_id BINARY(16) NULL AFTER settlement_id;

UPDATE settlement_inbox
SET account_id = SUBSTRING(canonical_payload, 37, 16)
WHERE account_id IS NULL;

ALTER TABLE settlement_inbox
    MODIFY COLUMN account_id BINARY(16) NOT NULL,
    ADD INDEX idx_settlement_inbox_account_status (account_id, status);

CREATE TABLE item_catalog (
    catalog_version SMALLINT UNSIGNED NOT NULL,
    item_id BIGINT UNSIGNED NOT NULL,
    unit_value BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (catalog_version, item_id),
    CONSTRAINT chk_item_catalog_value CHECK (unit_value > 0)
);

INSERT INTO item_catalog (catalog_version, item_id, unit_value)
VALUES (1, 1, 100), (1, 2, 300);

CREATE TABLE account_inventory (
    account_id BINARY(16) NOT NULL,
    item_id BIGINT UNSIGNED NOT NULL,
    quantity BIGINT UNSIGNED NOT NULL,
    unit_value BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (account_id, item_id),
    CONSTRAINT chk_account_inventory_quantity CHECK (quantity > 0),
    CONSTRAINT chk_account_inventory_value CHECK (unit_value > 0)
);

CREATE TABLE account_wallet (
    account_id BINARY(16) NOT NULL,
    balance BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (account_id)
);
