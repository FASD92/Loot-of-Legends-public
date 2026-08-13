CREATE TABLE settlement_inbox (
    settlement_id BINARY(16) NOT NULL,
    payload_hash BINARY(32) NOT NULL,
    canonical_payload MEDIUMBLOB NOT NULL,
    status VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    accepted_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    applied_at TIMESTAMP(6) NULL,
    PRIMARY KEY (settlement_id),
    CONSTRAINT chk_settlement_inbox_status
        CHECK (status IN ('ACCEPTED_PENDING', 'APPLIED'))
);
