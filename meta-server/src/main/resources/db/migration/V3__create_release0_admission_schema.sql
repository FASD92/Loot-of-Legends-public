CREATE TABLE release0_game_session_claim (
    connection_id VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    account_id BIGINT NOT NULL,
    nickname VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    reservation_expires_at_ms BIGINT NOT NULL,
    claimed_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    released_at TIMESTAMP(6) NULL,
    PRIMARY KEY (connection_id),
    KEY ix_release0_game_session_claim_account (account_id),
    CONSTRAINT fk_release0_game_session_claim_account
        FOREIGN KEY (account_id) REFERENCES player_account (account_id)
);
